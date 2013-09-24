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
 * Ventana (bif) support
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
#include <math.h>
#include <tiffio.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

static const char LEVEL_DESCRIPTION_TOKEN[] = "level=";
static const char MACRO_DESCRIPTION[] = "Label Image";
static const char THUMBNAIL_DESCRIPTION[] = "Thumbnail";

static const char LEVEL_KEY[] = "level";
static const char MAGNIFICATION_KEY[] = "mag";

static const char INITIAL_ROOT_TAG[] = "iScan";
static const char ATTR_Z_LAYERS[] = "Z-layers";

static const char ATTR_AOI_SCANNED[] = "AOIScanned";
static const char ATTR_WIDTH[] = "Width";
static const char ATTR_HEIGHT[] = "Height";
static const char ATTR_NUM_ROWS[] = "NumRows";
static const char ATTR_NUM_COLS[] = "NumCols";
static const char ATTR_ORIGIN_X[] = "OriginX";
static const char ATTR_ORIGIN_Y[] = "OriginY";
static const char ATTR_CONFIDENCE[] = "Confidence";
static const char ATTR_DIRECTION[] = "Direction";
static const char ATTR_TILE1[] = "Tile1";
static const char ATTR_TILE2[] = "Tile2";
static const char ATTR_OVERLAP_X[] = "OverlapX";
static const char ATTR_OVERLAP_Y[] = "OverlapY";
static const char DIRECTION_RIGHT[] = "RIGHT";
static const char DIRECTION_UP[] = "UP";

#define PARSE_INT_ATTRIBUTE_OR_FAIL(NODE, NAME, OUT)		\
  do {								\
    GError *tmp_err = NULL;					\
    OUT = _openslide_xml_parse_int_attr(NODE, NAME, &tmp_err);	\
    if (tmp_err)  {						\
      g_propagate_error(err, tmp_err);				\
      goto FAIL;						\
    }								\
  } while (0)

struct ventana_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

// structs used during open
struct slide_info {
  struct area **areas;
  int32_t num_areas;

  double tile_advance_x;
  double tile_advance_y;
};

struct area {
  int64_t start_col;
  int64_t start_row;
  int64_t tiles_across;
  int64_t tiles_down;
  int64_t tile_count;
  struct tile **tiles;
};

struct joint {
  int64_t offset_x;
  int64_t offset_y;
  int64_t confidence;
};

struct tile {
  struct joint left;
  struct joint top;
};

static void destroy_data(struct ventana_ops_data *data,
                         struct level **levels, int32_t level_count) {
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct ventana_ops_data, data);

  for (int32_t i = 0; i < level_count; i++) {
    _openslide_grid_destroy(levels[i]->grid);
    g_slice_free(struct level, levels[i]);
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  struct ventana_ops_data *data = osr->data;
  struct level **levels = (struct level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}

static bool read_subtile(openslide_t *osr,
                         cairo_t *cr,
                         struct _openslide_level *level,
                         int64_t subtile_col, int64_t subtile_row,
                         void *subtile G_GNUC_UNUSED,
                         void *arg,
                         GError **err) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;
  const int64_t subtiles_per_tile = l->base.downsample;
  bool success = true;

  // tile size and coordinates
  int64_t tile_col = subtile_col / subtiles_per_tile;
  int64_t tile_row = subtile_row / subtiles_per_tile;
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // subtile offset and size
  double subtile_w = (double) tw / subtiles_per_tile;
  double subtile_h = (double) th / subtiles_per_tile;
  double subtile_x = subtile_col % subtiles_per_tile * subtile_w;
  double subtile_y = subtile_row % subtiles_per_tile * subtile_h;

  // get tile data, possibly from cache
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

  // draw
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 tw, th,
                                                                 tw * 4);

  // if we are drawing a subtile, we must do an additional copy,
  // because cairo lacks source clipping
  if (subtiles_per_tile > 1) {
    cairo_surface_t *surface2 = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                           ceil(subtile_w),
                                                           ceil(subtile_h));
    cairo_t *cr2 = cairo_create(surface2);
    cairo_set_source_surface(cr2, surface, -subtile_x, -subtile_y);

    // replace original image surface
    cairo_surface_destroy(surface);
    surface = surface2;

    cairo_rectangle(cr2, 0, 0,
                    ceil(subtile_w),
                    ceil(subtile_h));
    cairo_fill(cr2);
    success = _openslide_check_cairo_status(cr2, err);
    cairo_destroy(cr2);
  }

  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return success;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct ventana_ops_data *data = osr->data;
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

static const struct _openslide_ops ventana_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static char *read_xml_packet(TIFF *tiff) {
  void *xml;
  uint32_t len;
  if (!TIFFGetField(tiff, TIFFTAG_XMLPACKET, &len, &xml)) {
    return NULL;
  }
  // copy to ensure null-termination
  return g_strndup(xml, len);
}

static void slide_info_free(struct slide_info *slide) {
  if (!slide) {
    return;
  }
  for (int32_t i = 0; i < slide->num_areas; i++) {
    struct area *area = slide->areas[i];
    for (int64_t j = 0; j < area->tile_count; j++) {
      g_slice_free(struct tile, area->tiles[j]);
    }
    g_free(area->tiles);
    g_slice_free(struct area, area);
  }
  g_slice_free(struct slide_info, slide);
}

static int width_compare(gconstpointer a, gconstpointer b) {
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

static bool parse_initial_xml(openslide_t *osr, const char *xml,
                              GError **err) {
  xmlDoc *doc = NULL;
  GError *tmp_err = NULL;

  // quick check for plausible XML string before parsing
  if (!strstr(xml, INITIAL_ROOT_TAG)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "%s not in XMLPacket", INITIAL_ROOT_TAG);
    goto FAIL;
  }

  // parse
  doc = _openslide_xml_parse(xml, &tmp_err);
  if (!doc) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "%s", tmp_err->message);
    g_clear_error(&tmp_err);
    goto FAIL;
  }
  xmlNode *root = xmlDocGetRootElement(doc);

  // check root tag name
  if (xmlStrcmp(root->name, BAD_CAST INITIAL_ROOT_TAG)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Root tag not %s", INITIAL_ROOT_TAG);
    goto FAIL;
  }

  // okay, assume Ventana slide

  // we don't know how to handle multiple Z layers
  int64_t z_layers;
  PARSE_INT_ATTRIBUTE_OR_FAIL(root, ATTR_Z_LAYERS, z_layers);
  if (z_layers != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Slides with multiple Z layers are not supported");
    goto FAIL;
  }

  if (osr) {
    // copy all iScan attributes to vendor properties
    for (xmlAttr *attr = root->properties; attr; attr = attr->next) {
      xmlChar *value = xmlGetNoNsProp(root, attr->name);
      if (value && *value) {
        g_hash_table_insert(osr->properties,
                            g_strdup_printf("ventana.%s", attr->name),
                            g_strdup((char *) value));
      }
      xmlFree(value);
    }

    // set standard properties
    _openslide_duplicate_int_prop(osr->properties, "ventana.Magnification",
                                  OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
    _openslide_duplicate_double_prop(osr->properties, "ventana.ScanRes",
                                     OPENSLIDE_PROPERTY_NAME_MPP_X);
    _openslide_duplicate_double_prop(osr->properties, "ventana.ScanRes",
                                     OPENSLIDE_PROPERTY_NAME_MPP_Y);
  }

  // clean up
  xmlFreeDoc(doc);
  return true;

FAIL:
  if (doc) {
    xmlFreeDoc(doc);
  }
  return false;
}

static struct slide_info *parse_level0_xml(const char *xml,
                                           int64_t tiff_tile_width,
                                           int64_t tiff_tile_height,
                                           GError **err) {
  GPtrArray *area_array = g_ptr_array_new();
  xmlXPathContext *ctx = NULL;
  xmlXPathObject *info_result = NULL;
  xmlXPathObject *origin_result = NULL;
  xmlXPathObject *result = NULL;
  int64_t total_offset_x = 0;
  int64_t total_offset_y = 0;
  int64_t total_x_weight = 0;
  int64_t total_y_weight = 0;
  bool success = false;

  // parse
  xmlDoc *doc = _openslide_xml_parse(xml, err);
  if (!doc) {
    goto FAIL;
  }
  ctx = _openslide_xml_xpath_create(doc);

  // query AOI metadata
  info_result = _openslide_xml_xpath_eval(ctx, "/EncodeInfo/SlideStitchInfo/ImageInfo");
  origin_result = _openslide_xml_xpath_eval(ctx, "/EncodeInfo/AoiOrigin/*");
  if (!info_result || !origin_result ||
      info_result->nodesetval->nodeNr != origin_result->nodesetval->nodeNr) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Missing or inconsistent region metadata");
    goto FAIL;
  }

  // walk AOIs
  for (int i = 0; i < info_result->nodesetval->nodeNr; i++) {
    xmlNode *info = info_result->nodesetval->nodeTab[i];
    xmlNode *aoi = origin_result->nodesetval->nodeTab[i];

    // skip ignored AOIs
    int64_t aoi_scanned;
    PARSE_INT_ATTRIBUTE_OR_FAIL(info, ATTR_AOI_SCANNED, aoi_scanned);
    if (!aoi_scanned) {
      continue;
    }

    // create area
    struct area *area = g_slice_new0(struct area);
    g_ptr_array_add(area_array, area);

    // get start tiles
    int64_t start_col_x, start_row_y;
    PARSE_INT_ATTRIBUTE_OR_FAIL(aoi, ATTR_ORIGIN_X, start_col_x);
    PARSE_INT_ATTRIBUTE_OR_FAIL(aoi, ATTR_ORIGIN_Y, start_row_y);
    int64_t tile_width, tile_height;
    PARSE_INT_ATTRIBUTE_OR_FAIL(info, ATTR_WIDTH, tile_width);
    PARSE_INT_ATTRIBUTE_OR_FAIL(info, ATTR_HEIGHT, tile_height);
    if (tile_width != tiff_tile_width || tile_height != tiff_tile_height) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Tile size mismatch");
      goto FAIL;
    }
    if (start_col_x % tile_width || start_row_y % tile_height) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Area origin not divisible by tile size");
      goto FAIL;
    }
    area->start_col = start_col_x / tile_width;
    area->start_row = start_row_y / tile_height;

    // get tile counts
    PARSE_INT_ATTRIBUTE_OR_FAIL(info, ATTR_NUM_COLS, area->tiles_across);
    PARSE_INT_ATTRIBUTE_OR_FAIL(info, ATTR_NUM_ROWS, area->tiles_down);

    //g_debug("area %d: start %"G_GINT64_FORMAT" %"G_GINT64_FORMAT", count %"G_GINT64_FORMAT" %"G_GINT64_FORMAT, i, area->start_col, area->start_row, area->tiles_across, area->tiles_down);

    // create tile structs
    area->tile_count = area->tiles_across * area->tiles_down;
    area->tiles = g_new(struct tile *, area->tile_count);
    for (int64_t j = 0; j < area->tile_count; j++) {
      area->tiles[j] = g_slice_new0(struct tile);
    }

    // walk tiles
    ctx->node = info;
    result = _openslide_xml_xpath_eval(ctx, "TileJointInfo");
    if (!result) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Couldn't find tile joint info");
      goto FAIL;
    }
    for (int j = 0; j < result->nodesetval->nodeNr; j++) {
      xmlNode *joint_info = result->nodesetval->nodeTab[j];

      // get tile numbers
      int64_t tile1, tile2;
      PARSE_INT_ATTRIBUTE_OR_FAIL(joint_info, ATTR_TILE1, tile1);
      PARSE_INT_ATTRIBUTE_OR_FAIL(joint_info, ATTR_TILE2, tile2);
      if (tile1 < 1 ||
          tile2 < 1 ||
          tile1 > area->tile_count ||
          tile2 > area->tile_count) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Tile number out of bounds: %"G_GINT64_FORMAT
                    " %"G_GINT64_FORMAT, tile1, tile2);
        goto FAIL;
      }
      // convert to zero-indexed
      tile1 -= 1;
      tile2 -= 1;

      // compute coordinates
      int64_t tile1_col = tile1 % area->tiles_across;
      int64_t tile1_row = tile1 / area->tiles_across;
      // columns in rows 2nd/4th/... from the bottom are numbered from
      // right to left
      if (tile1_row % 2) {
        tile1_col = area->tiles_across - tile1_col - 1;
      }
      // rows are numbered from bottom to top
      tile1_row = area->tiles_down - tile1_row - 1;
      int64_t tile2_col = tile2 % area->tiles_across;
      int64_t tile2_row = tile2 / area->tiles_across;
      if (tile2_row % 2) {
        tile2_col = area->tiles_across - tile2_col - 1;
      }
      tile2_row = area->tiles_down - tile2_row - 1;

      // check coordinates against direction, and get joint
      xmlChar *direction = xmlGetProp(joint_info, BAD_CAST ATTR_DIRECTION);
      struct joint *joint;
      bool ok;
      bool direction_y = false;
      //g_debug("%s, tile1 %"G_GINT64_FORMAT" %"G_GINT64_FORMAT", tile2 %"G_GINT64_FORMAT" %"G_GINT64_FORMAT, (char *) direction, tile1_col, tile1_row, tile2_col, tile2_row);
      if (!xmlStrcmp(direction, BAD_CAST DIRECTION_RIGHT)) {
        // get left joint of right tile
        struct tile *tile =
          area->tiles[tile2_row * area->tiles_across + tile2_col];
        joint = &tile->left;
        ok = (tile2_col == tile1_col + 1 && tile2_row == tile1_row);
      } else if (!xmlStrcmp(direction, BAD_CAST DIRECTION_UP)) {
        // get top joint of bottom tile
        struct tile *tile =
          area->tiles[tile1_row * area->tiles_across + tile1_col];
        joint = &tile->top;
        ok = (tile2_col == tile1_col && tile2_row == tile1_row - 1);
        direction_y = true;
      } else {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Bad direction attribute \"%s\"", (char *) direction);
        xmlFree(direction);
        goto FAIL;
      }
      if (!ok) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unexpected tile join: %s, "
                    "(%"G_GINT64_FORMAT", %"G_GINT64_FORMAT"), "
                    "(%"G_GINT64_FORMAT", %"G_GINT64_FORMAT")",
                    (char *) direction, tile1_col, tile1_row,
                    tile2_col, tile2_row);
        xmlFree(direction);
        goto FAIL;
      }
      xmlFree(direction);

      // read values
      PARSE_INT_ATTRIBUTE_OR_FAIL(joint_info, ATTR_OVERLAP_X,
                                  joint->offset_x);
      joint->offset_x *= -1;
      PARSE_INT_ATTRIBUTE_OR_FAIL(joint_info, ATTR_OVERLAP_Y,
                                  joint->offset_y);
      joint->offset_y *= -1;
      PARSE_INT_ATTRIBUTE_OR_FAIL(joint_info, ATTR_CONFIDENCE,
                                  joint->confidence);

      // add to totals
      if (direction_y) {
        total_offset_y += joint->confidence * joint->offset_y;
        total_y_weight += joint->confidence;
      } else {
        total_offset_x += joint->confidence * joint->offset_x;
        total_x_weight += joint->confidence;
      }
    }
    xmlXPathFreeObject(result);
    result = NULL;
  }

  success = true;

FAIL:
  // clean up
  xmlXPathFreeObject(result);
  xmlXPathFreeObject(origin_result);
  xmlXPathFreeObject(info_result);
  xmlXPathFreeContext(ctx);
  if (doc) {
    xmlFreeDoc(doc);
  }

  // create wrapper struct
  struct slide_info *slide = g_slice_new0(struct slide_info);
  slide->num_areas = area_array->len;
  slide->areas = (struct area **) g_ptr_array_free(area_array, false);
  slide->tile_advance_x =
    tiff_tile_width + (double) total_offset_x / total_x_weight;
  slide->tile_advance_y =
    tiff_tile_height + (double) total_offset_y / total_y_weight;
  //g_debug("advances: %g %g", slide->tile_advance_x, slide->tile_advance_y);

  // free on failure
  if (!success) {
    slide_info_free(slide);
    slide = NULL;
  }

  return slide;
}

static bool parse_level_info(const char *desc,
                             int64_t *level, double *magnification,
                             GError **err) {
  bool success = false;

  // read all key/value pairs
  GHashTable *fields = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);
  char **pairs = g_strsplit(desc, " ", 0);
  for (char **pair = pairs; *pair; pair++) {
    char **kv = g_strsplit(*pair, "=", 2);
    if (g_strv_length(kv) == 2) {
      g_hash_table_insert(fields, kv[0], kv[1]);
      g_free(kv);
    } else {
      g_strfreev(kv);
    }
  }
  g_strfreev(pairs);

  // get mandatory fields
  char *level_str = g_hash_table_lookup(fields, LEVEL_KEY);
  char *magnification_str = g_hash_table_lookup(fields, MAGNIFICATION_KEY);
  if (!level_str || !magnification_str) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Missing level fields");
    goto DONE;
  }

  // parse level
  gchar *endptr;
  *level = g_ascii_strtoll(level_str, &endptr, 10);
  if (level_str[0] == 0 || endptr[0] != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid level number");
    goto DONE;
  }

  // parse magnification
  *magnification = g_ascii_strtod(magnification_str, &endptr);
  if (magnification_str[0] == 0 || endptr[0] != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid magnification");
    goto DONE;
  }

  success = true;

DONE:
  g_hash_table_destroy(fields);
  return success;
}

static struct _openslide_grid *create_grid(openslide_t *osr,
                                           struct slide_info *slide,
                                           double downsample,
                                           int64_t tile_w, int64_t tile_h) {
  double subtile_w = tile_w / downsample;
  double subtile_h = tile_h / downsample;

  struct _openslide_grid *grid =
    _openslide_grid_create_tilemap(osr,
                                   slide->tile_advance_x / downsample,
                                   slide->tile_advance_y / downsample,
                                   read_subtile, NULL);

  for (int32_t i = 0; i < slide->num_areas; i++) {
    struct area *area = slide->areas[i];
    for (int64_t row = area->start_row;
         row < area->start_row + area->tiles_down; row++) {
      for (int64_t col = area->start_col;
           col < area->start_col + area->tiles_across; col++) {
        _openslide_grid_tilemap_add_tile(grid,
                                         col, row,
                                         0, 0,
                                         subtile_w, subtile_h,
                                         NULL);
      }
    }
  }

  return grid;
}

bool _openslide_try_ventana(openslide_t *osr,
                            struct _openslide_tiffcache *tc,
                            TIFF *tiff,
                            struct _openslide_hash *quickhash1,
                            GError **err) {
  GPtrArray *level_array = g_ptr_array_new();
  struct slide_info *slide = NULL;

  // parse iScan XML
  char *xml = read_xml_packet(tiff);
  if (!xml) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a Ventana slide");
    goto FAIL;
  }
  if (!parse_initial_xml(osr, xml, err)) {
    g_free(xml);
    goto FAIL;
  }
  g_free(xml);

  // okay, assume Ventana slide

  // walk directories
  int64_t next_level = 0;
  double prev_magnification = INFINITY;
  double level0_magnification = 0;
  do {
    tdir_t dir = TIFFCurrentDirectory(tiff);

    // read ImageDescription
    char *image_desc;
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
      continue;
    }

    if (strstr(image_desc, LEVEL_DESCRIPTION_TOKEN)) {
      // is a level

      // parse description
      int64_t level;
      double magnification;
      if (!parse_level_info(image_desc, &level, &magnification, err)) {
        goto FAIL;
      }

      // verify that levels and magnifications are properly ordered
      if (level != next_level++) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unexpected encounter with level %"G_GINT64_FORMAT, level);
        goto FAIL;
      }
      if (magnification >= prev_magnification) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unexpected magnification in level %"G_GINT64_FORMAT,
                    level);
        goto FAIL;
      }
      prev_magnification = magnification;

      // compute downsample
      if (level == 0) {
        level0_magnification = magnification;
      }
      double downsample = level0_magnification / magnification;

      // if first level, parse tile info
      if (level == 0) {
        // get tile size
        struct _openslide_tiff_level tiffl;
        if (!_openslide_tiff_level_init(tiff, dir, NULL, &tiffl, err)) {
          goto FAIL;
        }
        // get XML
        xml = read_xml_packet(tiff);
        if (!xml) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "Can't read tile info");
          goto FAIL;
        }
        // parse
        slide = parse_level0_xml(xml, tiffl.tile_w, tiffl.tile_h, err);
        g_free(xml);
        if (!slide) {
          goto FAIL;
        }
      }

      // confirm that this directory is tiled
      if (!TIFFIsTiled(tiff)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Directory %d is not tiled", dir);
        goto FAIL;
      }

      // verify that we can read this compression (hard fail if not)
      uint16_t compression;
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Can't read compression scheme");
        goto FAIL;
      };
      if (!TIFFIsCODECConfigured(compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unsupported TIFF compression: %u", compression);
        goto FAIL;
      }

      // create level
      struct level *l = g_slice_new0(struct level);
      struct _openslide_tiff_level *tiffl = &l->tiffl;
      if (!_openslide_tiff_level_init(tiff, dir,
                                      NULL, tiffl,
                                      err)) {
        g_slice_free(struct level, l);
        goto FAIL;
      }
      struct level *level0 = l;
      if (level > 0) {
        level0 = level_array->pdata[0];
      }
      l->base.downsample = downsample;
      l->grid = create_grid(osr, slide,
                            downsample,
                            tiffl->tile_w, tiffl->tile_h);
      // the format doesn't seem to record the level size, so make it
      // large enough for all the pixels
      double x, y, w, h;
      _openslide_grid_get_bounds(l->grid, &x, &y, &w, &h);
      l->base.w = ceil(x + w);
      l->base.h = ceil(y + h);
      //g_debug("level %"G_GINT64_FORMAT": magnification %g, downsample %g, size %"G_GINT64_FORMAT" %"G_GINT64_FORMAT, level, magnification, downsample, l->base.w, l->base.h);

      // add to array
      g_ptr_array_add(level_array, l);

      // verify consistent tile sizes
      // our math is all based on level 0 tile sizes, but
      // _openslide_tiff_read_tile() uses the directory's tile size
      if (l->tiffl.tile_w != level0->tiffl.tile_w ||
          l->tiffl.tile_h != level0->tiffl.tile_h) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Inconsistent TIFF tile sizes");
        goto FAIL;
      }

    } else if (!strcmp(image_desc, MACRO_DESCRIPTION)) {
      // macro image
      if (!_openslide_tiff_add_associated_image(osr, "macro", tc, dir,
                                                err)) {
	g_prefix_error(err, "Can't read macro image: ");
	goto FAIL;
      }

    } else if (!strcmp(image_desc, THUMBNAIL_DESCRIPTION)) {
      // thumbnail image
      if (!_openslide_tiff_add_associated_image(osr, "thumbnail", tc, dir,
                                                err)) {
	g_prefix_error(err, "Can't read thumbnail image: ");
	goto FAIL;
      }
    }
  } while (TIFFReadDirectory(tiff));

  // free slide info
  slide_info_free(slide);
  slide = NULL;

  // sort tiled levels
  g_ptr_array_sort(level_array, width_compare);

  // unwrap level array
  int32_t level_count = level_array->len;
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // allocate private data
  struct ventana_ops_data *data = g_slice_new0(struct ventana_ops_data);

  if (osr == NULL) {
    // free now and return
    _openslide_tiffcache_put(tc, tiff);
    data->tc = tc;
    destroy_data(data, levels, level_count);
    return true;
  }

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
                      g_strdup("ventana"));

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &ventana_ops;

  // put TIFF handle and assume tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  return true;

FAIL:
  // free slide info
  slide_info_free(slide);
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
