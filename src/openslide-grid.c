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

#include <math.h>
#include <glib.h>
#include "openslide-private.h"
#include "openslide-tilehelper.h"

struct _openslide_grid_simple {
  openslide_t *osr;
  _openslide_tileread_fn read_tile;

  int64_t tiles_across;
  int64_t tiles_down;
  int32_t tile_w;
  int32_t tile_h;
};

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
                                         int64_t x, int64_t y,
                                         struct _openslide_level *level,
                                         int32_t w, int32_t h) {
  double ds_x = x / level->downsample;
  double ds_y = y / level->downsample;
  int64_t start_tile_x = ds_x / grid->tile_w;
  int64_t end_tile_x = ceil((ds_x + w) / grid->tile_w);
  int64_t start_tile_y = ds_y / grid->tile_h;
  int64_t end_tile_y = ceil((ds_y + h) / grid->tile_h);

  double offset_x = ds_x - (start_tile_x * grid->tile_w);
  double offset_y = ds_y - (start_tile_y * grid->tile_h);

  // openslide.c:read_region() ensures x/y are nonnegative
  if (start_tile_x > grid->tiles_across - 1 ||
      start_tile_y > grid->tiles_down - 1) {
    return;
  }
  end_tile_x = MIN(end_tile_x, grid->tiles_across);
  end_tile_y = MIN(end_tile_y, grid->tiles_down);

  _openslide_read_tiles(cr, level,
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
