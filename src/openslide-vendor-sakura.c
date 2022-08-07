/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2022 Benjamin Gilbert
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
#include <errno.h>

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

enum color_index {
  INDEX_RED = 0,
  INDEX_GREEN = 1,
  INDEX_BLUE = 2,
  NUM_INDEXES = 3,
};

#define PREPARE_OR_RETURN(DEST, DB, SQL, RET) do {			\
    DEST = _openslide_sqlite_prepare(DB, SQL, err);			\
    if (!DEST) {							\
      return RET;							\
    }									\
  } while (0)

#define BIND_TEXT_OR_RETURN(STMT, INDEX, STR, RET) do {			\
    if (sqlite3_bind_text(STMT, INDEX, STR, -1, SQLITE_TRANSIENT)) {	\
      _openslide_sqlite_propagate_stmt_error(STMT, err);		\
      return RET;							\
    }									\
  } while (0)

#define STEP_OR_RETURN(STMT, RET) do {					\
    if (!_openslide_sqlite_step(STMT, err)) {				\
      return RET;							\
    }									\
  } while (0)

struct sakura_ops_data {
  char *filename;
  char *data_sql;
  int32_t tile_size;
  int32_t focal_plane;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;
};

struct associated_image {
  struct _openslide_associated_image base;
  char *filename;
  char *data_sql;
};

static char *get_quoted_unique_table_name(sqlite3 *db, GError **err) {
  g_autoptr(sqlite3_stmt) stmt = NULL;
  PREPARE_OR_RETURN(stmt, db, "SELECT quote(TableName) FROM "
                  "DataManagerSQLiteConfigXPO", NULL);
  STEP_OR_RETURN(stmt, NULL);
  g_autofree char *result =
    g_strdup((const char *) sqlite3_column_text(stmt, 0));

  // we only expect to find one row
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Found > 1 unique tables");
    return NULL;
  }

  return g_steal_pointer(&result);
}

static bool sakura_detect(const char *filename,
                          struct _openslide_tifflike *tl, GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // open database
  g_autoptr(sqlite3) db = _openslide_sqlite_open(filename, err);
  if (!db) {
    return false;
  }

  // get name of unique table
  g_autofree char *unique_table_name = get_quoted_unique_table_name(db, err);
  if (!unique_table_name) {
    return false;
  }

  // check ++MagicBytes from unique table
  g_autofree char *sql =
    g_strdup_printf("SELECT data FROM %s WHERE id = '++MagicBytes'",
                    unique_table_name);
  g_autoptr(sqlite3_stmt) stmt = NULL;
  PREPARE_OR_RETURN(stmt, db, sql, false);
  STEP_OR_RETURN(stmt, false);
  const char *magic = (const char *) sqlite3_column_text(stmt, 0);
  if (strcmp(magic, MAGIC_BYTES)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Magic number does not match");
    return false;
  }

  return true;
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

static char *make_tileid(int64_t x, int64_t y,
                         int64_t downsample,
                         enum color_index color,
                         int32_t focal_plane) {
  // T;x|y;downsample;color;0
  return g_strdup_printf("T;%"PRId64"|%"PRId64";%"PRId64";%d;%d",
                         x, y, downsample, color, focal_plane);
}

static bool _parse_tileid_column(const char *tileid, const char *col,
                                 int64_t *result,
                                 GError **err) {
  char *endptr;
  errno = 0;
  int64_t val = g_ascii_strtoll(col, &endptr, 10);
  if (*col == 0 || *endptr != 0 || errno == ERANGE || val < 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Bad field value in tile ID %s", tileid);
    return false;
  }
  *result = val;
  return true;
}

static bool parse_tileid(const char *tileid,
                         int64_t *_x, int64_t *_y,
                         int64_t *_downsample,
                         enum color_index *_color,
                         int32_t *_focal_plane,
                         GError **err) {
  // preliminary checks
  if (!g_str_has_prefix(tileid, "T;") || // not a tile
      g_str_has_suffix(tileid, "#")) {   // hash of a tile
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_NO_VALUE,
                "Not a tile ID");
    return false;
  }

  // parse and check fields
  // T;x|y;downsample;color;0
  g_auto(GStrv) fields = g_strsplit_set(tileid, ";|", 0);
  if (g_strv_length(fields) != 6) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Bad field count in tile ID %s", tileid);
    return false;
  }
  int64_t x, y, downsample, color, focal_plane;
  if (!_parse_tileid_column(tileid, fields[1], &x, err) ||
      !_parse_tileid_column(tileid, fields[2], &y, err) ||
      !_parse_tileid_column(tileid, fields[3], &downsample, err) ||
      !_parse_tileid_column(tileid, fields[4], &color, err) ||
      !_parse_tileid_column(tileid, fields[5], &focal_plane, err)) {
    return false;
  }
  if (downsample < 1 || color >= NUM_INDEXES) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Bad field value in tile ID %s", tileid);
    return false;
  }

  // verify round trip (no leading zeros, etc.)
  g_autofree char *synth_tileid =
    make_tileid(x, y, downsample, color, focal_plane);
  if (strcmp(tileid, synth_tileid)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't round-trip tile ID %s", tileid);
    return false;
  }

  // commit
  if (_x) {
    *_x = x;
  }
  if (_y) {
    *_y = y;
  }
  if (_downsample) {
    *_downsample = downsample;
  }
  if (_color) {
    *_color = color;
  }
  if (_focal_plane) {
    *_focal_plane = focal_plane;
  }
  return true;
}

static bool read_channel(uint8_t *channeldata,
                         int64_t tile_col, int64_t tile_row,
                         int64_t downsample,
                         enum color_index color,
                         int32_t focal_plane,
                         int32_t tile_size,
                         sqlite3_stmt *stmt,
                         GError **err) {
  // compute tile id
  g_autofree char *tileid = make_tileid(tile_col * tile_size * downsample,
                                        tile_row * tile_size * downsample,
                                        downsample, color, focal_plane);

  // retrieve compressed tile
  sqlite3_reset(stmt);
  BIND_TEXT_OR_RETURN(stmt, 1, tileid, false);
  STEP_OR_RETURN(stmt, false);
  const void *buf = sqlite3_column_blob(stmt, 0);
  int buflen = sqlite3_column_bytes(stmt, 0);

  // decompress
  return _openslide_jpeg_decode_buffer_gray(buf, buflen, channeldata,
                                            tile_size, tile_size, err);
}

static bool read_image(uint32_t *tiledata,
                       int64_t tile_col, int64_t tile_row,
                       int64_t downsample,
                       int32_t focal_plane,
                       int32_t tile_size,
                       sqlite3_stmt *stmt,
                       GError **err) {
  g_auto(_openslide_slice) red_channel =
    _openslide_slice_alloc(tile_size * tile_size);
  g_auto(_openslide_slice) green_channel =
    _openslide_slice_alloc(tile_size * tile_size);
  g_auto(_openslide_slice) blue_channel =
    _openslide_slice_alloc(tile_size * tile_size);

  if (!read_channel(red_channel.p, tile_col, tile_row, downsample,
                    INDEX_RED, focal_plane, tile_size, stmt, err)) {
    return false;
  }
  if (!read_channel(green_channel.p, tile_col, tile_row, downsample,
                    INDEX_GREEN, focal_plane, tile_size, stmt, err)) {
    return false;
  }
  if (!read_channel(blue_channel.p, tile_col, tile_row, downsample,
                    INDEX_BLUE, focal_plane, tile_size, stmt, err)) {
    return false;
  }

  for (int32_t i = 0; i < tile_size * tile_size; i++) {
    tiledata[i] = 0xff000000 |
                  (((uint8_t *) red_channel.p)[i] << 16) |
                  (((uint8_t *) green_channel.p)[i] << 8) |
                  ((uint8_t *) blue_channel.p)[i];
  }

  return true;
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *arg,
                      GError **err) {
  struct sakura_ops_data *data = osr->data;
  struct level *l = (struct level *) level;
  sqlite3_stmt *stmt = arg;
  int32_t tile_size = data->tile_size;
  GError *tmp_err = NULL;

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    g_auto(_openslide_slice) box =
      _openslide_slice_alloc(tile_size * tile_size * 4);

    // read tile
    if (!read_image(box.p, tile_col, tile_row, l->base.downsample,
                    data->focal_plane, tile_size, stmt, &tmp_err)) {
      if (g_error_matches(tmp_err, OPENSLIDE_ERROR,
                          OPENSLIDE_ERROR_NO_VALUE)) {
        // no such tile
        g_clear_error(&tmp_err);
        return true;
      } else {
        g_propagate_error(err, tmp_err);
        return false;
      }
    }

    // clip, if necessary
    if (!_openslide_clip_tile(box.p,
                              tile_size, tile_size,
                              l->base.w - tile_col * tile_size,
                              l->base.h - tile_row * tile_size,
                              err)) {
      return false;
    }

    // put it in the cache
    tiledata = _openslide_slice_steal(&box);
    _openslide_cache_put(osr->cache,
			 level, tile_col, tile_row,
			 tiledata, tile_size * tile_size * 4,
			 &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_ARGB32,
                                        tile_size, tile_size, tile_size * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct sakura_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  g_autoptr(sqlite3) db = _openslide_sqlite_open(data->filename, err);
  if (!db) {
    return false;
  }
  g_autoptr(sqlite3_stmt) stmt = NULL;
  PREPARE_OR_RETURN(stmt, db, data->data_sql, false);

  return _openslide_grid_paint_region(l->grid, cr, stmt,
                                      x / l->base.downsample,
                                      y / l->base.downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops sakura_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dest,
                                      GError **err) {
  struct associated_image *img = (struct associated_image *) _img;

  //g_debug("read Sakura associated image: %s", img->data_sql);

  // open DB handle
  g_autoptr(sqlite3) db = _openslide_sqlite_open(img->filename, err);
  if (!db) {
    return false;
  }

  // read data
  g_autoptr(sqlite3_stmt) stmt = NULL;
  PREPARE_OR_RETURN(stmt, db, img->data_sql, false);
  STEP_OR_RETURN(stmt, false);
  const void *buf = sqlite3_column_blob(stmt, 0);
  int buflen = sqlite3_column_bytes(stmt, 0);

  // decode it
  return _openslide_jpeg_decode_buffer(buf, buflen, dest,
                                       img->base.w, img->base.h, err);
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
  // read data
  g_autoptr(sqlite3_stmt) stmt = NULL;
  PREPARE_OR_RETURN(stmt, db, data_sql, false);
  STEP_OR_RETURN(stmt, false);
  const void *buf = sqlite3_column_blob(stmt, 0);
  int buflen = sqlite3_column_bytes(stmt, 0);

  // read dimensions from JPEG header
  int32_t w, h;
  if (!_openslide_jpeg_decode_buffer_dimensions(buf, buflen, &w, &h, err)) {
    return false;
  }

  // ensure there is only one row
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Query returned multiple rows: %s", data_sql);
    return false;
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

  return true;
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

static bool read_header(sqlite3 *db, const char *unique_table_name,
                        int64_t *image_width, int64_t *image_height,
                        int32_t *_tile_size, int32_t *_focal_planes,
                        GError **err) {
  // load header
  g_autofree char *sql =
    g_strdup_printf("SELECT data FROM %s WHERE id = 'Header'",
                    unique_table_name);
  g_autoptr(sqlite3_stmt) stmt = NULL;
  PREPARE_OR_RETURN(stmt, db, sql, false);
  STEP_OR_RETURN(stmt, false);
  const void *buf = sqlite3_column_blob(stmt, 0);
  const int buflen = sqlite3_column_bytes(stmt, 0);
  if (!buf) {
    buf = "";
  }

  // create data stream
  g_autoptr(GInputStream) strm =
    g_memory_input_stream_new_from_data(buf, buflen, NULL);
  g_autoptr(GDataInputStream) dstrm = g_data_input_stream_new(strm);
  g_data_input_stream_set_byte_order(dstrm,
                                     G_DATA_STREAM_BYTE_ORDER_LITTLE_ENDIAN);

  // read fields
  GError *tmp_err = NULL;
  uint32_t tile_size = g_data_input_stream_read_uint32(dstrm, NULL, &tmp_err);
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    return false;
  }
  if (tile_size == 0 || tile_size > INT32_MAX) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid tile size: %u", tile_size);
    return false;
  }
  uint32_t w = g_data_input_stream_read_uint32(dstrm, NULL, &tmp_err);
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    return false;
  }
  uint32_t h = g_data_input_stream_read_uint32(dstrm, NULL, &tmp_err);
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    return false;
  }
  if (!g_seekable_seek(G_SEEKABLE(dstrm), 16, G_SEEK_SET, NULL, err)) {
    return false;
  }
  uint32_t focal_planes = g_data_input_stream_read_uint32(dstrm, NULL,
                                                          &tmp_err);
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    return false;
  }

  // commit
  *image_width = w;
  *image_height = h;
  *_tile_size = tile_size;
  *_focal_planes = focal_planes;
  return true;
}

static void add_properties(openslide_t *osr,
                           sqlite3 *db,
                           const char *unique_table_name) {
  {
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
    g_autofree char *sql = g_string_free(query, false);
    //g_debug("%s", sql);

    // execute it
    g_autoptr(sqlite3_stmt) stmt = _openslide_sqlite_prepare(db, sql, NULL);
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
  }

  {
    // set MPP and objective power
    g_autoptr(sqlite3_stmt) stmt =
      _openslide_sqlite_prepare(db, "SELECT ResolutionMmPerPix FROM "
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
    _openslide_duplicate_double_prop(osr, "sakura.NominalLensMagnification",
                                     OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  }

  {
    // add version property
    g_autofree char *sql =
      g_strdup_printf("SELECT data FROM %s WHERE id = '++VersionBytes'",
                      unique_table_name);
    g_autoptr(sqlite3_stmt) stmt = _openslide_sqlite_prepare(db, sql, NULL);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
      const char *version = (const char *) sqlite3_column_text(stmt, 0);
      g_hash_table_insert(osr->properties,
                          g_strdup("sakura.VersionBytes"),
                          g_strdup(version));
    }
  }
}

static bool hash_columns(struct _openslide_hash *quickhash1,
                         sqlite3 *db,
                         const char *sql,
                         GError **err) {
  //g_debug("%s", sql);

  g_autoptr(sqlite3_stmt) stmt = NULL;
  PREPARE_OR_RETURN(stmt, db, sql, false);
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
    return false;
  }

  return true;
}

static gint compare_tileids(const void *a, const void *b,
                            void *data G_GNUC_UNUSED) {
  return strcmp(a, b);
}

static bool hash_tiles(struct _openslide_hash *quickhash1,
                       sqlite3 *db,
                       const char *unique_table_name,
                       GQueue *tileids,
                       GError **err) {
  // sort tile IDs
  g_queue_sort(tileids, compare_tileids, NULL);

  // prepare query
  g_autofree char *sql = g_strdup_printf("SELECT data from %s WHERE id = ?",
                                         unique_table_name);
  g_autoptr(sqlite3_stmt) stmt = NULL;
  PREPARE_OR_RETURN(stmt, db, sql, false);

  // hash tiles
  for (GList *cur = tileids->head; cur; cur = cur->next) {
    sqlite3_reset(stmt);
    BIND_TEXT_OR_RETURN(stmt, 1, cur->data, false);
    STEP_OR_RETURN(stmt, false);
    const void *data = sqlite3_column_blob(stmt, 0);
    int datalen = sqlite3_column_bytes(stmt, 0);

    //g_debug("hash %s: %d bytes", (const char *) cur->data, datalen);
    _openslide_hash_data(quickhash1, data, datalen);
  }

  return true;
}

static void compute_quickhash1(struct _openslide_hash *quickhash1,
                               sqlite3 *db,
                               const char *unique_table_name,
                               GQueue *tileids) {
  if (!hash_columns(quickhash1, db, "SELECT SlideId, Date, Creator, "
                    "Description, Keywords FROM SVSlideDataXPO "
                    "ORDER BY OID", NULL)) {
    _openslide_hash_disable(quickhash1);
    return;
  }
  if (!hash_columns(quickhash1, db, "SELECT ScanId, Date, Name, Description "
                    "FROM SVHRScanDataXPO ORDER BY OID", NULL)) {
    _openslide_hash_disable(quickhash1);
    return;
  }

  // header blob
  g_autofree char *sql =
    g_strdup_printf("SELECT data FROM %s WHERE id = 'Header' ORDER BY rowid",
                    unique_table_name);
  if (!hash_columns(quickhash1, db, sql, NULL)) {
    _openslide_hash_disable(quickhash1);
    return;
  }

  // tiles in lowest-resolution level
  if (!hash_tiles(quickhash1, db, unique_table_name, tileids, NULL)) {
    _openslide_hash_disable(quickhash1);
    return;
  }
}

static void clear_tileids(GQueue *tileids) {
  char *str;
  while ((str = g_queue_pop_head(tileids))) {
    g_free(str);
  }
}

static void free_tileid_queue(GQueue *tileids) {
  g_queue_free_full(tileids, g_free);
}

typedef GQueue tileid_queue;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(tileid_queue, free_tileid_queue)

static bool sakura_open(openslide_t *osr, const char *filename,
                        struct _openslide_tifflike *tl G_GNUC_UNUSED,
                        struct _openslide_hash *quickhash1, GError **err) {
  // open database
  g_autoptr(sqlite3) db = _openslide_sqlite_open(filename, err);
  if (!db) {
    return false;
  }

  // get unique table name
  g_autofree char *unique_table_name = get_quoted_unique_table_name(db, err);
  if (!unique_table_name) {
    return false;
  }

  // read header
  // initialize to avoid spurious warnings
  int64_t image_width = 0;
  int64_t image_height = 0;
  int32_t tile_size = 0;
  int32_t focal_planes = 0;
  if (!read_header(db, unique_table_name,
                   &image_width, &image_height,
                   &tile_size, &focal_planes, err)) {
    return false;
  }

  // select middle focal plane
  int32_t chosen_focal_plane = (focal_planes / 2) + (focal_planes % 2) - 1;
  //g_debug("Using focal plane %d", chosen_focal_plane);

  // create levels; gather tileids for top level
  g_autoptr(GHashTable) level_hash =
    g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                          (GDestroyNotify) destroy_level);
  g_autoptr(tileid_queue) quickhash_tileids = g_queue_new();
  int64_t quickhash_downsample = 0;
  g_autofree char *sql =
    g_strdup_printf("SELECT id FROM %s", unique_table_name);
  g_autoptr(sqlite3_stmt) stmt = NULL;
  PREPARE_OR_RETURN(stmt, db, sql, false);
  int ret;
  while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *tileid = (const char *) sqlite3_column_text(stmt, 0);
    int64_t downsample;
    int32_t focal_plane;
    GError *tmp_err = NULL;
    if (!parse_tileid(tileid, NULL, NULL, &downsample, NULL, &focal_plane,
                      &tmp_err)) {
      if (g_error_matches(tmp_err, OPENSLIDE_ERROR,
                          OPENSLIDE_ERROR_NO_VALUE)) {
        // not a tile
        g_clear_error(&tmp_err);
        continue;
      } else {
        g_propagate_error(err, tmp_err);
        return false;
      }
    }

    // create level if new
    struct level *l = g_hash_table_lookup(level_hash, &downsample);
    if (!l && focal_plane == 0) {
      // ensure downsample is > 0 and a power of 2
      if (downsample <= 0 || (downsample & (downsample - 1))) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Invalid downsample %"PRId64, downsample);
        return false;
      }

      l = g_slice_new0(struct level);
      l->base.downsample = downsample;
      l->base.w = image_width / downsample;
      l->base.h = image_height / downsample;
      l->base.tile_w = tile_size;
      l->base.tile_h = tile_size;
      int64_t tiles_across =
        (l->base.w / tile_size) + !!(l->base.w % tile_size);
      int64_t tiles_down =
        (l->base.h / tile_size) + !!(l->base.h % tile_size);
      l->grid = _openslide_grid_create_simple(osr,
                                              tiles_across, tiles_down,
                                              tile_size, tile_size,
                                              read_tile);
      int64_t *downsample_val = g_new(int64_t, 1);
      *downsample_val = downsample;
      g_hash_table_insert(level_hash, downsample_val, l);
    }

    // save tileid if smallest level
    if (downsample > quickhash_downsample) {
      clear_tileids(quickhash_tileids);
      quickhash_downsample = downsample;
    }
    if (downsample == quickhash_downsample) {
      g_queue_push_tail(quickhash_tileids, g_strdup(tileid));
    }
  }
  if (ret != SQLITE_DONE) {
    _openslide_sqlite_propagate_error(db, err);
    return false;
  }

  // move levels to level array
  int32_t level_count = g_hash_table_size(level_hash);
  if (level_count == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't find any tiles");
    return false;
  }
  // not autoptr; we can't fail after this
  struct level **levels = g_new(struct level *, level_count);
  GList *keys = g_hash_table_get_keys(level_hash);
  keys = g_list_sort(keys, compare_downsamples);
  int32_t i = 0;
  for (GList *cur = keys; cur; cur = cur->next) {
    levels[i] = g_hash_table_lookup(level_hash, cur->data);
    g_assert(levels[i]);
    g_hash_table_steal(level_hash, cur->data);
    i++;
  }
  g_list_free_full(keys, g_free);

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
  compute_quickhash1(quickhash1, db, unique_table_name, quickhash_tileids);

  // build ops data
  struct sakura_ops_data *data = g_slice_new0(struct sakura_ops_data);
  data->filename = g_strdup(filename);
  data->data_sql =
    g_strdup_printf("SELECT data FROM %s WHERE id=?", unique_table_name);
  data->tile_size = tile_size;
  data->focal_plane = chosen_focal_plane;

  // commit
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &sakura_ops;

  return true;
}

const struct _openslide_format _openslide_format_sakura = {
  .name = "sakura",
  .vendor = "sakura",
  .detect = sakura_detect,
  .open = sakura_open,
};
