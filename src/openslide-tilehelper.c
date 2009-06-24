/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#include "openslide-tilehelper.h"

#include <glib.h>
#include <string.h>
#include <inttypes.h>
#include <cairo.h>

void _openslide_read_tiles(cairo_t *cr,
			   int32_t layer,
			   int64_t start_tile_x, int64_t start_tile_y,
			   int64_t end_tile_x, int64_t end_tile_y,
			   int32_t offset_x, int32_t offset_y,
			   float advance_x, float advance_y,
			   openslide_t *osr,
			   struct _openslide_cache *cache,
			   void (*read_tile)(openslide_t *osr,
					     cairo_t *cr,
					     int32_t layer,
					     int64_t tile_x, int64_t tile_y,
					     struct _openslide_cache *cache)) {
  cairo_save(cr);
  //  cairo_set_source_rgb(cr, 0, 1, 0);
  //  cairo_paint(cr);
  //g_debug("offset: %d %d", offset_x, offset_y);
  cairo_translate(cr, -offset_x, -offset_y);

  //g_debug("start: %" PRId64 " %" PRId64, start_tile_x, start_tile_y);
  //g_debug("end: %" PRId64 " %" PRId64, end_tile_x, end_tile_y);

  int64_t tile_y = start_tile_y;

  while (tile_y < end_tile_y) {
    cairo_save(cr);

    int64_t tile_x = start_tile_x;
    while (tile_x < end_tile_x) {
      //      g_debug("read_tiles %" PRId64 " %" PRId64, tile_x, tile_y);
      read_tile(osr, cr, layer, tile_x, tile_y, cache);

      tile_x++;
      cairo_translate(cr, advance_x, 0);
    }

    cairo_restore(cr);
    tile_y++;
    cairo_translate(cr, 0, advance_y);
  }
  cairo_restore(cr);
}


void _openslide_convert_coordinate(double downsample,
				   int64_t x, int64_t y,
				   int64_t tiles_across, int64_t tiles_down,
				   int32_t raw_tile_width,
				   int32_t raw_tile_height,
				   int32_t overlap_per_megatile_x,
				   int32_t overlap_per_megatile_y,
				   int32_t tiles_per_megatile_x,
				   int32_t tiles_per_megatile_y,
				   int64_t *tile_x, int64_t *tile_y,
				   int32_t *offset_x_in_tile,
				   int32_t *offset_y_in_tile) {
  g_assert(tiles_per_megatile_x > 0);
  g_assert(tiles_per_megatile_y > 0);

  // downsample coordinates
  int64_t ds_x = x / downsample;
  int64_t ds_y = y / downsample;

  // compute "megatile" (concatenate tiles to ease overlap computation)
  // overlap comes at the end of a megatile
  int32_t megatile_w = (raw_tile_width * tiles_per_megatile_x) - overlap_per_megatile_x;
  int32_t megatile_h = (raw_tile_height * tiles_per_megatile_y) - overlap_per_megatile_y;

  int64_t megatile_x = ds_x / megatile_w;
  int64_t megatile_y = ds_y / megatile_h;

  // now find where we are in the megatile
  ds_x -= megatile_x * megatile_w;
  ds_y -= megatile_y * megatile_h;

  // x
  int32_t localtile_x = ds_x / raw_tile_width;
  *tile_x = megatile_x * tiles_per_megatile_x + localtile_x;
  *offset_x_in_tile = ds_x % raw_tile_width;
  if (*tile_x >= tiles_across - 1) {
    // this is the last tile, adjust it, but not the offset
    *tile_x = tiles_across - 1;
  }

  // y
  int32_t localtile_y = ds_y / raw_tile_height;
  *tile_y = megatile_y * tiles_per_megatile_y + localtile_y;
  *offset_y_in_tile = ds_y % raw_tile_height;
  if (*tile_y >= tiles_down - 1) {
    // this is the last tile, adjust it, but not the offset
    *tile_y = tiles_down - 1;
  }

  /*
  g_debug("convert_coordinate: (%" PRId64 ",%" PRId64") ->"
	  " t(%" PRId64 ",%" PRId64 ") + (%d,%d)",
	  x, y, *tile_x, *tile_y, *offset_x_in_tile, *offset_y_in_tile);
  */
}
