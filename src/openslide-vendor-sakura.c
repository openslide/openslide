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
 * Sakura (svslide) support
 *
 * quickhash comes from a selection of metadata fields, the binary header
 * blob, and the lowest-resolution level
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-sqlite.h"
#include "openslide-hash.h"

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <string.h>

static const char MAGIC_BYTES[] = "SVGigaPixelImage";

static const struct property {
  const char *table;
  const char *column;
  int type;
} property_table[] = {
  {"SVSlideDataXPO", "SlideId", SQLITE_TEXT},
  {"SVSlideDataXPO", "Date", SQLITE_TEXT},
  {"SVSlideDataXPO", "Description", SQLITE_TEXT},
  {"SVSlideDataXPO", "Creator", SQLITE_TEXT},
  {"SVSlideDataXPO", "DiagnosisCode", SQLITE_TEXT},
  {"SVSlideDataXPO", "Keywords", SQLITE_TEXT},
  {"SVHRScanDataXPO", "ScanId", SQLITE_TEXT},
  {"SVHRScanDataXPO", "ResolutionMmPerPix", SQLITE_FLOAT},
  {"SVHRScanDataXPO", "NominalLensMagnification", SQLITE_FLOAT},
  {"SVHRScanDataXPO", "FocussingMethod", SQLITE_TEXT},
};

enum color_indexes {
  INDEX_RED = 0,
  INDEX_GREEN = 1,
  INDEX_BLUE = 2,
};

#define PREPARE_OR_FAIL(DEST, DB, SQL) do {				\
    DEST = _openslide_sqlite_prepare(DB, SQL, err);			\
    if (!DEST) {							\
      goto FAIL;							\
    }									\
  } while (0)

#define BIND_TEXT_OR_FAIL(STMT, INDEX, STR) do {			\
    if (sqlite3_bind_text(STMT, INDEX, STR, -1, SQLITE_TRANSIENT)) {	\
      _openslide_sqlite_propagate_stmt_error(STMT, err);		\
      goto FAIL;							\
    }									\
  } while (0)

#define STEP_OR_FAIL(STMT) do {						\
    if (!_openslide_sqlite_step(STMT, err)) {				\
      goto FAIL;							\
    }									\
  } while (0)

struct sakura_ops_data {
  char *filename;
  char *data_sql;
  int32_t tile_size;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
};

struct tile {
  char *id_red;
  char *id_green;
  char *id_blue;
};

struct associated_image {
  struct _openslide_associated_image base;
  char *filename;
  char *data_sql;
};

struct ensure_components_args {
  GError *err;
  double downsample;
};

static char *get_quoted_unique_table_name(sqlite3 *db, GError **err) {
  sqlite3_stmt *stmt;
  char *result = NULL;

  PREPARE_OR_FAIL(stmt, db, "SELECT quote(TableName) FROM "
                  "DataManagerSQLiteConfigXPO");
  STEP_OR_FAIL(stmt);
  result = g_strdup((const char *) sqlite3_column_text(stmt, 0));

  // we only expect to find one row
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Found > 1 unique tables");
    g_free(result);
    result = NULL;
  }

FAIL:
  sqlite3_finalize(stmt);
  return result;
}

static bool sakura_detect(const char *filename,
                          struct _openslide_tifflike *tl, GError **err) {
  sqlite3_stmt *stmt = NULL;
  char *unique_table_name = NULL;
  char *sql = NULL;
  bool result = false;

  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // open database
  sqlite3 *db = _openslide_sqlite_open(filename, err);
  if (!db) {
    return false;
  }

  // get name of unique table
  unique_table_name = get_quoted_unique_table_name(db, err);
  if (!unique_table_name) {
    goto FAIL;
  }

  // check ++MagicBytes from unique table
  sql = g_strdup_printf("SELECT data FROM %s WHERE id = '++MagicBytes'",
                        unique_table_name);
  PREPARE_OR_FAIL(stmt, db, sql);
  STEP_OR_FAIL(stmt);
  const char *magic = (const char *) sqlite3_column_text(stmt, 0);
  if (strcmp(magic, MAGIC_BYTES)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Magic number does not match");
    goto FAIL;
  }

  result = true;

FAIL:
  sqlite3_finalize(stmt);
  g_free(sql);
  g_free(unique_table_name);
  _openslide_sqlite_close(db);
  return result;
}

static void tile_free(struct tile *tile) {
  g_free(tile->id_red);
  g_free(tile->id_green);
  g_free(tile->id_blue);
  g_slice_free(struct tile, tile);
}

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_slice_free(struct level, l);
}

static void destroy(openslide_t *osr) {
  struct sakura_ops_data *data = osr->data;
  g_free(data->filename);
  g_free(data->data_sql);
  g_slice_free(struct sakura_ops_data, data);

  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *) osr->levels[i]);
  }
  g_free(osr->levels);
}

static bool read_channel(uint8_t *channeldata,
                         const char *tileid,
                         int32_t tile_size,
                         sqlite3_stmt *stmt,
                         GError **err) {
  // retrieve compressed tile
  sqlite3_reset(stmt);
  BIND_TEXT_OR_FAIL(stmt, 1, tileid);
  STEP_OR_FAIL(stmt);
  const void *buf = sqlite3_column_blob(stmt, 0);
  int buflen = sqlite3_column_bytes(stmt, 0);

  // decompress
  return _openslide_jpeg_decode_buffer_gray(buf, buflen, channeldata,
                                            tile_size, tile_size, err);

FAIL:
  return false;
}

static bool read_image(uint32_t *tiledata,
                       const struct tile *tile,
                       int32_t tile_size,
                       sqlite3_stmt *stmt,
                       GError **err) {
  uint8_t *red_channel = g_slice_alloc(tile_size * tile_size);
  uint8_t *green_channel = g_slice_alloc(tile_size * tile_size);
  uint8_t *blue_channel = g_slice_alloc(tile_size * tile_size);
  bool success = false;

  if (!read_channel(red_channel, tile->id_red, tile_size, stmt, err)) {
    goto OUT;
  }
  if (!read_channel(green_channel, tile->id_green, tile_size, stmt, err)) {
    goto OUT;
  }
  if (!read_channel(blue_channel, tile->id_blue, tile_size, stmt, err)) {
    goto OUT;
  }

  for (int32_t i = 0; i < tile_size * tile_size; i++) {
    tiledata[i] = 0xff000000 |
                  (red_channel[i] << 16) |
                  (green_channel[i] << 8) |
                  blue_channel[i];
  }

  success = true;

OUT:
  g_slice_free1(tile_size * tile_size, red_channel);
  g_slice_free1(tile_size * tile_size, green_channel);
  g_slice_free1(tile_size * tile_size, blue_channel);
  return success;
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *_tile, void *arg,
                      GError **err) {
  struct sakura_ops_data *data = osr->data;
  struct level *l = (struct level *) level;
  struct tile *tile = _tile;
  sqlite3_stmt *stmt = arg;
  int32_t tile_size = data->tile_size;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tile_size * tile_size * 4);

    // read tile
    if (!read_image(tiledata, tile, tile_size, stmt, err)) {
      g_slice_free1(tile_size * tile_size * 4, tiledata);
      return false;
    }

    // clip, if necessary
    if (!_openslide_clip_tile(tiledata,
                              tile_size, tile_size,
                              l->base.w - tile_col * tile_size,
                              l->base.h - tile_row * tile_size,
                              err)) {
      g_slice_free1(tile_size * tile_size * 4, tiledata);
      return false;
    }

    // put it in the cache
    _openslide_cache_put(osr->cache,
			 level, tile_col, tile_row,
			 tiledata, tile_size * tile_size * 4,
			 &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 tile_size, tile_size,
                                                                 tile_size * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct sakura_ops_data *data = osr->data;
  struct level *l = (struct level *) level;
  sqlite3_stmt *stmt = NULL;
  bool success = false;

  sqlite3 *db = _openslide_sqlite_open(data->filename, err);
  if (!db) {
    return false;
  }
  PREPARE_OR_FAIL(stmt, db, data->data_sql);

  success = _openslide_grid_paint_region(l->grid, cr, stmt,
                                         x / l->base.downsample,
                                         y / l->base.downsample,
                                         level, w, h,
                                         err);

FAIL:
  sqlite3_finalize(stmt);
  _openslide_sqlite_close(db);
  return success;
}

static const struct _openslide_ops sakura_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dest,
                                      GError **err) {
  struct associated_image *img = (struct associated_image *) _img;
  bool success = false;

  //g_debug("read Sakura associated image: %s", img->data_sql);

  // open DB handle
  sqlite3 *db = _openslide_sqlite_open(img->filename, err);
  if (!db) {
    return false;
  }

  // read data
  sqlite3_stmt *stmt;
  PREPARE_OR_FAIL(stmt, db, img->data_sql);
  STEP_OR_FAIL(stmt);
  const void *buf = sqlite3_column_blob(stmt, 0);
  int buflen = sqlite3_column_bytes(stmt, 0);

  // decode it
  success = _openslide_jpeg_decode_buffer(buf, buflen, dest,
                                          img->base.w, img->base.h, err);

FAIL:
  sqlite3_finalize(stmt);
  _openslide_sqlite_close(db);
  return success;
}

static void destroy_associated_image(struct _openslide_associated_image *_img) {
  struct associated_image *img = (struct associated_image *) _img;

  g_free(img->filename);
  g_free(img->data_sql);
  g_slice_free(struct associated_image, img);
}

static const struct _openslide_associated_image_ops sakura_associated_ops = {
  .get_argb_data = get_associated_image_data,
  .destroy = destroy_associated_image,
};

static bool add_associated_image(openslide_t *osr,
                                 sqlite3 *db,
                                 const char *filename,
                                 const char *name,
                                 const char *data_sql,
                                 GError **err) {
  bool success = false;

  // read data
  sqlite3_stmt *stmt;
  PREPARE_OR_FAIL(stmt, db, data_sql);
  STEP_OR_FAIL(stmt);
  const void *buf = sqlite3_column_blob(stmt, 0);
  int buflen = sqlite3_column_bytes(stmt, 0);

  // read dimensions from JPEG header
  int32_t w, h;
  if (!_openslide_jpeg_decode_buffer_dimensions(buf, buflen, &w, &h, err)) {
    goto FAIL;
  }

  // ensure there is only one row
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Query returned multiple rows: %s", data_sql);
    goto FAIL;
  }

  // create struct
  struct associated_image *img = g_slice_new0(struct associated_image);
  img->base.ops = &sakura_associated_ops;
  img->base.w = w;
  img->base.h = h;
  img->filename = g_strdup(filename);
  img->data_sql = g_strdup(data_sql);

  // add it
  g_hash_table_insert(osr->associated_images, g_strdup(name), img);

  success = true;

FAIL:
  sqlite3_finalize(stmt);
  return success;
}

static gint compare_downsamples(const void *a, const void *b) {
  int64_t aa = *(const int64_t *) a;
  int64_t bb = *(const int64_t *) b;

  if (aa < bb) {
    return -1;
  } else if (aa > bb) {
    return 1;
  } else {
    return 0;
  }
}

static void ensure_components(struct _openslide_grid *grid G_GNUC_UNUSED,
                              int64_t tile_col, int64_t tile_row,
                              void *_tile, void *arg) {
  struct tile *tile = _tile;
  struct ensure_components_args *args = arg;

  if (args->err) {
    return;
  }
  if (!tile->id_red || !tile->id_green || !tile->id_blue) {
    g_set_error(&args->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Missing color component: downsample %g, tile "
                "(%"G_GINT64_FORMAT", %"G_GINT64_FORMAT")",
                args->downsample, tile_col, tile_row);
  }
}

static bool read_header(sqlite3 *db, const char *unique_table_name,
                        int64_t *image_width, int64_t *image_height,
                        int32_t *_tile_size,
                        GError **err) {
  GInputStream *strm = NULL;
  GDataInputStream *dstrm = NULL;
  GError *tmp_err = NULL;
  bool success = false;

  // load header
  char *sql = g_strdup_printf("SELECT data FROM %s WHERE id = 'Header'",
                              unique_table_name);
  sqlite3_stmt *stmt;
  PREPARE_OR_FAIL(stmt, db, sql);
  STEP_OR_FAIL(stmt);
  const void *buf = sqlite3_column_blob(stmt, 0);
  const int buflen = sqlite3_column_bytes(stmt, 0);
  if (!buf) {
    buf = "";
  }

  // create data stream
  strm = g_memory_input_stream_new_from_data(buf, buflen, NULL);
  dstrm = g_data_input_stream_new(strm);
  g_data_input_stream_set_byte_order(dstrm,
                                     G_DATA_STREAM_BYTE_ORDER_LITTLE_ENDIAN);

  // read fields
  uint32_t tile_size = g_data_input_stream_read_uint32(dstrm, NULL, &tmp_err);
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    goto FAIL;
  }
  if (tile_size == 0 || tile_size > INT32_MAX) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid tile size: %u", tile_size);
    goto FAIL;
  }
  uint32_t w = g_data_input_stream_read_uint32(dstrm, NULL, &tmp_err);
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    goto FAIL;
  }
  uint32_t h = g_data_input_stream_read_uint32(dstrm, NULL, &tmp_err);
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    goto FAIL;
  }

  // commit
  *image_width = w;
  *image_height = h;
  *_tile_size = tile_size;
  success = true;

FAIL:
  if (dstrm) {
    g_object_unref(dstrm);
  }
  if (strm) {
    g_object_unref(strm);
  }
  sqlite3_finalize(stmt);
  g_free(sql);
  return success;
}

static void add_properties(openslide_t *osr,
                           sqlite3 *db,
                           const char *unique_table_name) {
  // build unified query
  GString *query = g_string_new("SELECT ");
  for (uint32_t i = 0; i < G_N_ELEMENTS(property_table); i++) {
    const struct property *prop = &property_table[i];
    g_string_append_printf(query, "%s%s.%s",
                           i ? ", " : "",
                           prop->table,
                           prop->column);
  }
  g_string_append(query, " FROM SVSlideDataXPO JOIN SVHRScanDataXPO ON "
                  "SVHRScanDataXPO.ParentSlide == SVSlideDataXPO.OID");
  char *sql = g_string_free(query, false);
  //g_debug("%s", sql);

  // execute it
  sqlite3_stmt *stmt = _openslide_sqlite_prepare(db, sql, NULL);
  if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
    // add properties
    for (uint32_t i = 0; i < G_N_ELEMENTS(property_table); i++) {
      const struct property *prop = &property_table[i];
      switch (prop->type) {
      case SQLITE_TEXT: {
        const char *value = (const char *) sqlite3_column_text(stmt, i);
        if (value[0]) {
          g_hash_table_insert(osr->properties,
                              g_strdup_printf("sakura.%s", prop->column),
                              g_strdup(value));
        }
        break;
      }
      case SQLITE_FLOAT: {
        // convert to text ourselves to ensure full precision
        double value = sqlite3_column_double(stmt, i);
        g_hash_table_insert(osr->properties,
                            g_strdup_printf("sakura.%s", prop->column),
                            _openslide_format_double(value));
        break;
      }
      default:
        g_assert_not_reached();
      }
    }
  }
  sqlite3_finalize(stmt);
  g_free(sql);

  // set MPP and objective power
  stmt = _openslide_sqlite_prepare(db, "SELECT ResolutionMmPerPix FROM "
                                   "SVHRScanDataXPO JOIN SVSlideDataXPO ON "
                                   "SVHRScanDataXPO.ParentSlide = "
                                   "SVSlideDataXPO.OID", NULL);
  if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
    double mmpp = sqlite3_column_double(stmt, 0);
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                        _openslide_format_double(mmpp * 1000));
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                        _openslide_format_double(mmpp * 1000));
  }
  sqlite3_finalize(stmt);
  _openslide_duplicate_double_prop(osr, "sakura.NominalLensMagnification",
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);

  // add version property
  sql = g_strdup_printf("SELECT data FROM %s WHERE id = '++VersionBytes'",
                        unique_table_name);
  stmt = _openslide_sqlite_prepare(db, sql, NULL);
  if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *version = (const char *) sqlite3_column_text(stmt, 0);
    g_hash_table_insert(osr->properties,
                        g_strdup("sakura.VersionBytes"),
                        g_strdup(version));
  }
  sqlite3_finalize(stmt);
  g_free(sql);
}

static bool hash_columns(struct _openslide_hash *quickhash1,
                         sqlite3 *db,
                         const char *sql,
                         GError **err) {
  bool success = false;

  //g_debug("%s", sql);

  sqlite3_stmt *stmt;
  PREPARE_OR_FAIL(stmt, db, sql);
  int ret;
  while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
    for (int i = 0; i < sqlite3_column_count(stmt); i++) {
      const void *data = sqlite3_column_blob(stmt, i);
      int datalen = sqlite3_column_bytes(stmt, i);

      //g_debug("hash: %d bytes", datalen);
      _openslide_hash_data(quickhash1, data, datalen);
      _openslide_hash_data(quickhash1, "", 1);
    }
  }
  if (ret != SQLITE_DONE) {
    _openslide_sqlite_propagate_error(db, err);
    goto FAIL;
  }

  success = true;

FAIL:
  sqlite3_finalize(stmt);
  return success;
}

static void gather_tileids(struct _openslide_grid *grid G_GNUC_UNUSED,
                           int64_t tile_col G_GNUC_UNUSED,
                           int64_t tile_row G_GNUC_UNUSED,
                           void *_tile, void *arg) {
  struct tile *tile = _tile;
  GList **list = arg;

  *list = g_list_prepend(*list, tile->id_red);
  *list = g_list_prepend(*list, tile->id_green);
  *list = g_list_prepend(*list, tile->id_blue);
}

static gint compare_tileids(const void *a, const void *b) {
  return strcmp(a, b);
}

static bool hash_tiles(struct _openslide_hash *quickhash1,
                       sqlite3 *db,
                       struct level *l,
                       const char *unique_table_name,
                       GError **err) {
  bool success = false;

  // collect tile IDs
  GList *tileids = NULL;
  _openslide_grid_tilemap_foreach(l->grid, gather_tileids, &tileids);
  tileids = g_list_sort(tileids, compare_tileids);

  // prepare query
  char *sql = g_strdup_printf("SELECT data from %s WHERE id = ?",
                              unique_table_name);
  sqlite3_stmt *stmt;
  PREPARE_OR_FAIL(stmt, db, sql);

  // hash tiles
  for (GList *cur = tileids; cur; cur = cur->next) {
    sqlite3_reset(stmt);
    BIND_TEXT_OR_FAIL(stmt, 1, cur->data);
    STEP_OR_FAIL(stmt);
    const void *data = sqlite3_column_blob(stmt, 0);
    int datalen = sqlite3_column_bytes(stmt, 0);

    //g_debug("hash %s: %d bytes", (const char *) cur->data, datalen);
    _openslide_hash_data(quickhash1, data, datalen);
  }

  success = true;

FAIL:
  sqlite3_finalize(stmt);
  g_free(sql);
  g_list_free(tileids);
  return success;
}

static void compute_quickhash1(struct _openslide_hash *quickhash1,
                               sqlite3 *db,
                               struct level *l,
                               const char *unique_table_name) {
  if (!hash_columns(quickhash1, db, "SELECT SlideId, Date, Creator, "
                    "Description, Keywords FROM SVSlideDataXPO "
                    "ORDER BY OID", NULL)) {
    goto FAIL;
  }
  if (!hash_columns(quickhash1, db, "SELECT ScanId, Date, Name, Description "
                    "FROM SVHRScanDataXPO ORDER BY OID", NULL)) {
    goto FAIL;
  }

  // header blob
  char *sql = g_strdup_printf("SELECT data FROM %s WHERE id = 'Header' "
                              "ORDER BY rowid", unique_table_name);
  bool success = hash_columns(quickhash1, db, sql, NULL);
  g_free(sql);
  if (!success) {
    goto FAIL;
  }

  // tiles in lowest-resolution level
  if (!hash_tiles(quickhash1, db, l, unique_table_name, NULL)) {
    goto FAIL;
  }

  return;

FAIL:
  _openslide_hash_disable(quickhash1);
}

static bool sakura_open(openslide_t *osr, const char *filename,
                        struct _openslide_tifflike *tl G_GNUC_UNUSED,
                        struct _openslide_hash *quickhash1, GError **err) {
  struct level **levels = NULL;
  int32_t level_count = 0;
  char *unique_table_name = NULL;
  sqlite3_stmt *stmt = NULL;
  GHashTable *level_hash =
    g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                          (GDestroyNotify) destroy_level);
  bool success = false;

  // open database
  sqlite3 *db = _openslide_sqlite_open(filename, err);
  if (!db) {
    goto FAIL;
  }

  // get unique table name
  unique_table_name = get_quoted_unique_table_name(db, err);
  if (!unique_table_name) {
    goto FAIL;
  }

  // read header
  int64_t image_width, image_height;
  int32_t tile_size;
  if (!read_header(db, unique_table_name,
                   &image_width, &image_height,
                   &tile_size, err)) {
    goto FAIL;
  }

  // create levels
  // copy tile table into tilemap grid, since the table has no useful indexes
  PREPARE_OR_FAIL(stmt, db, "SELECT PYRAMIDLEVEL, COLUMNINDEX, ROWINDEX, "
                  "COLORINDEX, TILEID FROM tile");
  int ret;
  while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
    int64_t downsample = sqlite3_column_int64(stmt, 0);
    int64_t x = sqlite3_column_int64(stmt, 1);
    int64_t y = sqlite3_column_int64(stmt, 2);
    int color = sqlite3_column_int(stmt, 3);
    const char *tileid = (const char *) sqlite3_column_text(stmt, 4);

    // get or create level
    struct level *l = g_hash_table_lookup(level_hash, &downsample);
    if (!l) {
      // ensure downsample is > 0 and a power of 2
      if (downsample <= 0 || (downsample & (downsample - 1))) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Invalid downsample %"G_GINT64_FORMAT, downsample);
        goto FAIL;
      }

      l = g_slice_new0(struct level);
      l->base.downsample = downsample;
      l->base.w = image_width / downsample;
      l->base.h = image_height / downsample;
      l->base.tile_w = tile_size;
      l->base.tile_h = tile_size;
      l->grid = _openslide_grid_create_tilemap(osr, tile_size, tile_size,
                                               read_tile,
                                               (GDestroyNotify) tile_free);
      int64_t *downsample_val = g_new(int64_t, 1);
      *downsample_val = downsample;
      g_hash_table_insert(level_hash, downsample_val, l);
    }

    // get or create tile
    int64_t col = x / (tile_size * downsample);
    int64_t row = y / (tile_size * downsample);
    int64_t tw = MIN(tile_size, l->base.w - col * tile_size);
    int64_t th = MIN(tile_size, l->base.h - row * tile_size);
    struct tile *tile = _openslide_grid_tilemap_get_tile(l->grid, col, row);
    if (!tile) {
      tile = g_slice_new0(struct tile);
      _openslide_grid_tilemap_add_tile(l->grid, col, row, 0, 0, tw, th, tile);
    }

    // store tileid
    char **id_field;
    switch (color) {
    case INDEX_RED:
      id_field = &tile->id_red;
      break;
    case INDEX_GREEN:
      id_field = &tile->id_green;
      break;
    case INDEX_BLUE:
      id_field = &tile->id_blue;
      break;
    default:
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unknown tile color: %d", color);
      goto FAIL;
    }
    if (*id_field) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Collision in tile table: %"G_GINT64_FORMAT
                  "/%"G_GINT64_FORMAT"/%"G_GINT64_FORMAT"/%d",
                  downsample, x, y, color);
      goto FAIL;
    }
    *id_field = g_strdup(tileid);
  }
  if (ret != SQLITE_DONE) {
    _openslide_sqlite_propagate_error(db, err);
    goto FAIL;
  }
  sqlite3_finalize(stmt);
  stmt = NULL;

  // move levels to level array
  level_count = g_hash_table_size(level_hash);
  if (level_count == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't find any tiles");
    goto FAIL;
  }
  levels = g_new(struct level *, level_count);
  GList *keys = g_hash_table_get_keys(level_hash);
  keys = g_list_sort(keys, compare_downsamples);
  int32_t i = 0;
  for (GList *cur = keys; cur; cur = cur->next) {
    levels[i] = g_hash_table_lookup(level_hash, cur->data);
    g_assert(levels[i]);
    g_hash_table_steal(level_hash, cur->data);
    g_free(cur->data);
    i++;
  }
  g_list_free(keys);

  // ensure all tiles have all components
  for (i = 0; i < level_count; i++) {
    struct level *l = levels[i];
    struct ensure_components_args args = {
      .downsample = l->base.downsample,
    };
    _openslide_grid_tilemap_foreach(l->grid, ensure_components, &args);
    if (args.err) {
      g_propagate_error(err, args.err);
      goto FAIL;
    }
  }

  // add properties
  add_properties(osr, db, unique_table_name);

  // add associated images
  // errors are non-fatal
  add_associated_image(osr, db, filename, "label",
                       "SELECT Image FROM SVScannedImageDataXPO JOIN "
                       "SVSlideDataXPO ON SVSlideDataXPO.m_labelScan = "
                       "SVScannedImageDataXPO.OID", NULL);
  add_associated_image(osr, db, filename, "macro",
                       "SELECT Image FROM SVScannedImageDataXPO JOIN "
                       "SVSlideDataXPO ON SVSlideDataXPO.m_overviewScan = "
                       "SVScannedImageDataXPO.OID", NULL);
  add_associated_image(osr, db, filename, "thumbnail",
                       "SELECT ThumbnailImage FROM SVHRScanDataXPO JOIN "
                       "SVSlideDataXPO ON SVHRScanDataXPO.ParentSlide = "
                       "SVSlideDataXPO.OID", NULL);

  // compute quickhash
  compute_quickhash1(quickhash1, db,
                     levels[level_count - 1], unique_table_name);

  // build ops data
  struct sakura_ops_data *data = g_slice_new0(struct sakura_ops_data);
  data->filename = g_strdup(filename);
  data->data_sql =
    g_strdup_printf("SELECT data FROM %s WHERE id=?", unique_table_name);
  data->tile_size = tile_size;

  // commit
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &sakura_ops;

  success = true;

FAIL:
  if (!success) {
    for (int32_t i = 0; i < level_count; i++) {
      destroy_level(levels[i]);
    }
    g_free(levels);
  }

  sqlite3_finalize(stmt);
  _openslide_sqlite_close(db);
  g_hash_table_destroy(level_hash);
  g_free(unique_table_name);
  return success;
}

const struct _openslide_format _openslide_format_sakura = {
  .name = "sakura",
  .vendor = "sakura",
  .detect = sakura_detect,
  .open = sakura_open,
};
