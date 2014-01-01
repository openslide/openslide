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
 * Sakura (svslide) support
 *
 * quickhash comes from XXX
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
#include <stdlib.h> // for mkstemp
#include <unistd.h> // for close, unlink

static const char MAGIC_BYTES[] = "SVGigaPixelImage";

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
  sqlite3_close(db);
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

static uint32_t *read_channel(const char *tileid,
                              int32_t tile_size,
                              sqlite3_stmt *stmt,
                              GError **err) {
  uint32_t *channeldata = NULL;

  // retrieve compressed tile
  sqlite3_reset(stmt);
  BIND_TEXT_OR_FAIL(stmt, 1, tileid);
  STEP_OR_FAIL(stmt);
  const void *buf = sqlite3_column_blob(stmt, 0);
  if (!buf) {
    // don't pass NULL pointer to JPEG decoder
    buf = "";
  }
  int buflen = sqlite3_column_bytes(stmt, 0);

  // decompress
  // via tempfile, pending decode-jpeg changes
  char tempfile[] = "/tmp/sakura.XXXXXX";
  int fd = mkstemp(tempfile);
  if (fd == -1) {
    _openslide_io_error(err, "Couldn't create temporary file");
    goto FAIL;
  }
  close(fd);
  if (!g_file_set_contents(tempfile, buf, buflen, err)) {
    unlink(tempfile);
    goto FAIL;
  }
  channeldata = g_slice_alloc(tile_size * tile_size * 4);
  if (!_openslide_jpeg_read(tempfile, 0, channeldata,
                            tile_size, tile_size, err)) {
    g_slice_free1(tile_size * tile_size * 4, channeldata);
    channeldata = NULL;
  }
  unlink(tempfile);

FAIL:
  return channeldata;
}

static uint32_t *read_image(const struct tile *tile,
                            int32_t tile_size,
                            sqlite3_stmt *stmt,
                            GError **err) {
  uint32_t *tiledata = NULL;
  uint32_t *red_channel = NULL;
  uint32_t *green_channel = NULL;
  uint32_t *blue_channel = NULL;

  red_channel = read_channel(tile->id_red, tile_size, stmt, err);
  if (!red_channel) {
    goto OUT;
  }
  green_channel = read_channel(tile->id_green, tile_size, stmt, err);
  if (!green_channel) {
    goto OUT;
  }
  blue_channel = read_channel(tile->id_blue, tile_size, stmt, err);
  if (!blue_channel) {
    goto OUT;
  }

  tiledata = g_slice_alloc(tile_size * tile_size * 4);
  for (int32_t i = 0; i < tile_size * tile_size; i++) {
    tiledata[i] = 0xff000000 |
                  (red_channel[i] & 0x00ff0000) |
                  (green_channel[i] & 0x0000ff00) |
                  (blue_channel[i] & 0x000000ff);
  }

OUT:
  g_slice_free1(tile_size * tile_size * 4, red_channel);
  g_slice_free1(tile_size * tile_size * 4, green_channel);
  g_slice_free1(tile_size * tile_size * 4, blue_channel);
  return tiledata;
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level G_GNUC_UNUSED,
                      int64_t tile_col, int64_t tile_row,
                      void *_tile, void *arg,
                      GError **err) {
  struct sakura_ops_data *data = osr->data;
  struct tile *tile = _tile;
  sqlite3_stmt *stmt = arg;
  int32_t tile_size = data->tile_size;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    // read tile
    tiledata = read_image(tile, tile_size, stmt, err);
    if (!tiledata) {
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
  sqlite3_close(db);
  return success;
}

static const struct _openslide_ops sakura_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

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
    struct tile *tile = _openslide_grid_tilemap_get_tile(l->grid, col, row);
    if (!tile) {
      tile = g_slice_new0(struct tile);
      _openslide_grid_tilemap_add_tile(l->grid, col, row, 0, 0,
                                       tile_size, tile_size, tile);
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

  // levels are complete and sorted; walk them
  for (i = 0; i < level_count; i++) {
    struct level *l = levels[i];

    // ensure all tiles have all components
    struct ensure_components_args args = {
      .downsample = l->base.downsample,
    };
    _openslide_grid_tilemap_foreach(l->grid, ensure_components, &args);
    if (args.err) {
      g_propagate_error(err, args.err);
      goto FAIL;
    }

    // set level sizes and tile size hints
    l->base.w = image_width / l->base.downsample;
    l->base.h = image_height / l->base.downsample;
    l->base.tile_w = tile_size;
    l->base.tile_h = tile_size;
  }

  // no quickhash for now
  _openslide_hash_disable(quickhash1);

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
  sqlite3_close(db);
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
