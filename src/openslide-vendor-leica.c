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
#include "openslide-decode-tiff.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

static const char LEICA_XMLNS[] = "http://www.leica-microsystems.com/scn/2010/10/01";
static const char LEICA_ATTR_SIZE_X[] = "sizeX";
static const char LEICA_ATTR_SIZE_Y[] = "sizeY";
static const char LEICA_ATTR_IFD[] = "ifd";
static const char LEICA_ATTR_Z_PLANE[] = "z";

#define PARSE_INT_ATTRIBUTE_OR_FAIL(NODE, NAME, OUT)		\
  do {								\
    GError *tmp_err = NULL;					\
    OUT = _openslide_xml_parse_int_attr(NODE, NAME, &tmp_err);	\
    if (tmp_err)  {						\
      g_propagate_error(err, tmp_err);				\
      goto FAIL;						\
    }								\
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

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *arg,
                      GError **err) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    if (!_openslide_tiff_read_tile(tiffl, tiff,
                                   tiledata, tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, tiledata,
                                   tile_col, tile_row,
                                   err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // put it in the cache
    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
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

  //_openslide_grid_label_tile(l->grid, cr, tile_col, tile_row);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 struct _openslide_level *level,
			 int32_t w, int32_t h,
			 GError **err) {
  struct leica_ops_data *data = osr->data;
  struct level *l = (struct level *) level;
  bool success = false;

  TIFF *tiff = _openslide_tiffcache_get(data->tc, err);
  if (tiff == NULL) {
    return false;
  }

  if (TIFFSetDirectory(tiff, l->tiffl.dir)) {
    success = _openslide_grid_paint_region(l->grid, cr, tiff,
                                           x / l->base.downsample,
                                           y / l->base.downsample,
                                           level, w, h,
                                           err);
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot set TIFF directory");
  }
  _openslide_tiffcache_put(data->tc, tiff);

  return success;
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
                                  int *macro_ifd,
                                  GError **err) {
  xmlDoc *doc = NULL;
  xmlNode *collection;

  xmlNode *main_image = NULL;
  xmlNode *macro_image = NULL;

  xmlNode *image;

  xmlXPathContext *context = NULL;
  xmlXPathObject *images_result = NULL;
  xmlXPathObject *result = NULL;

  int64_t collection_width;
  int64_t collection_height;

  int64_t macro_width = 0;
  int64_t macro_height = 0;

  int64_t test_width;
  int64_t test_height;
  int64_t test_ifd;

  bool success = false;

  int i;

  GError *tmp_err = NULL;

  *macro_ifd = -1;

  // try to parse the xml
  doc = _openslide_xml_parse(xml, &tmp_err);
  if (doc == NULL) {
    // not leica
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "%s", tmp_err->message);
    g_clear_error(&tmp_err);
    goto FAIL;
  }

  if (!_openslide_xml_has_default_namespace(doc, LEICA_XMLNS)) {
    // not leica
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Unexpected XML namespace");
    goto FAIL;
  }

  // create XPATH context to query the document
  context = _openslide_xml_xpath_create(doc);

  // the recognizable structure is the following:
  /*
    scn (root node)
      collection
        barcode
        image
        image
  */

  result = _openslide_xml_xpath_eval(context, "/d:scn/d:collection");
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
  _openslide_xml_set_prop_from_xpath(osr, context, "leica.barcode",
                                     "/d:scn/d:collection/d:barcode/text()");

  // read collection's size
  PARSE_INT_ATTRIBUTE_OR_FAIL(collection, LEICA_ATTR_SIZE_X, collection_width);
  PARSE_INT_ATTRIBUTE_OR_FAIL(collection, LEICA_ATTR_SIZE_Y, collection_height);

  // get the image nodes
  context->node = collection;
  images_result = _openslide_xml_xpath_eval(context, "d:image");
  if (!images_result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find any images");
    goto FAIL;
  }

  // loop through all image nodes to find the main image and the macro
  for (i = 0; i < images_result->nodesetval->nodeNr; i++) {
    image = images_result->nodesetval->nodeTab[i];

    context->node = image;
    result = _openslide_xml_xpath_eval(context, "d:view");

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
  result = _openslide_xml_xpath_eval(context, "d:pixels/d:dimension");

  if (!result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find any dimensions in the main image");
    goto FAIL;
  }

  // add all the IFDs of the main image to the level list
  for (i = 0; i < result->nodesetval->nodeNr; i++) {
    xmlChar *z = xmlGetProp(result->nodesetval->nodeTab[i],
                            BAD_CAST LEICA_ATTR_Z_PLANE);
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
  _openslide_xml_set_prop_from_xpath(osr, context, "leica.device-model",
                                     "d:device/@model");
  _openslide_xml_set_prop_from_xpath(osr, context, "leica.device-version",
                                     "d:device/@version");
  _openslide_xml_set_prop_from_xpath(osr, context, "leica.creation-date",
                                     "d:creationDate/text()");
  _openslide_xml_set_prop_from_xpath(osr, context, "leica.objective",
                                     "d:scanSettings/d:objectiveSettings/d:objective/text()");
  _openslide_xml_set_prop_from_xpath(osr, context, "leica.aperture",
                                     "d:scanSettings/d:illuminationSettings/d:numericalAperture/text()");
  _openslide_xml_set_prop_from_xpath(osr, context, "leica.illumination-source",
                                     "d:scanSettings/d:illuminationSettings/d:illuminationSource/text()");

  // copy objective to standard property
  if (osr) {
    _openslide_duplicate_int_prop(osr->properties, "leica.objective",
                                  OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  }

  // process macro image
  if (macro_image != NULL) {
    context->node = macro_image;
    result = _openslide_xml_xpath_eval(context, "d:pixels/d:dimension");

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
        *macro_ifd = test_ifd;
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
