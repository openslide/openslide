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

#include <stdint.h>
#include <math.h>
#include <glib.h>
#include <cairo.h>
#include "openslide-private.h"

struct _openslide_grid_simple {
  openslide_t *osr;
  _openslide_tileread_fn read_tile;

  int64_t tiles_across;
  int64_t tiles_down;
  int32_t tile_w;
  int32_t tile_h;
};

struct _openslide_grid_tilemap {
  openslide_t *osr;
  GHashTable *tiles;
  _openslide_tilemap_fn read_tile;
  GDestroyNotify destroy_tile;

  double tile_advance_x;
  double tile_advance_y;

  // how much extra we might need to read to get all relevant tiles
  // computed from tile offsets
  int32_t extra_tiles_top;
  int32_t extra_tiles_bottom;
  int32_t extra_tiles_left;
  int32_t extra_tiles_right;
};

struct grid_tile {
  struct _openslide_grid_tilemap *grid;
  void *data;

  int64_t col;
  int64_t row;

  double w;
  double h;
  // delta from the "natural" position
  double offset_x;
  double offset_y;
};

struct tilemap_read_tile_args {
  struct _openslide_grid_tilemap *grid;
  void *arg;
  double region_x;
  double region_y;
  int32_t region_w;
  int32_t region_h;
};

static void read_tiles(cairo_t *cr,
                       struct _openslide_level *level,
                       int64_t start_tile_x, int64_t start_tile_y,
                       int64_t end_tile_x, int64_t end_tile_y,
                       double offset_x, double offset_y,
                       double advance_x, double advance_y,
                       openslide_t *osr,
                       void *arg,
                       _openslide_tileread_fn read_tile) {
  //g_debug("offset: %g %g, advance: %g %g", offset_x, offset_y, advance_x, advance_y);
  if (fabs(offset_x) >= advance_x) {
    _openslide_set_error(osr, "internal error: fabs(offset_x) >= advance_x");
    return;
  }
  if (fabs(offset_y) >= advance_y) {
    _openslide_set_error(osr, "internal error: fabs(offset_y) >= advance_y");
    return;
  }

  //  cairo_set_source_rgb(cr, 0, 1, 0);
  //  cairo_paint(cr);
  //g_debug("offset: %d %d", offset_x, offset_y);

  //g_debug("start: %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, start_tile_x, start_tile_y);
  //g_debug("end: %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, end_tile_x, end_tile_y);

  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);

  int64_t tile_y = end_tile_y - 1;

  while (tile_y >= start_tile_y) {
    double translate_y = ((tile_y - start_tile_y) * advance_y) - offset_y;
    int64_t tile_x = end_tile_x - 1;

    while (tile_x >= start_tile_x) {
      double translate_x = ((tile_x - start_tile_x) * advance_x) - offset_x;
      //      g_debug("read_tiles %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, tile_x, tile_y);
      cairo_translate(cr, translate_x, translate_y);
      read_tile(osr, cr, level, tile_x, tile_y, arg);
      cairo_set_matrix(cr, &matrix);
      tile_x--;
    }

    tile_y--;
  }
}

struct _openslide_grid_simple *_openslide_grid_simple_create(openslide_t *osr,
                                                             int64_t tiles_across,
                                                             int64_t tiles_down,
                                                             int32_t tile_w,
                                                             int32_t tile_h,
                                                             _openslide_tileread_fn read_tile) {
  struct _openslide_grid_simple *grid =
    g_slice_new0(struct _openslide_grid_simple);
  grid->osr = osr;
  grid->tiles_across = tiles_across;
  grid->tiles_down = tiles_down;
  grid->tile_w = tile_w;
  grid->tile_h = tile_h;
  grid->read_tile = read_tile;
  return grid;
}

void _openslide_grid_simple_paint_region(struct _openslide_grid_simple *grid,
                                         cairo_t *cr,
                                         void *arg,
                                         double x, double y,
                                         struct _openslide_level *level,
                                         int32_t w, int32_t h) {
  int64_t start_tile_x = x / grid->tile_w;
  int64_t end_tile_x = ceil((x + w) / grid->tile_w);
  int64_t start_tile_y = y / grid->tile_h;
  int64_t end_tile_y = ceil((y + h) / grid->tile_h);

  double offset_x = x - (start_tile_x * grid->tile_w);
  double offset_y = y - (start_tile_y * grid->tile_h);

  // openslide.c:read_region() ensures x/y are nonnegative
  if (start_tile_x > grid->tiles_across - 1 ||
      start_tile_y > grid->tiles_down - 1) {
    return;
  }
  end_tile_x = MIN(end_tile_x, grid->tiles_across);
  end_tile_y = MIN(end_tile_y, grid->tiles_down);

  read_tiles(cr, level,
             start_tile_x, start_tile_y,
             end_tile_x, end_tile_y,
             offset_x, offset_y,
             grid->tile_w, grid->tile_h,
             grid->osr, arg, grid->read_tile);
}

void _openslide_grid_simple_destroy(struct _openslide_grid_simple *grid) {
  if (grid == NULL) {
    return;
  }
  g_slice_free(struct _openslide_grid_simple, grid);
}



static guint grid_tile_hash_func(gconstpointer key) {
  const struct grid_tile *tile = key;

  // assume 32-bit hash
  return (guint) ((tile->row << 16) ^ (tile->col));
}

static gboolean grid_tile_hash_key_equal(gconstpointer a, gconstpointer b) {
  const struct grid_tile *c_a = a;
  const struct grid_tile *c_b = b;

  return (c_a->col == c_b->col) && (c_a->row == c_b->row);
}

static void grid_tile_hash_destroy_value(gpointer data) {
  struct grid_tile *tile = data;
  if (tile->grid->destroy_tile && tile->data) {
    tile->grid->destroy_tile(tile->data);
  }
  g_slice_free(struct grid_tile, tile);
}

struct _openslide_grid_tilemap *_openslide_grid_tilemap_create(openslide_t *osr,
                                                               double tile_advance_x,
                                                               double tile_advance_y,
                                                               _openslide_tilemap_fn read_tile,
                                                               GDestroyNotify destroy_tile) {
  struct _openslide_grid_tilemap *grid =
    g_slice_new0(struct _openslide_grid_tilemap);
  grid->osr = osr;
  grid->tile_advance_x = tile_advance_x;
  grid->tile_advance_y = tile_advance_y;
  grid->read_tile = read_tile;
  grid->destroy_tile = destroy_tile;

  grid->tiles = g_hash_table_new_full(grid_tile_hash_func,
                                      grid_tile_hash_key_equal,
                                      NULL,
                                      grid_tile_hash_destroy_value);

  return grid;
}

void _openslide_grid_tilemap_add_tile(struct _openslide_grid_tilemap *grid,
                                      int64_t col, int64_t row,
                                      double offset_x, double offset_y,
                                      double w, double h,
                                      void *data) {
  struct grid_tile *tile = g_slice_new0(struct grid_tile);
  tile->grid = grid;
  tile->col = col;
  tile->row = row;
  tile->offset_x = offset_x;
  tile->offset_y = offset_y;
  tile->w = w;
  tile->h = h;
  tile->data = data;

  g_hash_table_replace(grid->tiles, tile, tile);

  if (offset_x > 0) {
    // extra on left
    int32_t extra_left = ceil(offset_x / grid->tile_advance_x);
    grid->extra_tiles_left = MAX(grid->extra_tiles_left, extra_left);
  } else {
    // extra on right
    int32_t extra_right = ceil(-offset_x / grid->tile_advance_x);
    grid->extra_tiles_right = MAX(grid->extra_tiles_right, extra_right);
  }

  if (offset_y > 0) {
    // extra on top
    int32_t extra_top = ceil(offset_y / grid->tile_advance_y);
    grid->extra_tiles_top = MAX(grid->extra_tiles_top, extra_top);
  } else {
    // extra on bottom
    int32_t extra_bottom = ceil(-offset_y / grid->tile_advance_y);
    grid->extra_tiles_bottom = MAX(grid->extra_tiles_bottom, extra_bottom);
  }
  //g_debug("%p: extra_left: %d, extra_right: %d, extra_top: %d, extra_bottom: %d", (void *) grid, grid->extra_tiles_left, grid->extra_tiles_right, grid->extra_tiles_top, grid->extra_tiles_bottom);
}

static void grid_tilemap_read_tile(openslide_t *osr,
                                   cairo_t *cr,
                                   struct _openslide_level *level,
                                   int64_t tile_col, int64_t tile_row,
                                   void *arg) {
  struct tilemap_read_tile_args *args = arg;

  struct grid_tile coords = {
    .col = tile_col,
    .row = tile_row,
  };
  struct grid_tile *tile = g_hash_table_lookup(args->grid->tiles, &coords);
  if (tile == NULL) {
    //g_debug("no tile at %"G_GINT64_FORMAT", %"G_GINT64_FORMAT, tile_col, tile_row);
    return;
  }

  double x = tile_col * args->grid->tile_advance_x + tile->offset_x;
  double y = tile_row * args->grid->tile_advance_y + tile->offset_y;

  // skip the tile if it's outside the requested region
  // (i.e., extra_tiles_* gave us an irrelevant tile)
  if (x + tile->w <= args->region_x ||
      y + tile->h <= args->region_y ||
      x >= args->region_x + args->region_w ||
      y >= args->region_y + args->region_h) {
    //g_debug("skip x %g w %g y %g h %g, region x %g w %"G_GINT32_FORMAT" y %g h %"G_GINT32_FORMAT, x, tile->w, y, tile->h, args->region_x, args->region_w, args->region_y, args->region_h);
    return;
  }

  //g_debug("tilemap read_tile: %" G_GINT64_FORMAT " %" G_GINT64_FORMAT ", offset: %g %g, dim: %g %g", tile_col, tile_row, tile->offset_x, tile->offset_y, tile->w, tile->h);

  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);
  cairo_translate(cr, tile->offset_x, tile->offset_y);
  args->grid->read_tile(osr, cr, level, tile->data, args->arg);
  cairo_set_matrix(cr, &matrix);
}

void _openslide_grid_tilemap_paint_region(struct _openslide_grid_tilemap *grid,
                                          cairo_t *cr,
                                          void *arg,
                                          double x, double y,
                                          struct _openslide_level *level,
                                          int32_t w, int32_t h) {
  int64_t start_tile_x = x / grid->tile_advance_x;
  int64_t end_tile_x = ceil((x + w) / grid->tile_advance_x);
  int64_t start_tile_y = y / grid->tile_advance_y;
  int64_t end_tile_y = ceil((y + h) / grid->tile_advance_y);

  double offset_x = x - (start_tile_x * grid->tile_advance_x);
  double offset_y = y - (start_tile_y * grid->tile_advance_y);

  struct tilemap_read_tile_args args = {
    .grid = grid,
    .arg = arg,
    .region_x = x,
    .region_y = y,
    .region_w = w,
    .region_h = h,
  };

  //g_debug("coords: %g %g", x, y);
  //g_debug("advances: %g %g", grid->tile_advance_x, grid->tile_advance_y);
  //g_debug("start tile: %" G_GINT64_FORMAT " %" G_GINT64_FORMAT ", end tile: %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, start_tile_x, start_tile_y, end_tile_x, end_tile_y);

  // accommodate extra tiles being drawn
  cairo_translate(cr,
                  -grid->extra_tiles_left * grid->tile_advance_x,
                  -grid->extra_tiles_top * grid->tile_advance_y);

  read_tiles(cr, level,
             start_tile_x - grid->extra_tiles_left,
             start_tile_y - grid->extra_tiles_top,
             end_tile_x + grid->extra_tiles_right,
             end_tile_y + grid->extra_tiles_bottom,
             offset_x, offset_y,
             grid->tile_advance_x, grid->tile_advance_y,
             grid->osr, &args, grid_tilemap_read_tile);
}

void _openslide_grid_tilemap_destroy(struct _openslide_grid_tilemap *grid) {
  if (grid == NULL) {
    return;
  }
  g_hash_table_destroy(grid->tiles);
  g_slice_free(struct _openslide_grid_tilemap, grid);
}
