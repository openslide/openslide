/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
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

#include <stdarg.h>
#include <math.h>
#include <glib.h>
#include <cairo.h>
#include "openslide-private.h"
#include "openslide-decode-sqlite.h"

struct region {
  double x;
  double y;
  int32_t w;
  int32_t h;

  int64_t start_tile_x;
  int64_t start_tile_y;
  int64_t end_tile_x;
  int64_t end_tile_y;

  double offset_x;
  double offset_y;
};

struct bounds {
  double x;
  double y;
  double w;
  double h;
};

struct grid_ops {
  void (*get_bounds)(struct _openslide_grid *grid,
                     struct bounds *bounds);
  bool (*paint_region)(struct _openslide_grid *grid,
                       cairo_t *cr, void *arg,
                       double x, double y,
                       struct _openslide_level *level,
                       int32_t w, int32_t h,
                       GError **err);
  void (*destroy)(struct _openslide_grid *grid);
};

typedef bool (*read_tiles_callback_fn)(struct _openslide_grid *grid,
                                       struct region *region,
                                       cairo_t *cr,
                                       struct _openslide_level *level,
                                       int64_t tile_col, int64_t tile_row,
                                       void *arg,
                                       GError **err);

struct _openslide_grid {
  openslide_t *osr;
  const struct grid_ops *ops;

  double tile_advance_x;
  double tile_advance_y;
};

struct simple_grid {
  struct _openslide_grid base;

  int64_t tiles_across;
  int64_t tiles_down;
  _openslide_grid_simple_read_fn read_tile;
};

struct tilemap_grid {
  struct _openslide_grid base;

  GHashTable *tiles;
  _openslide_grid_tilemap_read_fn read_tile;
  GDestroyNotify destroy_tile;

  // outer boundaries of grid
  double top;
  double bottom;
  double left;
  double right;

  // how much extra we might need to read to get all relevant tiles
  // computed from tile offsets
  int32_t extra_tiles_top;
  int32_t extra_tiles_bottom;
  int32_t extra_tiles_left;
  int32_t extra_tiles_right;
};

struct tilemap_tile {
  struct tilemap_grid *grid;
  void *data;

  int64_t col;
  int64_t row;

  double w;
  double h;
  // delta from the "natural" position
  double offset_x;
  double offset_y;
};

struct range_grid {
  struct _openslide_grid base;

  GPtrArray *tiles;
  sqlite3 *index;
  sqlite3_stmt *insert_stmt;

  _openslide_grid_range_read_fn read_tile;
  GDestroyNotify destroy_tile;

  // outer boundaries of grid
  double top;
  double bottom;
  double left;
  double right;
};

struct range_tile {
  void *data;

  double x;
  double y;
  double w;
  double h;
};

static void compute_region(struct _openslide_grid *grid,
                           double x, double y,
                           int32_t w, int32_t h,
                           struct region *region) {
  region->x = x;
  region->y = y;
  region->w = w;
  region->h = h;

  region->start_tile_x = x / grid->tile_advance_x;
  region->end_tile_x = ceil((x + w) / grid->tile_advance_x);
  region->start_tile_y = y / grid->tile_advance_y;
  region->end_tile_y = ceil((y + h) / grid->tile_advance_y);

  region->offset_x = x - (region->start_tile_x * grid->tile_advance_x);
  region->offset_y = y - (region->start_tile_y * grid->tile_advance_y);
}

static bool read_tiles(cairo_t *cr,
                       struct _openslide_level *level,
                       struct _openslide_grid *grid,
                       struct region *region,
                       read_tiles_callback_fn callback,
                       void *arg,
                       GError **err) {
  //g_debug("offset: %g %g, advance: %g %g", region->offset_x, region->offset_y, grid->tile_advance_x, grid->tile_advance_y);
  if (fabs(region->offset_x) >= grid->tile_advance_x) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "internal error: fabs(offset_x) >= tile_advance_x");
    return false;
  }
  if (fabs(region->offset_y) >= grid->tile_advance_y) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "internal error: fabs(offset_y) >= tile_advance_y");
    return false;
  }

  //  cairo_set_source_rgb(cr, 0, 1, 0);
  //  cairo_paint(cr);
  //g_debug("offset: %d %d", region->offset_x, region->offset_y);

  //g_debug("start: %"PRId64" %"PRId64, region->start_tile_x, region->start_tile_y);
  //g_debug("end: %"PRId64" %"PRId64, region->end_tile_x, region->end_tile_y);

  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);

  int64_t tile_y = region->end_tile_y - 1;

  while (tile_y >= region->start_tile_y) {
    double translate_y = ((tile_y - region->start_tile_y) *
                          grid->tile_advance_y) - region->offset_y;
    int64_t tile_x = region->end_tile_x - 1;

    while (tile_x >= region->start_tile_x) {
      double translate_x = ((tile_x - region->start_tile_x) *
                            grid->tile_advance_x) - region->offset_x;
      //      g_debug("read_tiles %"PRId64" %"PRId64, tile_x, tile_y);
      cairo_translate(cr, translate_x, translate_y);
      bool success = callback(grid, region, cr,
                              level, tile_x, tile_y,
                              arg, err);
      cairo_set_matrix(cr, &matrix);
      if (!success) {
        return false;
      }

      tile_x--;
    }

    tile_y--;
  }

  return true;
}

static void label_tile(cairo_t *cr,
                       double w, double h,
                       const char *coordinates) {
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  cairo_set_source_rgba(cr, 0.6, 0, 0, 0.3);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, 0.6, 0, 0, 1);
  cairo_text_extents_t extents;
  cairo_text_extents(cr, coordinates, &extents);
  cairo_move_to(cr,
                (w - extents.width) / 2,
                (h + extents.height) / 2);
  cairo_show_text(cr, coordinates);

  cairo_restore(cr);
}



static void simple_get_bounds(struct _openslide_grid *_grid,
                              struct bounds *bounds) {
  struct simple_grid *grid = (struct simple_grid *) _grid;

  bounds->w = grid->tiles_across * grid->base.tile_advance_x;
  bounds->h = grid->tiles_down * grid->base.tile_advance_y;
}

static bool simple_read_tile(struct _openslide_grid *_grid,
                             struct region *region G_GNUC_UNUSED,
                             cairo_t *cr,
                             struct _openslide_level *level,
                             int64_t tile_col, int64_t tile_row,
                             void *arg,
                             GError **err) {
  struct simple_grid *grid = (struct simple_grid *) _grid;

  if (!grid->read_tile(grid->base.osr, cr, level,
                       tile_col, tile_row, arg, err)) {
    return false;
  }
  if (_openslide_debug(OPENSLIDE_DEBUG_TILES)) {
    char *coordinates = g_strdup_printf("%"PRId64", %"PRId64,
                                        tile_col, tile_row);
    label_tile(cr,
               grid->base.tile_advance_x, grid->base.tile_advance_y,
               coordinates);
    g_free(coordinates);
  }
  return true;
}

static bool simple_paint_region(struct _openslide_grid *_grid,
                                cairo_t *cr,
                                void *arg,
                                double x, double y,
                                struct _openslide_level *level,
                                int32_t w, int32_t h,
                                GError **err) {
  struct simple_grid *grid = (struct simple_grid *) _grid;
  struct region region;

  compute_region(_grid, x, y, w, h, &region);

  // check if completely outside grid
  if (region.end_tile_x <= 0 ||
      region.end_tile_y <= 0 ||
      region.start_tile_x > grid->tiles_across - 1 ||
      region.start_tile_y > grid->tiles_down - 1) {
    return true;
  }

  // save
  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);

  // bound on left/top
  int64_t skipped_tiles_x = -MIN(region.start_tile_x, 0);
  int64_t skipped_tiles_y = -MIN(region.start_tile_y, 0);
  cairo_translate(cr,
                  skipped_tiles_x * grid->base.tile_advance_x,
                  skipped_tiles_y * grid->base.tile_advance_y);
  region.start_tile_x += skipped_tiles_x;
  region.start_tile_y += skipped_tiles_y;

  // bound on right/bottom
  region.end_tile_x = MIN(region.end_tile_x, grid->tiles_across);
  region.end_tile_y = MIN(region.end_tile_y, grid->tiles_down);

  // read
  bool result = read_tiles(cr, level, _grid, &region,
                           simple_read_tile, arg, err);

  // restore
  cairo_set_matrix(cr, &matrix);

  return result;
}

static void simple_destroy(struct _openslide_grid *_grid) {
  struct simple_grid *grid = (struct simple_grid *) _grid;

  g_slice_free(struct simple_grid, grid);
}

const struct grid_ops simple_grid_ops = {
  .get_bounds = simple_get_bounds,
  .paint_region = simple_paint_region,
  .destroy = simple_destroy,
};

struct _openslide_grid *_openslide_grid_create_simple(openslide_t *osr,
                                                      int64_t tiles_across,
                                                      int64_t tiles_down,
                                                      int32_t tile_w,
                                                      int32_t tile_h,
                                                      _openslide_grid_simple_read_fn read_tile) {
  struct simple_grid *grid = g_slice_new0(struct simple_grid);
  grid->base.osr = osr;
  grid->base.ops = &simple_grid_ops;
  grid->base.tile_advance_x = tile_w;
  grid->base.tile_advance_y = tile_h;
  grid->tiles_across = tiles_across;
  grid->tiles_down = tiles_down;
  grid->read_tile = read_tile;
  return (struct _openslide_grid *) grid;
}



static guint tilemap_tile_hash_func(gconstpointer key) {
  const struct tilemap_tile *tile = key;

  // assume 32-bit hash
  return (guint) ((34369 * (uint64_t) tile->row) + ((uint64_t) tile->col));
}

static gboolean tilemap_tile_hash_key_equal(gconstpointer a, gconstpointer b) {
  const struct tilemap_tile *c_a = a;
  const struct tilemap_tile *c_b = b;

  return (c_a->col == c_b->col) && (c_a->row == c_b->row);
}

static void tilemap_tile_hash_destroy_value(gpointer data) {
  struct tilemap_tile *tile = data;
  if (tile->grid->destroy_tile && tile->data) {
    tile->grid->destroy_tile(tile->data);
  }
  g_slice_free(struct tilemap_tile, tile);
}

static void tilemap_get_bounds(struct _openslide_grid *_grid,
                               struct bounds *bounds) {
  struct tilemap_grid *grid = (struct tilemap_grid *) _grid;

  if (!isinf(grid->left)) {
    bounds->x = grid->left;
    bounds->y = grid->top;
    bounds->w = grid->right - grid->left;
    bounds->h = grid->bottom - grid->top;
  }
}

static bool tilemap_read_tile(struct _openslide_grid *_grid,
                              struct region *region,
                              cairo_t *cr,
                              struct _openslide_level *level,
                              int64_t tile_col, int64_t tile_row,
                              void *arg,
                              GError **err) {
  struct tilemap_grid *grid = (struct tilemap_grid *) _grid;

  struct tilemap_tile coords = {
    .col = tile_col,
    .row = tile_row,
  };
  struct tilemap_tile *tile = g_hash_table_lookup(grid->tiles, &coords);
  if (tile == NULL) {
    //g_debug("no tile at %"PRId64", %"PRId64, tile_col, tile_row);
    return true;
  }

  double x = tile_col * grid->base.tile_advance_x + tile->offset_x;
  double y = tile_row * grid->base.tile_advance_y + tile->offset_y;

  // skip the tile if it's outside the requested region
  // (i.e., extra_tiles_* gave us an irrelevant tile)
  if (x + tile->w <= region->x ||
      y + tile->h <= region->y ||
      x >= region->x + region->w ||
      y >= region->y + region->h) {
    //g_debug("skip x %g w %g y %g h %g, region x %g w %d y %g h %d", x, tile->w, y, tile->h, region->x, region->w, region->y, region->h);
    return true;
  }

  //g_debug("tilemap read_tile: %"PRId64" %"PRId64", offset: %g %g, dim: %g %g", tile_col, tile_row, tile->offset_x, tile->offset_y, tile->w, tile->h);

  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);
  cairo_translate(cr, tile->offset_x, tile->offset_y);
  bool success = grid->read_tile(grid->base.osr, cr, level,
                                 tile->col, tile->row, tile->data,
                                 arg, err);
  if (success && _openslide_debug(OPENSLIDE_DEBUG_TILES)) {
    char *coordinates = g_strdup_printf("%"PRId64", %"PRId64,
                                        tile_col, tile_row);
    label_tile(cr, tile->w, tile->h, coordinates);
    g_free(coordinates);
  }
  cairo_set_matrix(cr, &matrix);
  return success;
}

static bool tilemap_paint_region(struct _openslide_grid *_grid,
                                 cairo_t *cr,
                                 void *arg,
                                 double x, double y,
                                 struct _openslide_level *level,
                                 int32_t w, int32_t h,
                                 GError **err) {
  struct tilemap_grid *grid = (struct tilemap_grid *) _grid;
  struct region region;

  compute_region(_grid, x, y, w, h, &region);

  //g_debug("coords: %g %g", x, y);
  //g_debug("advances: %g %g", grid->base.tile_advance_x, grid->base.tile_advance_y);
  //g_debug("start tile: %"PRId64" %"PRId64", end tile: %"PRId64" %"PRId64, start_tile_x, start_tile_y, end_tile_x, end_tile_y);

  // save
  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);

  // accommodate extra tiles being drawn
  region.start_tile_x -= grid->extra_tiles_left;
  region.start_tile_y -= grid->extra_tiles_top;
  region.end_tile_x += grid->extra_tiles_right;
  region.end_tile_y += grid->extra_tiles_bottom;
  cairo_translate(cr,
                  -grid->extra_tiles_left * grid->base.tile_advance_x,
                  -grid->extra_tiles_top * grid->base.tile_advance_y);

  // read
  bool result = read_tiles(cr, level, _grid, &region,
                           tilemap_read_tile, arg, err);

  // restore
  cairo_set_matrix(cr, &matrix);

  return result;
}

static void tilemap_destroy(struct _openslide_grid *_grid) {
  struct tilemap_grid *grid = (struct tilemap_grid *) _grid;

  g_hash_table_destroy(grid->tiles);
  g_slice_free(struct tilemap_grid, grid);
}

const struct grid_ops tilemap_grid_ops = {
  .get_bounds = tilemap_get_bounds,
  .paint_region = tilemap_paint_region,
  .destroy = tilemap_destroy,
};

void _openslide_grid_tilemap_add_tile(struct _openslide_grid *_grid,
                                      int64_t col, int64_t row,
                                      double offset_x, double offset_y,
                                      double w, double h,
                                      void *data) {
  struct tilemap_grid *grid = (struct tilemap_grid *) _grid;
  g_assert(grid->base.ops == &tilemap_grid_ops);

  struct tilemap_tile *tile = g_slice_new0(struct tilemap_tile);
  tile->grid = grid;
  tile->col = col;
  tile->row = row;
  tile->offset_x = offset_x;
  tile->offset_y = offset_y;
  tile->w = w;
  tile->h = h;
  tile->data = data;

  g_hash_table_replace(grid->tiles, tile, tile);

  grid->left = MIN(col * grid->base.tile_advance_x + offset_x,
                   grid->left);
  grid->top = MIN(row * grid->base.tile_advance_y + offset_y,
                  grid->top);
  grid->right = MAX(col * grid->base.tile_advance_x + offset_x + w,
                    grid->right);
  grid->bottom = MAX(row * grid->base.tile_advance_y + offset_y + h,
                     grid->bottom);

  if (offset_x < 0) {
    // extra on right
    int32_t extra_right = ceil(-offset_x / grid->base.tile_advance_x);
    grid->extra_tiles_right = MAX(grid->extra_tiles_right, extra_right);
  }
  double offset_xr = offset_x + (tile->w - grid->base.tile_advance_x);
  if (offset_xr > 0) {
    // extra on left
    int32_t extra_left = ceil(offset_xr / grid->base.tile_advance_x);
    grid->extra_tiles_left = MAX(grid->extra_tiles_left, extra_left);
  }

  if (offset_y < 0) {
    // extra on bottom
    int32_t extra_bottom = ceil(-offset_y / grid->base.tile_advance_y);
    grid->extra_tiles_bottom = MAX(grid->extra_tiles_bottom, extra_bottom);
  }
  double offset_yr = offset_y + (tile->h - grid->base.tile_advance_y);
  if (offset_yr > 0) {
    // extra on top
    int32_t extra_top = ceil(offset_yr / grid->base.tile_advance_y);
    grid->extra_tiles_top = MAX(grid->extra_tiles_top, extra_top);
  }
  //g_debug("%p: extra_left: %d, extra_right: %d, extra_top: %d, extra_bottom: %d", (void *) grid, grid->extra_tiles_left, grid->extra_tiles_right, grid->extra_tiles_top, grid->extra_tiles_bottom);
}

struct _openslide_grid *_openslide_grid_create_tilemap(openslide_t *osr,
                                                       double tile_advance_x,
                                                       double tile_advance_y,
                                                       _openslide_grid_tilemap_read_fn read_tile,
                                                       GDestroyNotify destroy_tile) {
  struct tilemap_grid *grid = g_slice_new0(struct tilemap_grid);
  grid->base.osr = osr;
  grid->base.ops = &tilemap_grid_ops;
  grid->base.tile_advance_x = tile_advance_x;
  grid->base.tile_advance_y = tile_advance_y;
  grid->read_tile = read_tile;
  grid->destroy_tile = destroy_tile;

  grid->top = INFINITY;
  grid->bottom = -INFINITY;
  grid->left = INFINITY;
  grid->right = -INFINITY;

  grid->tiles = g_hash_table_new_full(tilemap_tile_hash_func,
                                      tilemap_tile_hash_key_equal,
                                      NULL,
                                      tilemap_tile_hash_destroy_value);

  return (struct _openslide_grid *) grid;
}



static void range_get_bounds(struct _openslide_grid *_grid,
                             struct bounds *bounds) {
  struct range_grid *grid = (struct range_grid *) _grid;

  if (!isinf(grid->left)) {
    bounds->x = grid->left;
    bounds->y = grid->top;
    bounds->w = grid->right - grid->left;
    bounds->h = grid->bottom - grid->top;
  }
}

static bool range_paint_region(struct _openslide_grid *_grid,
                               cairo_t *cr,
                               void *arg,
                               double x, double y,
                               struct _openslide_level *level,
                               int32_t w, int32_t h,
                               GError **err) {
  struct range_grid *grid = (struct range_grid *) _grid;
  bool result = false;

  // ensure _openslide_grid_range_finish_adding_tiles() was called
  g_assert(!grid->insert_stmt);

  // save
  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);

  // prepare query
  sqlite3_stmt *stmt =
    _openslide_sqlite_prepare(grid->index, "SELECT id FROM tiles WHERE "
                              "xmax >= ? AND xmin <= ? AND "
                              "ymax >= ? AND ymin <= ? "
                              "ORDER BY ymin DESC, xmin DESC;", err);
  if (!stmt) {
    return false;
  }
  if (sqlite3_bind_double(stmt, 1, x) ||
      sqlite3_bind_double(stmt, 2, x + w) ||
      sqlite3_bind_double(stmt, 3, y) ||
      sqlite3_bind_double(stmt, 4, y + h)) {
    _openslide_sqlite_propagate_error(grid->index, err);
    goto DONE;
  }

  // walk tiles
  int ret;
  while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
    // look up tile struct
    int64_t id = sqlite3_column_int64(stmt, 0);
    g_assert(id >= 0 && id < grid->tiles->len);
    struct range_tile *tile = grid->tiles->pdata[id];

    // skip tile if it's outside the requested region
    // (R*Tree gave us an irrelevant tile due to round-off error)
    if (tile->x + tile->w <= x ||
        tile->y + tile->h <= y ||
        tile->x >= x + w ||
        tile->y >= y + h) {
      //g_debug("skip x %g w %g y %g h %g, region x %g w %d y %g h %d", tile->x, tile->w, tile->y, tile->h, x, w, y, h);
      continue;
    }

    // draw
    //g_debug("tile x %g y %g", tile->x, tile->y);
    cairo_translate(cr, tile->x - x, tile->y - y);
    bool success = grid->read_tile(grid->base.osr, cr, level,
                                   id, tile->data,
                                   arg, err);
    if (success && _openslide_debug(OPENSLIDE_DEBUG_TILES)) {
      char *coordinates = g_strdup_printf("%"PRId64, id);
      label_tile(cr, tile->w, tile->h, coordinates);
      g_free(coordinates);
    }
    cairo_set_matrix(cr, &matrix);
    if (!success) {
      goto DONE;
    }
  }
  if (ret != SQLITE_DONE) {
    _openslide_sqlite_propagate_error(grid->index, err);
    goto DONE;
  }

  // success
  result = true;

DONE:
  sqlite3_finalize(stmt);
  return result;
}

static void range_destroy(struct _openslide_grid *_grid) {
  struct range_grid *grid = (struct range_grid *) _grid;

  _openslide_grid_range_finish_adding_tiles(_grid, NULL);
  _openslide_sqlite_close(grid->index);
  for (uint64_t cur = 0; cur < grid->tiles->len; cur++) {
    struct range_tile *tile = grid->tiles->pdata[cur];
    if (grid->destroy_tile && tile->data) {
      grid->destroy_tile(tile->data);
    }
    g_slice_free(struct range_tile, tile);
  }
  g_ptr_array_free(grid->tiles, true);
  g_slice_free(struct range_grid, grid);
}

const struct grid_ops range_grid_ops = {
  .get_bounds = range_get_bounds,
  .paint_region = range_paint_region,
  .destroy = range_destroy,
};

bool _openslide_grid_range_add_tile(struct _openslide_grid *_grid,
                                    double x, double y,
                                    double w, double h,
                                    void *data,
                                    GError **err) {
  struct range_grid *grid = (struct range_grid *) _grid;
  g_assert(grid->base.ops == &range_grid_ops);
  g_assert(grid->insert_stmt);

  int64_t id = grid->tiles->len;
  sqlite3_reset(grid->insert_stmt);
  if (sqlite3_bind_int64(grid->insert_stmt, 1, id) ||
      sqlite3_bind_double(grid->insert_stmt, 2, x) ||
      sqlite3_bind_double(grid->insert_stmt, 3, x + w) ||
      sqlite3_bind_double(grid->insert_stmt, 4, y) ||
      sqlite3_bind_double(grid->insert_stmt, 5, y + h)) {
    _openslide_sqlite_propagate_error(grid->index, err);
    return false;
  }
  if (sqlite3_step(grid->insert_stmt) != SQLITE_DONE) {
    _openslide_sqlite_propagate_error(grid->index, err);
    return false;
  }

  struct range_tile *tile = g_slice_new0(struct range_tile);
  tile->data = data;
  tile->x = x;
  tile->y = y;
  tile->w = w;
  tile->h = h;
  g_ptr_array_add(grid->tiles, tile);

  grid->left = MIN(x, grid->left);
  grid->top = MIN(y, grid->top);
  grid->right = MAX(x + w, grid->right);
  grid->bottom = MAX(y + h, grid->bottom);

  return true;
}

bool _openslide_grid_range_finish_adding_tiles(struct _openslide_grid *_grid,
                                               GError **err) {
  struct range_grid *grid = (struct range_grid *) _grid;
  g_assert(grid->base.ops == &range_grid_ops);

  if (grid->insert_stmt) {
    sqlite3_finalize(grid->insert_stmt);
    grid->insert_stmt = NULL;

    if (!_openslide_sqlite_exec(grid->index, "COMMIT", err)) {
      return false;
    }
  }
  return true;
}

struct _openslide_grid *_openslide_grid_create_range(openslide_t *osr,
                                                     _openslide_grid_range_read_fn read_tile,
                                                     GDestroyNotify destroy_tile,
                                                     GError **err) {
  sqlite3 *index = _openslide_sqlite_open_memory(err);
  if (!index) {
    g_prefix_error(err, "Creating R*Tree database: ");
    return NULL;
  }

  if (!_openslide_sqlite_exec(index, "BEGIN", err)) {
    goto FAIL;
  }

  if (!_openslide_sqlite_exec(index, "CREATE VIRTUAL TABLE tiles USING "
                              "rtree(id, xmin, xmax, ymin, ymax);", err)) {
    g_prefix_error(err, "Creating R*Tree table: ");
    goto FAIL;
  }

  sqlite3_stmt *insert_stmt =
    _openslide_sqlite_prepare(index, "INSERT INTO tiles "
                              "VALUES(?, ?, ?, ?, ?);", err);
  if (!insert_stmt) {
    goto FAIL;
  }

  struct range_grid *grid = g_slice_new0(struct range_grid);
  grid->base.osr = osr;
  grid->base.ops = &range_grid_ops;
  grid->base.tile_advance_x = NAN;  // unused
  grid->base.tile_advance_y = NAN;  // unused
  grid->tiles = g_ptr_array_new();
  grid->index = index;
  grid->insert_stmt = insert_stmt;
  grid->read_tile = read_tile;
  grid->destroy_tile = destroy_tile;

  grid->top = INFINITY;
  grid->bottom = -INFINITY;
  grid->left = INFINITY;
  grid->right = -INFINITY;

  return (struct _openslide_grid *) grid;

FAIL:
  _openslide_sqlite_close(index);
  return NULL;
}



void _openslide_grid_get_bounds(struct _openslide_grid *grid,
                                double *x, double *y,
                                double *w, double *h) {
  struct bounds bounds = {0, 0, 0, 0};
  grid->ops->get_bounds(grid, &bounds);
  //g_debug("%p bounds: x %g y %g w %g h %g", (void *) grid, bounds.x, bounds.y, bounds.w, bounds.h);
  if (x) {
    *x = bounds.x;
  }
  if (y) {
    *y = bounds.y;
  }
  if (w) {
    *w = bounds.w;
  }
  if (h) {
    *h = bounds.h;
  }
}

bool _openslide_grid_paint_region(struct _openslide_grid *grid,
                                  cairo_t *cr,
                                  void *arg,
                                  double x, double y,
                                  struct _openslide_level *level,
                                  int32_t w, int32_t h,
                                  GError **err) {
  return grid->ops->paint_region(grid, cr, arg, x, y, level, w, h, err);
}

void _openslide_grid_destroy(struct _openslide_grid *grid) {
  if (grid == NULL) {
    return;
  }
  grid->ops->destroy(grid);
}

void _openslide_grid_draw_tile_info(cairo_t *cr, const char *fmt, ...) {
  if (!_openslide_debug(OPENSLIDE_DEBUG_TILES)) {
    return;
  }

  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba(cr, 0.6, 0, 0, 1);

  va_list ap;
  va_start(ap, fmt);
  char *str = g_strdup_vprintf(fmt, ap);
  char **lines = g_strsplit(str, "\n", 0);
  int count = g_strv_length(lines);
  g_free(str);
  va_end(ap);

  cairo_font_extents_t extents;
  cairo_font_extents(cr, &extents);
  for (int i = 0; i < count; i++) {
    cairo_move_to(cr, 5, i * extents.height + extents.ascent + 5);
    cairo_show_text(cr, lines[i]);
  }

  g_strfreev(lines);
  cairo_restore(cr);
}
