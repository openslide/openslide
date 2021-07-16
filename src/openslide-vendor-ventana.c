/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
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
 * Ventana (bif/tif) support
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
#include <math.h>
#include <tiffio.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

static const char LEVEL_DESCRIPTION_TOKEN[] = "level=";
static const char MACRO_DESCRIPTION[] = "Label Image";
static const char MACRO_DESCRIPTION2[] = "Label_Image";
static const char THUMBNAIL_DESCRIPTION[] = "Thumbnail";

static const char LEVEL_KEY[] = "level";
static const char MAGNIFICATION_KEY[] = "mag";

static const char INITIAL_XML_ISCAN[] = "iScan";
static const char INITIAL_XML_ALT_ROOT[] = "Metadata";

static const char ATTR_AOI_SCANNED[] = "AOIScanned";
static const char ATTR_WIDTH[] = "Width";
static const char ATTR_HEIGHT[] = "Height";
static const char ATTR_NUM_ROWS[] = "NumRows";
static const char ATTR_NUM_COLS[] = "NumCols";
static const char ATTR_POS_X[] = "Pos-X";
static const char ATTR_POS_Y[] = "Pos-Y";
static const char ATTR_ORIGIN_X[] = "OriginX";
static const char ATTR_ORIGIN_Y[] = "OriginY";
static const char ATTR_CONFIDENCE[] = "Confidence";
static const char ATTR_DIRECTION[] = "Direction";
static const char ATTR_TILE1[] = "Tile1";
static const char ATTR_TILE2[] = "Tile2";
static const char ATTR_OVERLAP_X[] = "OverlapX";
static const char ATTR_OVERLAP_Y[] = "OverlapY";
static const char DIRECTION_LEFT[] = "LEFT";
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

#define PARSE_DOUBLE_ATTRIBUTE_OR_FAIL(NODE, NAME, OUT)			\
  do {									\
    GError *tmp_err = NULL;						\
    OUT = _openslide_xml_parse_double_attr(NODE, NAME, &tmp_err);	\
    if (tmp_err)  {							\
      g_propagate_error(err, tmp_err);					\
      goto FAIL;							\
    }									\
  } while (0)

struct ventana_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
  int64_t subtiles_per_tile;
};

// structs used during BIF open
struct bif {
  struct area **areas;
  int32_t num_areas;

  double tile_advance_x;
  double tile_advance_y;
};

struct area {
  int64_t x;
  int64_t y;
  int64_t start_col;
  int64_t start_row;
  int64_t tiles_across;
  int64_t tiles_down;
  int64_t tile_count;
  struct tile **tiles;
};

struct joint {
  double offset_x;
  double offset_y;
  int64_t confidence;
};

struct tile {
  struct joint left;
  struct joint top;
};

static void destroy(openslide_t *osr) {
  struct ventana_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct ventana_ops_data, data);

  for (int32_t i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];
    _openslide_grid_destroy(l->grid);
    g_slice_free(struct level, l);
  }
  g_free(osr->levels);
}

static bool read_subtile(openslide_t *osr,
                         cairo_t *cr,
                         struct _openslide_level *level,
                         int64_t subtile_col, int64_t subtile_row,
                         void *arg,
                         GError **err) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;
  bool success = true;

  // tile size and coordinates
  int64_t tile_col = subtile_col / l->subtiles_per_tile;
  int64_t tile_row = subtile_row / l->subtiles_per_tile;
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // subtile offset and size
  double subtile_w = (double) tw / l->subtiles_per_tile;
  double subtile_h = (double) th / l->subtiles_per_tile;
  double subtile_x = subtile_col % l->subtiles_per_tile * subtile_w;
  double subtile_y = subtile_row % l->subtiles_per_tile * subtile_h;

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
  if (l->subtiles_per_tile > 1) {
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

// read_subtile wrapper for BIF that drops the extra argument passed by
// the tilemap grid
static bool read_subtile_tilemap(openslide_t *osr,
                                 cairo_t *cr,
                                 struct _openslide_level *level,
                                 int64_t subtile_col, int64_t subtile_row,
                                 void *subtile G_GNUC_UNUSED,
                                 void *arg,
                                 GError **err) {
  return read_subtile(osr, cr, level,
                      subtile_col, subtile_row,
                      arg, err);
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct ventana_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  TIFF *tiff = _openslide_tiffcache_get(data->tc, err);
  if (tiff == NULL) {
    return false;
  }

  bool success = _openslide_grid_paint_region(l->grid, cr, tiff,
                                              x / l->base.downsample,
                                              y / l->base.downsample,
                                              level, w, h,
                                              err);
  _openslide_tiffcache_put(data->tc, tiff);

  return success;
}

static const struct _openslide_ops ventana_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static xmlNode *get_initial_xml_iscan(xmlDoc *doc, GError **err) {
  xmlNode *root = xmlDocGetRootElement(doc);
  if (!xmlStrcmp(root->name, BAD_CAST INITIAL_XML_ISCAN)) {
    // /iScan
    return root;

  } else if (!xmlStrcmp(root->name, BAD_CAST INITIAL_XML_ALT_ROOT)) {
    for (xmlNode *node = root->children; node; node = node->next) {
      if (!xmlStrcmp(node->name, BAD_CAST INITIAL_XML_ISCAN)) {
        // /Metadata/iScan, found in some slides
        return node;
      }
    }
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't find iScan element in initial XML");
    return false;

  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unrecognized root element in initial XML");
    return false;
  }
}

static bool ventana_detect(const char *filename G_GNUC_UNUSED,
                           struct _openslide_tifflike *tl,
                           GError **err) {
  // ensure we have a TIFF
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // read XMLPacket
  const char *xml = _openslide_tifflike_get_buffer(tl, 0, TIFFTAG_XMLPACKET,
                                                   err);
  if (!xml) {
    return false;
  }

  // check for plausible XML string before parsing
  if (!strstr(xml, INITIAL_XML_ISCAN)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "%s not in XMLPacket", INITIAL_XML_ISCAN);
    return false;
  }

  // parse
  xmlDoc *doc = _openslide_xml_parse(xml, err);
  if (!doc) {
    return false;
  }

  // check for iScan element
  if (!get_initial_xml_iscan(doc, err)) {
    xmlFreeDoc(doc);
    return false;
  }

  xmlFreeDoc(doc);
  return true;
}

static void bif_free(struct bif *bif) {
  if (!bif) {
    return;
  }
  for (int32_t i = 0; i < bif->num_areas; i++) {
    struct area *area = bif->areas[i];
    for (int64_t j = 0; j < area->tile_count; j++) {
      g_slice_free(struct tile, area->tiles[j]);
    }
    g_free(area->tiles);
    g_slice_free(struct area, area);
  }
  g_free(bif->areas);
  g_slice_free(struct bif, bif);
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
  // parse
  xmlDoc *doc = _openslide_xml_parse(xml, err);
  if (!doc) {
    return false;
  }

  // get iScan element
  xmlNode *iscan = get_initial_xml_iscan(doc, err);
  if (!iscan) {
    goto FAIL;
  }

  // copy all iScan attributes to vendor properties
  for (xmlAttr *attr = iscan->properties; attr; attr = attr->next) {
    xmlChar *value = xmlGetNoNsProp(iscan, attr->name);
    if (value && *value) {
      g_hash_table_insert(osr->properties,
                          g_strdup_printf("ventana.%s", attr->name),
                          g_strdup((char *) value));
    }
    xmlFree(value);
  }

  // set standard properties
  _openslide_duplicate_int_prop(osr, "ventana.Magnification",
                                OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_double_prop(osr, "ventana.ScanRes",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr, "ventana.ScanRes",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);

  // clean up
  xmlFreeDoc(doc);
  return true;

FAIL:
  xmlFreeDoc(doc);
  return false;
}

static bool get_tile_coordinates(const struct area *area,
                                 xmlNode *joint_info, const char *attr_name,
                                 int64_t *tile_col, int64_t *tile_row,
                                 GError **err) {
  // get tile number
  int64_t tile;
  PARSE_INT_ATTRIBUTE_OR_FAIL(joint_info, attr_name, tile);
  if (tile < 1 || tile > area->tile_count) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Tile number out of bounds: %"PRId64, tile);
    goto FAIL;
  }

  // convert to zero-indexed
  tile -= 1;

  // compute coordinates
  int64_t col = tile % area->tiles_across;
  int64_t row = tile / area->tiles_across;
  // columns in rows 2nd/4th/... from the bottom are numbered from
  // right to left
  if (row % 2) {
    col = area->tiles_across - col - 1;
  }
  // rows are numbered from bottom to top
  row = area->tiles_down - row - 1;

  // commit
  *tile_col = col;
  *tile_row = row;
  return true;

FAIL:
  return false;
}

static struct bif *parse_level0_xml(const char *xml,
                                    int64_t tiff_tile_width,
                                    int64_t tiff_tile_height,
                                    GError **err) {
  GPtrArray *area_array = g_ptr_array_new();
  xmlXPathContext *ctx = NULL;
  xmlXPathObject *info_result = NULL;
  xmlXPathObject *origin_result = NULL;
  xmlXPathObject *result = NULL;
  double total_offset_x = 0;
  double total_offset_y = 0;
  int64_t total_x_weight = 0;
  int64_t total_y_weight = 0;
  bool success = false;

  // parse
  xmlDoc *doc = _openslide_xml_parse(xml, err);
  if (!doc) {
    g_prefix_error(err, "Parsing level 0 XML: ");
    goto FAIL;
  }
  ctx = _openslide_xml_xpath_create(doc);

  // query AOI metadata
  info_result =
    _openslide_xml_xpath_eval(ctx, "/EncodeInfo/SlideStitchInfo/ImageInfo");
  origin_result =
    _openslide_xml_xpath_eval(ctx, "/EncodeInfo/AoiOrigin/*");
  if (!info_result || !origin_result ||
      info_result->nodesetval->nodeNr != origin_result->nodesetval->nodeNr) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
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
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Tile size mismatch: "
                  "expected %"PRId64"x%"PRId64", found %"PRId64"x%"PRId64,
                  tiff_tile_width, tiff_tile_height, tile_width, tile_height);
      goto FAIL;
    }
    if (start_col_x % tile_width || start_row_y % tile_height) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Area origin not divisible by tile size: "
                  "%"PRId64" %% %"PRId64", %"PRId64" %% %"PRId64,
                  start_col_x, tile_width, start_row_y, tile_height);
      goto FAIL;
    }
    area->start_col = start_col_x / tile_width;
    area->start_row = start_row_y / tile_height;

    // get tile counts
    PARSE_INT_ATTRIBUTE_OR_FAIL(info, ATTR_NUM_COLS, area->tiles_across);
    PARSE_INT_ATTRIBUTE_OR_FAIL(info, ATTR_NUM_ROWS, area->tiles_down);

    // get position
    // it seems these are always whole numbers, but they are sometimes
    // encoded as floating-point values
    double x, y;
    PARSE_DOUBLE_ATTRIBUTE_OR_FAIL(info, ATTR_POS_X, x);
    PARSE_DOUBLE_ATTRIBUTE_OR_FAIL(info, ATTR_POS_Y, y);
    area->x = x;
    area->y = y;

    //g_debug("area %d: start %"PRId64" %"PRId64", count %"PRId64" %"PRId64", pos %"PRId64" %"PRId64, i, area->start_col, area->start_row, area->tiles_across, area->tiles_down, area->x, area->y);

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
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't find tile joint info");
      goto FAIL;
    }
    for (int j = 0; j < result->nodesetval->nodeNr; j++) {
      xmlNode *joint_info = result->nodesetval->nodeTab[j];

      // get tile coordinates
      int64_t tile1_col, tile1_row;
      int64_t tile2_col, tile2_row;
      if (!get_tile_coordinates(area, joint_info, ATTR_TILE1,
                                &tile1_col, &tile1_row, err)) {
        goto FAIL;
      }
      if (!get_tile_coordinates(area, joint_info, ATTR_TILE2,
                                &tile2_col, &tile2_row, err)) {
        goto FAIL;
      }

      // check coordinates against direction, and get joint
      xmlChar *direction = xmlGetProp(joint_info, BAD_CAST ATTR_DIRECTION);
      struct joint *joint;
      bool ok;
      bool direction_y = false;
      //g_debug("%s, tile1 %"PRId64" %"PRId64", tile2 %"PRId64" %"PRId64, (char *) direction, tile1_col, tile1_row, tile2_col, tile2_row);
      if (!xmlStrcmp(direction, BAD_CAST DIRECTION_RIGHT) || !xmlStrcmp(direction, BAD_CAST DIRECTION_LEFT)) {
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
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Bad direction attribute \"%s\"", (char *) direction);
        xmlFree(direction);
        goto FAIL;
      }
      if (!ok) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unexpected tile join: %s, "
                    "(%"PRId64", %"PRId64"), (%"PRId64", %"PRId64")",
                    (char *) direction, tile1_col, tile1_row,
                    tile2_col, tile2_row);
        xmlFree(direction);
        goto FAIL;
      }
      xmlFree(direction);

      // read values
      PARSE_DOUBLE_ATTRIBUTE_OR_FAIL(joint_info, ATTR_OVERLAP_X,
                                     joint->offset_x);
      joint->offset_x *= -1;
      PARSE_DOUBLE_ATTRIBUTE_OR_FAIL(joint_info, ATTR_OVERLAP_Y,
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
  struct bif *bif = g_slice_new0(struct bif);
  bif->num_areas = area_array->len;
  bif->areas = (struct area **) g_ptr_array_free(area_array, false);
  bif->tile_advance_x = tiff_tile_width + total_offset_x / total_x_weight;
  bif->tile_advance_y = tiff_tile_height + total_offset_y / total_y_weight;
  //g_debug("advances: %g %g", bif->tile_advance_x, bif->tile_advance_y);

  // Fix area Y coordinates.  The Pos-Y read from the file is the distance
  // from the bottom of the area to a point below all areas.
  int64_t *heights = g_new(int64_t, bif->num_areas);
  // find position of top of slide in coordinate plane of file
  int64_t top = 0;
  for (int32_t i = 0; i < bif->num_areas; i++) {
    struct area *area = bif->areas[i];
    heights[i] =
      (area->tiles_down - 1) * bif->tile_advance_y + tiff_tile_height;
    top = MAX(top, area->y + heights[i]);
    //g_debug("area %d height %"PRId64, i, heights[i]);
  }
  //g_debug("top %"PRId64, top);
  // convert Y coordinate of each area
  for (int32_t i = 0; i < bif->num_areas; i++) {
    struct area *area = bif->areas[i];
    area->y = top - area->y - heights[i];
  }
  g_free(heights);

  // free on failure
  if (!success) {
    bif_free(bif);
    bif = NULL;
  }

  return bif;
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
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Missing level field");
    goto DONE;
  }

  // parse level
  gchar *endptr;
  *level = g_ascii_strtoll(level_str, &endptr, 10);
  if (level_str[0] == 0 || endptr[0] != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid level number");
    goto DONE;
  }

  // parse magnification
  *magnification = _openslide_parse_double(magnification_str);
  if (isnan(*magnification)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid magnification");
    goto DONE;
  }

  success = true;

DONE:
  g_hash_table_destroy(fields);
  return success;
}

static struct _openslide_grid *create_bif_grid(openslide_t *osr,
                                               struct bif *bif,
                                               double downsample,
                                               int64_t tile_w, int64_t tile_h) {
  double subtile_w = tile_w / downsample;
  double subtile_h = tile_h / downsample;

  struct _openslide_grid *grid =
    _openslide_grid_create_tilemap(osr,
                                   bif->tile_advance_x / downsample,
                                   bif->tile_advance_y / downsample,
                                   read_subtile_tilemap, NULL);

  for (int32_t i = 0; i < bif->num_areas; i++) {
    struct area *area = bif->areas[i];
    double offset_x =
      (area->x - area->start_col * bif->tile_advance_x) / downsample;
    double offset_y =
      (area->y - area->start_row * bif->tile_advance_y) / downsample;
    //g_debug("ds %g area %d pos %"PRId64" %"PRId64" offset %g %g", downsample, i, area->x, area->y, offset_x, offset_y);
    for (int64_t row = area->start_row;
         row < area->start_row + area->tiles_down; row++) {
      for (int64_t col = area->start_col;
           col < area->start_col + area->tiles_across; col++) {
        _openslide_grid_tilemap_add_tile(grid,
                                         col, row,
                                         offset_x, offset_y,
                                         subtile_w, subtile_h,
                                         NULL);
      }
    }
  }

  return grid;
}

static void set_region_props(openslide_t *osr, struct bif *bif,
                             struct level *level0) {
  for (int32_t i = 0; i < bif->num_areas; i++) {
    struct area *area = bif->areas[i];
    g_hash_table_insert(osr->properties,
                        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_X, i),
                        g_strdup_printf("%"PRId64, (int64_t) (bif->tile_advance_x * area->start_col)));
    g_hash_table_insert(osr->properties,
                        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_Y, i),
                        g_strdup_printf("%"PRId64, (int64_t) (bif->tile_advance_y * area->start_row)));
    g_hash_table_insert(osr->properties,
                        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_WIDTH, i),
                        g_strdup_printf("%"PRId64, (int64_t) ceil(bif->tile_advance_x * (area->tiles_across - 1) + level0->tiffl.tile_w)));
    g_hash_table_insert(osr->properties,
                        g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_REGION_HEIGHT, i),
                        g_strdup_printf("%"PRId64, (int64_t) ceil(bif->tile_advance_y * (area->tiles_down - 1) + level0->tiffl.tile_h)));
  }
}

static bool ventana_open(openslide_t *osr, const char *filename,
                         struct _openslide_tifflike *tl,
                         struct _openslide_hash *quickhash1, GError **err) {
  GPtrArray *level_array = g_ptr_array_new();
  struct bif *bif = NULL;
  GError *tmp_err = NULL;

  // open TIFF
  struct _openslide_tiffcache *tc = _openslide_tiffcache_create(filename);
  TIFF *tiff = _openslide_tiffcache_get(tc, err);
  if (!tiff) {
    goto FAIL;
  }

  // parse initial XML
  const char *xml = _openslide_tifflike_get_buffer(tl, 0, TIFFTAG_XMLPACKET,
                                                   err);
  if (!xml) {
    goto FAIL;
  }
  if (!parse_initial_xml(osr, xml, err)) {
    goto FAIL;
  }

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
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unexpected encounter with level %"PRId64, level);
        goto FAIL;
      }
      if (magnification >= prev_magnification) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unexpected magnification in level %"PRId64, level);
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
        // get XML
        xml = _openslide_tifflike_get_buffer(tl, dir, TIFFTAG_XMLPACKET,
                                             &tmp_err);
        if (xml) {
          // get tile size
          struct _openslide_tiff_level tiffl;
          if (!_openslide_tiff_level_init(tiff, dir, NULL, &tiffl, err)) {
            goto FAIL;
          }
          // parse
          bif = parse_level0_xml(xml, tiffl.tile_w, tiffl.tile_h, err);
          if (!bif) {
            goto FAIL;
          }
        } else if (g_error_matches(tmp_err, OPENSLIDE_ERROR,
                                   OPENSLIDE_ERROR_NO_VALUE)) {
          // Ventana TIFF (no AOIs or overlaps)
          g_clear_error(&tmp_err);
        } else {
          g_propagate_error(err, tmp_err);
          goto FAIL;
        }
      }

      // confirm that this directory is tiled
      if (!TIFFIsTiled(tiff)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Directory %d is not tiled", dir);
        goto FAIL;
      }

      // verify that we can read this compression (hard fail if not)
      uint16_t compression;
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Can't read compression scheme");
        goto FAIL;
      };
      if (!TIFFIsCODECConfigured(compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unsupported TIFF compression: %u", compression);
        goto FAIL;
      }

      // create level
      struct level *l = g_slice_new0(struct level);
      struct _openslide_tiff_level *tiffl = &l->tiffl;
      if (!_openslide_tiff_level_init(tiff, dir,
                                      &l->base, tiffl,
                                      err)) {
        g_slice_free(struct level, l);
        goto FAIL;
      }
      struct level *level0 = l;
      if (level > 0) {
        level0 = level_array->pdata[0];
      }
      l->base.downsample = downsample;
      if (bif) {
        l->grid = create_bif_grid(osr, bif,
                                  downsample,
                                  tiffl->tile_w, tiffl->tile_h);
        l->subtiles_per_tile = downsample;
        // the format doesn't seem to record the level size, so make it
        // large enough for all the pixels
        double x, y, w, h;
        _openslide_grid_get_bounds(l->grid, &x, &y, &w, &h);
        l->base.w = ceil(x + w);
        l->base.h = ceil(y + h);
        // clear tile size hints set by _openslide_tiff_level_init()
        l->base.tile_w = 0;
        l->base.tile_h = 0;
      } else {
        l->grid = _openslide_grid_create_simple(osr,
                                                tiffl->tiles_across,
                                                tiffl->tiles_down,
                                                tiffl->tile_w,
                                                tiffl->tile_h,
                                                read_subtile);
        l->subtiles_per_tile = 1;
      }
      //g_debug("level %"PRId64": magnification %g, downsample %g, size %"PRId64" %"PRId64, level, magnification, downsample, l->base.w, l->base.h);

      // add to array
      g_ptr_array_add(level_array, l);

      // verify consistent tile sizes
      // our math is all based on level 0 tile sizes, but
      // _openslide_tiff_read_tile() uses the directory's tile size
      if (l->tiffl.tile_w != level0->tiffl.tile_w ||
          l->tiffl.tile_h != level0->tiffl.tile_h) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Inconsistent TIFF tile sizes");
        goto FAIL;
      }

    } else if (!strcmp(image_desc, MACRO_DESCRIPTION) ||
               !strcmp(image_desc, MACRO_DESCRIPTION2)) {
      // macro image
      if (!_openslide_tiff_add_associated_image(osr, "macro", tc, dir,
                                                err)) {
	goto FAIL;
      }

    } else if (!strcmp(image_desc, THUMBNAIL_DESCRIPTION)) {
      // thumbnail image
      if (!_openslide_tiff_add_associated_image(osr, "thumbnail", tc, dir,
                                                err)) {
	goto FAIL;
      }
    }
  } while (TIFFReadDirectory(tiff));

  // sort tiled levels
  g_ptr_array_sort(level_array, width_compare);

  // get level 0
  if (level_array->len == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No pyramid levels in slide");
    goto FAIL;
  }
  struct level *level0 = level_array->pdata[0];

  // set region properties
  if (bif) {
    set_region_props(osr, bif, level0);
  }

  // free bif info
  bif_free(bif);
  bif = NULL;

  // set hash and TIFF properties
  struct level *top_level = level_array->pdata[level_array->len - 1];
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    top_level->tiffl.dir,
                                                    level0->tiffl.dir,
                                                    err)) {
    goto FAIL;
  }

  // unwrap level array
  int32_t level_count = level_array->len;
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // allocate private data
  struct ventana_ops_data *data = g_slice_new0(struct ventana_ops_data);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &ventana_ops;

  // put TIFF handle and store tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  return true;

FAIL:
  // free bif info
  bif_free(bif);
  // free the level array
  if (level_array) {
    for (uint32_t n = 0; n < level_array->len; n++) {
      struct level *l = level_array->pdata[n];
      _openslide_grid_destroy(l->grid);
      g_slice_free(struct level, l);
    }
    g_ptr_array_free(level_array, true);
  }
  // free TIFF
  _openslide_tiffcache_put(tc, tiff);
  _openslide_tiffcache_destroy(tc);
  return false;
}

const struct _openslide_format _openslide_format_ventana = {
  .name = "ventana",
  .vendor = "ventana",
  .detect = ventana_detect,
  .open = ventana_open,
};
