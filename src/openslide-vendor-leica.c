/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
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
 * quickhash comes from _openslide_tiff_init_properties_and_hash
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

struct leica_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

static void destroy_data(struct leica_ops_data *data,
                         struct level **levels, int32_t level_count) {
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct leica_ops_data, data);

  for (int32_t i = 0; i < level_count; i++) {
    _openslide_grid_destroy(levels[i]->grid);
    g_slice_free(struct level, levels[i]);
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  struct leica_ops_data *data = osr->data;
  struct level **levels = (struct level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}

static void read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      struct _openslide_grid *grid,
                      int64_t tile_col, int64_t tile_row,
                      void *arg) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            tile_col, tile_row, grid,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    _openslide_tiff_read_tile(osr, tiffl, tiff, tiledata, tile_col, tile_row);

    // clip, if necessary
    _openslide_tiff_clip_tile(osr, tiffl, tiledata, tile_col, tile_row);

    // put it in the cache
    _openslide_cache_put(osr->cache, tile_col, tile_row, grid,
			 tiledata, tw * th * 4,
			 &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 tw, th,
                                                                 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  //_openslide_grid_label_tile(grid, cr, tile_col, tile_row);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);
}

static void paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 struct _openslide_level *level,
			 int32_t w, int32_t h) {
  struct leica_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  TIFF *tiff = _openslide_tiffcache_get(data->tc);
  if (tiff) {
    if (TIFFSetDirectory(tiff, l->tiffl.dir)) {
      _openslide_grid_paint_region(l->grid, cr, tiff,
                                   x / l->base.downsample,
                                   y / l->base.downsample,
                                   level, w, h);
    } else {
      _openslide_set_error(osr, "Cannot set TIFF directory");
    }
  } else {
    _openslide_set_error(osr, "Cannot open TIFF file");
  }
  _openslide_tiffcache_put(data->tc, tiff);
}

static const struct _openslide_ops leica_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static int width_compare(const void *a, const void *b) {
  const struct level *la = *(const struct level **) a;
  const struct level *lb = *(const struct level **) b;

  if (la->tiffl.image_w > lb->tiffl.image_w) {
    return -1;
  } else if (la->tiffl.image_w == lb->tiffl.image_w) {
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
                                  GPtrArray *main_image_levels,
                                  int *out_macro_ifd,
                                  GError **err) {
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

  bool success = false;

  int i;

  *out_macro_ifd = -1;

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

    int64_t dir;
    PARSE_INT_ATTRIBUTE_OR_FAIL(result->nodesetval->nodeTab[i],
                                LEICA_ATTR_IFD, dir);

    struct level *l = g_slice_new0(struct level);
    l->tiffl.dir = dir;

    g_ptr_array_add(main_image_levels, l);
  }

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

bool _openslide_try_leica(openslide_t *osr,
                          struct _openslide_tiffcache *tc, TIFF *tiff,
                          struct _openslide_hash *quickhash1,
                          GError **err) {
  GPtrArray *level_array = g_ptr_array_new();
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

  if (!parse_xml_description(tagval, osr, level_array, &macroIFD, err)) {
    // unrecognizable xml
    goto FAIL;
  }

  // add macro image if found
  if (macroIFD != -1) {
    if (!check_directory(tiff, macroIFD, err)) {
      goto FAIL;
    }
    if (!_openslide_tiff_add_associated_image(osr, "macro",
                                              tc, macroIFD, err)) {
      goto FAIL;
    }
  }

  // initialize and verify levels
  for (uint32_t n = 0; n < level_array->len; n++) {
    struct level *l = level_array->pdata[n];
    struct _openslide_tiff_level *tiffl = &l->tiffl;

    if (!check_directory(tiff, l->tiffl.dir, err)) {
      goto FAIL;
    }

    if (!_openslide_tiff_level_init(tiff,
                                    l->tiffl.dir,
                                    (struct _openslide_level *) l,
                                    tiffl,
                                    err)) {
      goto FAIL;
    }

    l->grid = _openslide_grid_create_simple(osr,
                                            tiffl->tiles_across,
                                            tiffl->tiles_down,
                                            tiffl->tile_w,
                                            tiffl->tile_h,
                                            read_tile);
  }

  // sort levels
  g_ptr_array_sort(level_array, width_compare);

  // unwrap level array
  int32_t level_count = level_array->len;
  g_assert(level_count > 0);
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // allocate private data
  struct leica_ops_data *data = g_slice_new0(struct leica_ops_data);

  if (osr == NULL) {
    // free now and return
    _openslide_tiffcache_put(tc, tiff);
    data->tc = tc;
    destroy_data(data, levels, level_count);
    return true;
  }

  // set MPP properties
  if (!TIFFSetDirectory(tiff, levels[0]->tiffl.dir)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read directory");
    destroy_data(data, levels, level_count);
    return false;
  }
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_X,
                      TIFFTAG_XRESOLUTION);
  set_resolution_prop(osr, tiff, OPENSLIDE_PROPERTY_NAME_MPP_Y,
                      TIFFTAG_YRESOLUTION);

  // set hash and properties
  if (!_openslide_tiff_init_properties_and_hash(osr, tiff, quickhash1,
                                                levels[level_count - 1]->tiffl.dir,
                                                levels[0]->tiffl.dir,
                                                err)) {
    destroy_data(data, levels, level_count);
    return false;
  }
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
                      g_strdup("leica"));

  // keep the XML document out of the properties
  // (in case pyramid level 0 is also directory 0)
  g_hash_table_remove(osr->properties, OPENSLIDE_PROPERTY_NAME_COMMENT);
  g_hash_table_remove(osr->properties, "tiff.ImageDescription");

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &leica_ops;

  // put TIFF handle and assume tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  return true;

FAIL:
  // free the level array
  if (level_array) {
    for (uint32_t n = 0; n < level_array->len; n++) {
      struct level *l = level_array->pdata[n];
      _openslide_grid_destroy(l->grid);
      g_slice_free(struct level, l);
    }
    g_ptr_array_free(level_array, true);
  }

  return false;
}
