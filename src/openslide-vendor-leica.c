/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

/*
 * LEICA (scn) BigTIFF support
 *
 * quickhash comes from what the TIFF backend does
 *
 */

#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

static const xmlChar LEICA_XMLNS[] = "http://www.leica-microsystems.com/scn/2010/10/01";
static const xmlChar LEICA_ATTR_SIZE_X[] = "sizeX";
static const xmlChar LEICA_ATTR_SIZE_Y[] = "sizeY";
static const xmlChar LEICA_ATTR_IFD[] = "ifd";
static const xmlChar LEICA_ATTR_Z_PLANE[] = "z";

#define PARSE_INT_ATTRIBUTE_OR_FAIL(NODE, NAME, OUT)	\
  do {							\
    if (!parse_int_attr(NODE, NAME, &OUT, err))  {	\
      goto FAIL;					\
    }							\
  } while (0)

struct level {
  int64_t directory_number;
  int64_t width;
};

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct level *la = a;
  const struct level *lb = b;

  if (la->width > lb->width) {
    return -1;
  } else if (la->width == lb->width) {
    return 0;
  } else {
    return 1;
  }
}

static bool parse_int_attr(xmlNodePtr node, const xmlChar *name,
                           int64_t *out, GError **err) {
  xmlChar *value = xmlGetProp(node, name);
  int64_t result;
  gchar *endptr;

  if (value == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "No integer attribute \"%s\"", name);
    return false;
  }

  result = g_ascii_strtoll((gchar *) value, &endptr, 10);
  if (value[0] == 0 || endptr[0] != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid integer attribute \"%s\"", name);
    xmlFree(value);
    return false;
  }

  xmlFree(value);
  *out = result;
  return true;
}

// returns NULL if no matches
static xmlXPathObjectPtr eval_xpath(const char *xpath,
                                    xmlXPathContextPtr context) {
  xmlXPathObjectPtr result;

  result = xmlXPathEvalExpression(BAD_CAST xpath, context);
  if (result && (result->nodesetval == NULL ||
                 result->nodesetval->nodeNr == 0)) {
    xmlXPathFreeObject(result);
    result = NULL;
  }
  return result;
}

static void set_prop_from_content(openslide_t *osr,
                                  const char *property_name,
                                  const char *xpath,
                                  xmlXPathContextPtr context) {
  xmlXPathObjectPtr result;

  result = eval_xpath(xpath, context);
  if (result) {
    xmlChar *str = xmlNodeGetContent(result->nodesetval->nodeTab[0]);
    if (osr && str) {
      g_hash_table_insert(osr->properties,
                          g_strdup(property_name),
                          g_strdup((char *) str));
    }
    xmlFree(str);
  }
  xmlXPathFreeObject(result);
}

static void set_prop_from_attribute(openslide_t *osr,
                                    const char *property_name,
                                    const char *xpath,
                                    const char *attribute_name,
                                    xmlXPathContextPtr context) {
  xmlXPathObjectPtr result;

  result = eval_xpath(xpath, context);
  if (result) {
    xmlChar *str = xmlGetProp(result->nodesetval->nodeTab[0],
                              BAD_CAST attribute_name);
    if (osr && str) {
      g_hash_table_insert(osr->properties,
                          g_strdup(property_name),
                          g_strdup((char *) str));
    }
    xmlFree(str);
  }
  xmlXPathFreeObject(result);
}

static void set_resolution_prop(openslide_t *osr, TIFF *tiff,
                                const char *property_name,
                                ttag_t tag) {
  float f;
  uint16_t unit;

  if (TIFFGetFieldDefaulted(tiff, TIFFTAG_RESOLUTIONUNIT, &unit) &&
      TIFFGetField(tiff, tag, &f) &&
      osr &&
      unit == RESUNIT_CENTIMETER) {
    g_hash_table_insert(osr->properties, g_strdup(property_name),
                        _openslide_format_double(10000.0 / f));
  }
}

static bool parse_xml_description(const char *xml, openslide_t *osr, 
                                  int *out_macro_ifd,
                                  GList **out_main_image_ifds,
                                  int *level_count, GError **err) {
  xmlDocPtr doc = NULL;
  xmlNode *root_element;
  xmlNode *collection;

  xmlNode *main_image = NULL;
  xmlNode *macro_image = NULL;

  xmlNode *image;

  xmlXPathContextPtr context = NULL;
  xmlXPathObjectPtr images_result = NULL;
  xmlXPathObjectPtr result = NULL;

  int64_t collection_width;
  int64_t collection_height;

  int64_t macro_width = 0;
  int64_t macro_height = 0;

  int64_t test_width;
  int64_t test_height;
  int64_t test_ifd;

  struct level *l = NULL;

  bool success = false;

  int i;

  *out_macro_ifd = -1;
  *level_count = 0;

  // try to parse the xml
  doc = xmlReadMemory(xml, strlen(xml), "/", NULL, XML_PARSE_NOERROR |
                      XML_PARSE_NOWARNING | XML_PARSE_NONET);
  if (doc == NULL) {
    // not leica
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Could not parse XML");
    goto FAIL;
  }

  root_element = xmlDocGetRootElement(doc);
  if (xmlStrcmp(root_element->ns->href, LEICA_XMLNS) != 0) {
    // not leica
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Unexpected XML namespace");
    goto FAIL;
  }

  // create XPATH context to query the document
  context = xmlXPathNewContext(doc);
  if (context == NULL) {
    // allocation error, abort
    g_error("xmlXPathNewContext failed");
    // not reached
  }

  // register the document's NS to a shorter name
  xmlXPathRegisterNs(context, BAD_CAST "l", root_element->ns->href);

  // the recognizable structure is the following:
  /*
    scn (root node)
      collection
        barcode
        image
        image
  */

  result = eval_xpath("/l:scn/l:collection", context);
  // the root node should only have one child, named collection, otherwise fail
  if (result == NULL || result->nodesetval->nodeNr != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find collection element");
    goto FAIL;
  }

  collection = result->nodesetval->nodeTab[0];
  xmlXPathFreeObject(result);
  result = NULL;

  // read barcode
  set_prop_from_content(osr, "leica.barcode",
                        "/l:scn/l:collection/l:barcode", context);

  // read collection's size
  PARSE_INT_ATTRIBUTE_OR_FAIL(collection, LEICA_ATTR_SIZE_X, collection_width);
  PARSE_INT_ATTRIBUTE_OR_FAIL(collection, LEICA_ATTR_SIZE_Y, collection_height);

  // get the image nodes
  context->node = collection;
  images_result = eval_xpath("l:image", context);
  if (!images_result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find any images");
    goto FAIL;
  }

  // loop through all image nodes to find the main image and the macro
  for (i = 0; i < images_result->nodesetval->nodeNr; i++) {
    image = images_result->nodesetval->nodeTab[i];

    context->node = image;
    result = eval_xpath("l:view", context);

    if (result == NULL || result->nodesetval->nodeNr != 1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't find view node");
      goto FAIL;
    }

    PARSE_INT_ATTRIBUTE_OR_FAIL(result->nodesetval->nodeTab[0],
                                LEICA_ATTR_SIZE_X, test_width);
    PARSE_INT_ATTRIBUTE_OR_FAIL(result->nodesetval->nodeTab[0],
                                LEICA_ATTR_SIZE_Y, test_height);

    xmlXPathFreeObject(result);
    result = NULL;

    // we assume that the macro's dimensions are the same as the collection's
    if (test_width == collection_width && test_height == collection_height) {
      if (macro_image != NULL) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Found multiple macro images");
        goto FAIL;
      }
      macro_image = image;
    } else {
      if (main_image != NULL) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Found multiple main images");
        goto FAIL;
      }
      main_image = image;
    }
  }

  if (main_image == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find main image node");
    goto FAIL;
  }

  context->node = main_image;
  result = eval_xpath("l:pixels/l:dimension", context);

  if (!result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find any dimensions in the main image");
    goto FAIL;
  }

  // add all the IFDs of the main image to the level list
  for (i = 0; i < result->nodesetval->nodeNr; i++) {
    xmlChar *z = xmlGetProp(result->nodesetval->nodeTab[i],
                            LEICA_ATTR_Z_PLANE);
    if (z && strcmp((char *) z, "0")) {
      // accept only IFDs from z-plane 0
      // TODO: support multiple z-planes
      xmlFree(z);
      continue;
    }
    xmlFree(z);

    l = g_slice_new(struct level);

    PARSE_INT_ATTRIBUTE_OR_FAIL(result->nodesetval->nodeTab[i],
                                LEICA_ATTR_SIZE_X, l->width);
    PARSE_INT_ATTRIBUTE_OR_FAIL(result->nodesetval->nodeTab[i],
                                LEICA_ATTR_IFD, l->directory_number);

    *out_main_image_ifds = g_list_prepend(*out_main_image_ifds, l);
    ++*level_count;
  }

  l = NULL;

  xmlXPathFreeObject(result);
  result = NULL;

  // add some more properties from the main image
  set_prop_from_attribute(osr, "leica.device-model",
                          "l:device", "model",
                          context);
  set_prop_from_attribute(osr, "leica.device-version",
                          "l:device", "version",
                          context);
  set_prop_from_content(osr, "leica.creation-date",
                        "l:creationDate",
                        context);
  set_prop_from_content(osr, "leica.objective",
                        "l:scanSettings/l:objectiveSettings/l:objective",
                        context);
  set_prop_from_content(osr, "leica.aperture",
                        "l:scanSettings/l:illuminationSettings/l:numericalAperture",
                        context);
  set_prop_from_content(osr, "leica.illumination-source",
                        "l:scanSettings/l:illuminationSettings/l:illuminationSource",
                        context);

  // copy objective to standard property
  if (osr) {
    _openslide_duplicate_int_prop(osr->properties, "leica.objective",
                                  OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  }

  // process macro image
  if (macro_image != NULL) {
    context->node = macro_image;
    result = eval_xpath("l:pixels/l:dimension", context);

    if (!result) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't find any dimensions in the macro image");
      goto FAIL;
    }

    for (i = 0; i < result->nodesetval->nodeNr; i++) {
      PARSE_INT_ATTRIBUTE_OR_FAIL(result->nodesetval->nodeTab[i],
                                  LEICA_ATTR_SIZE_X, test_width);
      PARSE_INT_ATTRIBUTE_OR_FAIL(result->nodesetval->nodeTab[i],
                                  LEICA_ATTR_SIZE_Y, test_height);
      PARSE_INT_ATTRIBUTE_OR_FAIL(result->nodesetval->nodeTab[i],
                                  LEICA_ATTR_IFD, test_ifd);

      if (test_width >= macro_width && test_height >= macro_height) {
        macro_width = test_width;
        macro_height = test_height;
        *out_macro_ifd = test_ifd;
      }
    }

    xmlXPathFreeObject(result);
    result = NULL;
  }

  success = true;

FAIL:
  g_slice_free(struct level, l);

  xmlXPathFreeObject(result);
  xmlXPathFreeObject(images_result);
  xmlXPathFreeContext(context);
  if (doc != NULL) {
    xmlFreeDoc(doc);
  }

  return success;
}

static bool check_directory(TIFF *tiff, uint16 dir_num, GError **err) {
  if (TIFFSetDirectory(tiff, dir_num) == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find directory");
    return false;
  }

  // verify that we can read this compression (hard fail if not)
  uint16_t compression;
  if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read compression scheme");
    return false;
  }

  if (!TIFFIsCODECConfigured(compression)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Unsupported TIFF compression: %u", compression);
    return false;
  }

  return true;
}

bool _openslide_try_leica(openslide_t *osr, TIFF *tiff, 
                          struct _openslide_hash *quickhash1,
                          GError **err) {
  GList *level_list = NULL;
  int32_t level_count;
  int32_t *levels = NULL;
  char *tagval;
  int tiff_result;

  int macroIFD;  // which IFD contains the macro image

  if (!TIFFIsTiled(tiff)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "TIFF is not tiled");
    goto FAIL;
  }

  // get the xml description
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);

  // check if it containes the XML namespace string before we invoke
  // the parser
  if (!tiff_result || (strstr(tagval, (const char *) LEICA_XMLNS) == NULL)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a Leica slide");
    goto FAIL;
  }

  if (!parse_xml_description(tagval, osr, &macroIFD, &level_list,
                             &level_count, err)) {
    // unrecognizable xml
    goto FAIL;
  }

  if (osr) {
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
                        g_strdup("leica"));
  }

  // add macro image if found
  if (macroIFD != -1) {
    if (!check_directory(tiff, macroIFD, err)) {
      goto FAIL;
    }
    if (!_openslide_add_tiff_associated_image(osr ? osr->associated_images : NULL,
                                              "macro", tiff, err)) {
      goto FAIL;
    }
  }

  // sort tiled levels
  level_list = g_list_sort(level_list, width_compare);

  // copy levels in, while deleting the list
  levels = g_new(int32_t, level_count);
  for (int i = 0; i < level_count; i++) {
    struct level *l = level_list->data;
    if (!check_directory(tiff, l->directory_number, err)) {
      goto FAIL;
    }
    level_list = g_list_delete_link(level_list, level_list);

    levels[i] = l->directory_number;
    g_slice_free(struct level, l);
  }

  g_assert(level_list == NULL);
  g_assert(level_count > 0);

  // set MPP properties
  if (!TIFFSetDirectory(tiff, levels[0])) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read directory");
    goto FAIL;
  }
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_X,
                      TIFFTAG_XRESOLUTION);
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_Y,
                      TIFFTAG_YRESOLUTION);

  // all set, load up the TIFF-specific ops
  _openslide_add_tiff_ops(osr, tiff, levels[0],
    level_count, levels,
    quickhash1);

  // keep the XML document out of the properties
  // (in case pyramid level 0 is also directory 0)
  if (osr) {
    g_hash_table_remove(osr->properties, OPENSLIDE_PROPERTY_NAME_COMMENT);
    g_hash_table_remove(osr->properties, "tiff.ImageDescription");
  }

  return true;

FAIL:
  // free the level list
  for (GList *i = level_list; i != NULL; i = g_list_delete_link(i, i)) {
    g_slice_free(struct level, i->data);
  }

  g_free(levels);

  return false;
}
