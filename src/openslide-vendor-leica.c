/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
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
 * quickhash comes from _openslide_tifflike_init_properties_and_hash
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"
#include "openslide-decode-xml.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <tiffio.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

static const char LEICA_XMLNS_1[] = "http://www.leica-microsystems.com/scn/2010/03/10";
static const char LEICA_XMLNS_2[] = "http://www.leica-microsystems.com/scn/2010/10/01";
static const char LEICA_ATTR_SIZE_X[] = "sizeX";
static const char LEICA_ATTR_SIZE_Y[] = "sizeY";
static const char LEICA_ATTR_OFFSET_X[] = "offsetX";
static const char LEICA_ATTR_OFFSET_Y[] = "offsetY";
static const char LEICA_ATTR_IFD[] = "ifd";
static const char LEICA_ATTR_Z_PLANE[] = "z";
static const char LEICA_VALUE_BRIGHTFIELD[] = "brightfield";

#define PARSE_INT_ATTRIBUTE_OR_RETURN(NODE, NAME, OUT, RET)	\
  do {								\
    GError *tmp_err = NULL;					\
    OUT = _openslide_xml_parse_int_attr(NODE, NAME, &tmp_err);	\
    if (tmp_err)  {						\
      g_propagate_error(err, tmp_err);				\
      return RET;						\
    }								\
  } while (0)

struct leica_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  double nm_per_pixel;
  GPtrArray *areas;
};

// a TIFF directory within a level
struct area {
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;

  int64_t offset_x;
  int64_t offset_y;
};

struct read_tile_args {
  TIFF *tiff;
  struct area *area;
};

/* structs representing data parsed from ImageDescription XML */
struct collection {
  char *barcode;

  int64_t nm_across;
  int64_t nm_down;

  GPtrArray *images;
};

struct image {
  char *creation_date;
  char *device_model;
  char *device_version;
  char *illumination_source;

  // doubles, but not parsed
  char *objective;
  char *aperture;

  bool is_macro;
  int64_t nm_across;
  int64_t nm_down;
  int64_t nm_offset_x;
  int64_t nm_offset_y;

  GPtrArray *dimensions;
};

struct dimension {
  int64_t dir;
  int64_t width;
  int64_t height;
  double nm_per_pixel;
};

static void destroy_area(struct area *area) {
  _openslide_grid_destroy(area->grid);
  g_slice_free(struct area, area);
}

static void destroy_level(struct level *l) {
  g_ptr_array_free(l->areas, true);
  g_slice_free(struct level, l);
}

static void destroy(openslide_t *osr) {
  struct leica_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct leica_ops_data, data);

  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *) osr->levels[i]);
  }
  g_free(osr->levels);
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level G_GNUC_UNUSED,
                      int64_t tile_col, int64_t tile_row,
                      void *arg,
                      GError **err) {
  struct read_tile_args *args = arg;
  struct _openslide_tiff_level *tiffl = &args->area->tiffl;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            args->area, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    g_auto(_openslide_slice) box = _openslide_slice_alloc(tw * th * 4);
    if (!_openslide_tiff_read_tile(tiffl, args->tiff,
                                   box.p, tile_col, tile_row,
                                   err)) {
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, box.p,
                                   tile_col, tile_row,
                                   err)) {
      return false;
    }

    // put it in the cache
    tiledata = _openslide_slice_steal(&box);
    _openslide_cache_put(osr->cache,
			 args->area, tile_col, tile_row,
			 tiledata, tw * th * 4,
			 &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_ARGB32,
                                        tw, th, tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 struct _openslide_level *level,
			 int32_t w, int32_t h,
			 GError **err) {
  struct leica_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(data->tc, err);
  if (ct.tiff == NULL) {
    return false;
  }

  for (uint32_t n = 0; n < l->areas->len; n++) {
    struct area *area = l->areas->pdata[n];

    struct read_tile_args args = {
      .tiff = ct.tiff,
      .area = area,
    };
    int64_t ax = x / l->base.downsample - area->offset_x;
    int64_t ay = y / l->base.downsample - area->offset_y;
    if (!_openslide_grid_paint_region(area->grid, cr, &args,
                                      ax, ay, level, w, h,
                                      err)) {
      return false;
    }
  }

  return true;
}

static const struct _openslide_ops leica_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool leica_detect(const char *filename G_GNUC_UNUSED,
                         struct _openslide_tifflike *tl, GError **err) {
  // ensure we have a TIFF
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // ensure TIFF is tiled
  if (!_openslide_tifflike_is_tiled(tl, 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFF is not tiled");
    return false;
  }

  // read XML description; check that it contains the XML namespace string
  // before we invoke the parser
  const char *image_desc = _openslide_tifflike_get_buffer(tl, 0,
                                                          TIFFTAG_IMAGEDESCRIPTION,
                                                          err);
  if (!image_desc) {
    return false;
  }
  if (!strstr(image_desc, LEICA_XMLNS_1) &&
      !strstr(image_desc, LEICA_XMLNS_2)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a Leica slide");
    return false;
  }

  // try to parse the xml
  g_autoptr(xmlDoc) doc = _openslide_xml_parse(image_desc, err);
  if (doc == NULL) {
    return false;
  }

  // check default namespace
  if (!_openslide_xml_has_default_namespace(doc, LEICA_XMLNS_1) &&
      !_openslide_xml_has_default_namespace(doc, LEICA_XMLNS_2)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected XML namespace");
    return false;
  }

  return true;
}

static void dimension_free(struct dimension *dimension) {
  g_slice_free(struct dimension, dimension);
}

static void image_free(struct image *image) {
  g_ptr_array_free(image->dimensions, true);
  g_free(image->creation_date);
  g_free(image->device_model);
  g_free(image->device_version);
  g_free(image->illumination_source);
  g_free(image->objective);
  g_free(image->aperture);
  g_slice_free(struct image, image);
}

static void collection_free(struct collection *collection) {
  g_ptr_array_free(collection->images, true);
  g_free(collection->barcode);
  g_slice_free(struct collection, collection);
}

typedef struct collection collection;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(collection, collection_free)

static int dimension_compare(const void *a, const void *b) {
  const struct dimension *da = *(const struct dimension **) a;
  const struct dimension *db = *(const struct dimension **) b;

  if (da->width > db->width) {
    return -1;
  } else if (da->width == db->width) {
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
      unit == RESUNIT_CENTIMETER) {
    g_hash_table_insert(osr->properties, g_strdup(property_name),
                        _openslide_format_double(10000.0 / f));
  }
}

static void set_region_bounds_props(openslide_t *osr,
                                    struct level *level0) {
  int64_t x0 = INT64_MAX;
  int64_t y0 = INT64_MAX;
  int64_t x1 = INT64_MIN;
  int64_t y1 = INT64_MIN;

  g_assert(level0->areas->len);
  for (uint32_t n = 0; n < level0->areas->len; n++) {
    struct area *area = level0->areas->pdata[n];
    g_hash_table_insert(osr->properties,
                        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_X, n),
                        g_strdup_printf("%"PRId64, area->offset_x));
    g_hash_table_insert(osr->properties,
                        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_Y, n),
                        g_strdup_printf("%"PRId64, area->offset_y));
    g_hash_table_insert(osr->properties,
                        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_WIDTH, n),
                        g_strdup_printf("%"PRId64, area->tiffl.image_w));
    g_hash_table_insert(osr->properties,
                        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_HEIGHT, n),
                        g_strdup_printf("%"PRId64, area->tiffl.image_h));
    x0 = MIN(x0, area->offset_x);
    y0 = MIN(y0, area->offset_y);
    x1 = MAX(x1, area->offset_x + area->tiffl.image_w);
    y1 = MAX(y1, area->offset_y + area->tiffl.image_h);
  }

  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_BOUNDS_X),
                      g_strdup_printf("%"PRId64, x0));
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_BOUNDS_Y),
                      g_strdup_printf("%"PRId64, y0));
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_BOUNDS_WIDTH),
                      g_strdup_printf("%"PRId64, x1 - x0));
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_BOUNDS_HEIGHT),
                      g_strdup_printf("%"PRId64, y1 - y0));
}

static struct collection *parse_xml_description(const char *xml,
                                                GError **err) {
  // parse the xml
  g_autoptr(xmlDoc) doc = _openslide_xml_parse(xml, err);
  if (doc == NULL) {
    return NULL;
  }

  // create XPATH context to query the document
  g_autoptr(xmlXPathContext) ctx = _openslide_xml_xpath_create(doc);

  // the recognizable structure is the following:
  /*
    scn (root node)
      collection
        barcode		(2010/10/01 namespace only)
        image
          dimension
          dimension
        image
          dimension
          dimension
  */

  // get collection node
  xmlNode *collection_node = _openslide_xml_xpath_get_node(ctx,
                                                           "/d:scn/d:collection");
  if (!collection_node) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't find collection element");
    return NULL;
  }

  // create collection struct
  g_autoptr(collection) collection = g_slice_new0(struct collection);
  collection->images =
    g_ptr_array_new_with_free_func((GDestroyNotify) image_free);

  // Get barcode as stored in 2010/10/01 namespace
  g_autofree char *barcode =
    _openslide_xml_xpath_get_string(ctx, "/d:scn/d:collection/d:barcode/text()");
  if (barcode) {
    // Decode Base64
    gsize len;
    void *decoded = g_base64_decode(barcode, &len);
    // null-terminate
    collection->barcode = g_realloc(decoded, len + 1);
    collection->barcode[len] = 0;
  } else {
    // Fall back to 2010/03/10 namespace.  It's not clear whether this
    // namespace also Base64-encodes the barcode, so we avoid performing
    // a transformation that may not be correct.
    collection->barcode = _openslide_xml_xpath_get_string(ctx, "/d:scn/d:collection/@barcode");
  }

  PARSE_INT_ATTRIBUTE_OR_RETURN(collection_node, LEICA_ATTR_SIZE_X,
                                collection->nm_across, NULL);
  PARSE_INT_ATTRIBUTE_OR_RETURN(collection_node, LEICA_ATTR_SIZE_Y,
                                collection->nm_down, NULL);

  // get the image nodes
  ctx->node = collection_node;
  g_autoptr(xmlXPathObject) images_result =
    _openslide_xml_xpath_eval(ctx, "d:image");
  if (!images_result) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't find any images");
    return NULL;
  }

  // create image structs
  for (int i = 0; i < images_result->nodesetval->nodeNr; i++) {
    xmlNode *image_node = images_result->nodesetval->nodeTab[i];
    ctx->node = image_node;

    // get view node
    xmlNode *view = _openslide_xml_xpath_get_node(ctx, "d:view");
    if (!view) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't find view node");
      return NULL;
    }

    // create image struct
    struct image *image = g_slice_new0(struct image);
    image->dimensions =
      g_ptr_array_new_with_free_func((GDestroyNotify) dimension_free);
    g_ptr_array_add(collection->images, image);

    image->creation_date = _openslide_xml_xpath_get_string(ctx, "d:creationDate/text()");
    image->device_model = _openslide_xml_xpath_get_string(ctx, "d:device/@model");
    image->device_version = _openslide_xml_xpath_get_string(ctx, "d:device/@version");
    image->illumination_source = _openslide_xml_xpath_get_string(ctx, "d:scanSettings/d:illuminationSettings/d:illuminationSource/text()");
    image->objective = _openslide_xml_xpath_get_string(ctx, "d:scanSettings/d:objectiveSettings/d:objective/text()");
    image->aperture = _openslide_xml_xpath_get_string(ctx, "d:scanSettings/d:illuminationSettings/d:numericalAperture/text()");

    PARSE_INT_ATTRIBUTE_OR_RETURN(view, LEICA_ATTR_SIZE_X,
                                  image->nm_across, NULL);
    PARSE_INT_ATTRIBUTE_OR_RETURN(view, LEICA_ATTR_SIZE_Y,
                                  image->nm_down, NULL);
    PARSE_INT_ATTRIBUTE_OR_RETURN(view, LEICA_ATTR_OFFSET_X,
                                  image->nm_offset_x, NULL);
    PARSE_INT_ATTRIBUTE_OR_RETURN(view, LEICA_ATTR_OFFSET_Y,
                                  image->nm_offset_y, NULL);

    image->is_macro = (image->nm_offset_x == 0 &&
                       image->nm_offset_y == 0 &&
                       image->nm_across == collection->nm_across &&
                       image->nm_down == collection->nm_down);

    // get dimensions
    ctx->node = image_node;
    g_autoptr(xmlXPathObject) result =
      _openslide_xml_xpath_eval(ctx, "d:pixels/d:dimension");
    if (!result) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't find any dimensions in image");
      return NULL;
    }

    // create dimension structs
    for (int i = 0; i < result->nodesetval->nodeNr; i++) {
      xmlNode *dimension_node = result->nodesetval->nodeTab[i];

      // accept only dimensions from z-plane 0
      // TODO: support multiple z-planes
      g_autoptr(xmlChar) z =
        xmlGetProp(dimension_node, BAD_CAST LEICA_ATTR_Z_PLANE);
      if (z && strcmp((char *) z, "0")) {
        continue;
      }

      struct dimension *dimension = g_slice_new0(struct dimension);
      g_ptr_array_add(image->dimensions, dimension);

      PARSE_INT_ATTRIBUTE_OR_RETURN(dimension_node, LEICA_ATTR_IFD,
                                    dimension->dir, NULL);
      PARSE_INT_ATTRIBUTE_OR_RETURN(dimension_node, LEICA_ATTR_SIZE_X,
                                    dimension->width, NULL);
      PARSE_INT_ATTRIBUTE_OR_RETURN(dimension_node, LEICA_ATTR_SIZE_Y,
                                    dimension->height, NULL);

      dimension->nm_per_pixel = (double) image->nm_across / dimension->width;
    }

    // sort dimensions
    g_ptr_array_sort(image->dimensions, dimension_compare);
  }

  return g_steal_pointer(&collection);
}

static void set_prop(openslide_t *osr, const char *name, const char *value) {
  if (value) {
    g_hash_table_insert(osr->properties,
                        g_strdup(name),
                        g_strdup(value));
  }
}

// For compatibility, slides with 0-1 macro images, 1 brightfield main image,
// and no other main images quickhash the smallest main image dimension
// in z-plane 0.
// All other slides quickhash the lowest-resolution brightfield macro image.
static bool should_use_legacy_quickhash(const struct collection *collection) {
  uint32_t brightfield_main_images = 0;
  uint32_t macro_images = 0;
  for (uint32_t image_num = 0; image_num < collection->images->len;
       image_num++) {
    struct image *image = collection->images->pdata[image_num];
    if (image->is_macro) {
      macro_images++;
    } else {
      if (!image->illumination_source ||
          strcmp(image->illumination_source, LEICA_VALUE_BRIGHTFIELD)) {
        return false;
      }
      brightfield_main_images++;
    }
  }
  return (brightfield_main_images == 1 && macro_images <= 1);
}

// parent must free levels on failure
static bool create_levels_from_collection(openslide_t *osr,
                                          struct _openslide_tiffcache *tc,
                                          TIFF *tiff,
                                          struct collection *collection,
                                          GPtrArray *levels,
                                          int64_t *quickhash_dir,
                                          GError **err) {
  *quickhash_dir = -1;

  // set barcode property
  set_prop(osr, "leica.barcode", collection->barcode);

  // determine quickhash mode
  bool legacy_quickhash = should_use_legacy_quickhash(collection);
  //g_debug("legacy quickhash %s", legacy_quickhash ? "true" : "false");

  // process main image
  struct image *first_main_image = NULL;
  for (uint32_t image_num = 0; image_num < collection->images->len;
       image_num++) {
    struct image *image = collection->images->pdata[image_num];

    if (image->is_macro) {
      continue;
    }

    // we only support brightfield
    if (!image->illumination_source ||
        strcmp(image->illumination_source, LEICA_VALUE_BRIGHTFIELD)) {
      continue;
    }

    if (!first_main_image) {
      // first main image
      first_main_image = image;

      // add some properties
      set_prop(osr, "leica.aperture", image->aperture);
      set_prop(osr, "leica.creation-date", image->creation_date);
      set_prop(osr, "leica.device-model", image->device_model);
      set_prop(osr, "leica.device-version", image->device_version);
      set_prop(osr, "leica.illumination-source", image->illumination_source);
      set_prop(osr, "leica.objective", image->objective);

      // copy objective to standard property
      _openslide_duplicate_int_prop(osr, "leica.objective",
                                    OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
    }

    // verify that it's safe to composite this main image with the others
    if (strcmp(image->illumination_source,
               first_main_image->illumination_source) ||
        strcmp(image->objective, first_main_image->objective) ||
        image->dimensions->len != first_main_image->dimensions->len) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Slides with dissimilar main images are not supported");
      return false;
    }

    // add all the IFDs to the level list
    for (uint32_t dimension_num = 0; dimension_num < image->dimensions->len;
         dimension_num++) {
      struct dimension *dimension = image->dimensions->pdata[dimension_num];

      struct level *l;
      if (image == first_main_image) {
        // no level yet; create it
        l = g_slice_new0(struct level);
        l->areas =
          g_ptr_array_new_with_free_func((GDestroyNotify) destroy_area);
        l->nm_per_pixel = dimension->nm_per_pixel;
        g_ptr_array_add(levels, l);
      } else {
        // get level
        g_assert(dimension_num < levels->len);
        l = levels->pdata[dimension_num];

        // maximize pixel density
        l->nm_per_pixel = MIN(l->nm_per_pixel, dimension->nm_per_pixel);

        // verify compatible resolution, with some tolerance for rounding
        struct dimension *first_image_dimension =
          first_main_image->dimensions->pdata[dimension_num];
        double resolution_similarity = 1 - fabs(dimension->nm_per_pixel -
          first_image_dimension->nm_per_pixel) /
          first_image_dimension->nm_per_pixel;
        //g_debug("resolution similarity %g", resolution_similarity);
        if (resolution_similarity < 0.98) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "Inconsistent main image resolutions");
          return false;
        }
      }

      // create area
      struct area *area = g_slice_new0(struct area);
      struct _openslide_tiff_level *tiffl = &area->tiffl;
      g_ptr_array_add(l->areas, area);

      // select and examine TIFF directory
      if (!_openslide_tiff_level_init(tiff, dimension->dir,
                                      NULL, tiffl,
                                      err)) {
        return false;
      }

      // set area offset, in nm
      area->offset_x = image->nm_offset_x;
      area->offset_y = image->nm_offset_y;
      //g_debug("directory %"PRId64", nm/pixel %g", dimension->dir, dimension->nm_per_pixel);

      // verify that we can read this compression (hard fail if not)
      uint16_t compression;
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Can't read compression scheme");
        return false;
      }
      if (!TIFFIsCODECConfigured(compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unsupported TIFF compression: %u", compression);
        return false;
      }

      // create grid
      area->grid = _openslide_grid_create_simple(osr,
                                                 tiffl->tiles_across,
                                                 tiffl->tiles_down,
                                                 tiffl->tile_w,
                                                 tiffl->tile_h,
                                                 read_tile);
    }

    // set quickhash directory in legacy mode
    if (legacy_quickhash && image == first_main_image) {
      struct dimension *dimension =
        image->dimensions->pdata[image->dimensions->len - 1];
      *quickhash_dir = dimension->dir;
    }
  }

  if (!first_main_image) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't find main image");
    return false;
  }

  // now we have maximized pixel densities
  for (uint32_t level_num = 0; level_num < levels->len; level_num++) {
    struct level *l = levels->pdata[level_num];

    // set level size
    l->base.w = ceil(collection->nm_across / l->nm_per_pixel);
    l->base.h = ceil(collection->nm_down / l->nm_per_pixel);
    //g_debug("level %d, nm/pixel %g", level_num, l->nm_per_pixel);

    // convert area offsets from nm to pixels
    for (uint32_t area_num = 0; area_num < l->areas->len; area_num++) {
      struct area *area = l->areas->pdata[area_num];
      area->offset_x = area->offset_x / l->nm_per_pixel;
      area->offset_y = area->offset_y / l->nm_per_pixel;
    }
  }

  // process macro image
  bool have_macro_image = false;
  for (uint32_t image_num = 0; image_num < collection->images->len;
       image_num++) {
    struct image *image = collection->images->pdata[image_num];

    if (!image->is_macro) {
      continue;
    }

    // we only support brightfield
    if (!image->illumination_source ||
        strcmp(image->illumination_source, LEICA_VALUE_BRIGHTFIELD)) {
      continue;
    }

    if (have_macro_image) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Found multiple macro images");
      return false;
    }

    // add associated image with largest dimension
    struct dimension *dimension = image->dimensions->pdata[0];
    if (!_openslide_tiff_add_associated_image(osr, "macro", tc,
                                              dimension->dir, err)) {
      return false;
    }

    // use smallest macro dimension for quickhash
    if (!legacy_quickhash) {
      dimension = image->dimensions->pdata[image->dimensions->len - 1];
      *quickhash_dir = dimension->dir;
    }

    have_macro_image = true;
  }

  if (*quickhash_dir == -1) {
    // e.g., new-style quickhash but no macro image
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't locate TIFF directory for quickhash");
    return false;
  }

  //g_debug("quickhash directory %"PRId64, *quickhash_dir);
  return true;
}

static bool leica_open(openslide_t *osr, const char *filename,
                       struct _openslide_tifflike *tl,
                       struct _openslide_hash *quickhash1, GError **err) {
  // open TIFF
  g_autoptr(_openslide_tiffcache) tc = _openslide_tiffcache_create(filename);
  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
  if (!ct.tiff) {
    return false;
  }

  // get the xml description
  char *image_desc;
  if (!TIFFGetField(ct.tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read ImageDescription");
    return false;
  }

  // read XML
  g_autoptr(collection) collection = parse_xml_description(image_desc, err);
  if (!collection) {
    return false;
  }

  // initialize and verify levels
  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_with_free_func((GDestroyNotify) destroy_level);
  int64_t quickhash_dir;
  if (!create_levels_from_collection(osr, tc, ct.tiff, collection,
                                     level_array, &quickhash_dir, err)) {
    return false;
  }

  // set hash and properties
  g_assert(level_array->len > 0);
  struct level *level0 = level_array->pdata[0];
  struct area *property_area = level0->areas->pdata[0];
  tdir_t property_dir = property_area->tiffl.dir;
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    quickhash_dir,
                                                    property_dir,
                                                    err)) {
    return false;
  }

  // keep the XML document out of the properties
  // (in case pyramid level 0 is also directory 0)
  g_hash_table_remove(osr->properties, OPENSLIDE_PROPERTY_NAME_COMMENT);
  g_hash_table_remove(osr->properties, "tiff.ImageDescription");

  // set MPP properties
  if (!_openslide_tiff_set_dir(ct.tiff, property_dir, err)) {
    return false;
  }
  set_resolution_prop(osr, ct.tiff, OPENSLIDE_PROPERTY_NAME_MPP_X,
                      TIFFTAG_XRESOLUTION);
  set_resolution_prop(osr, ct.tiff, OPENSLIDE_PROPERTY_NAME_MPP_Y,
                      TIFFTAG_YRESOLUTION);

  // set region bounds properties
  set_region_bounds_props(osr, level0);

  // allocate private data
  struct leica_ops_data *data = g_slice_new0(struct leica_ops_data);
  data->tc = g_steal_pointer(&tc);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = level_array->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &leica_ops;

  return true;
}

const struct _openslide_format _openslide_format_leica = {
  .name = "leica",
  .vendor = "leica",
  .detect = leica_detect,
  .open = leica_open,
};
