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

static void copy_tile(const uint32_t *tile,
		      uint32_t *dest,
		      int32_t w, int32_t h,
		      int64_t src_x, int64_t src_y, int32_t src_w,
		      int64_t dest_x, int64_t dest_y, int32_t dest_w) {
  if (dest != NULL) {
    dest += (dest_y * dest_w) + dest_x;
    tile += (src_y * src_w) + src_x;
    while (h) {
      // copy 1 row
      memcpy(dest, tile, 4 * w);
      tile += src_w;
      dest += dest_w;
      h--;
    }
  }
}


void _openslide_read_tiles(uint32_t *dest,
			   int64_t dest_w, int64_t dest_h,
			   int32_t layer,
			   int64_t start_tile_x, int64_t start_tile_y,
			   int32_t offset_x, int32_t offset_y,
			   openslide_t *osr,
			   int32_t (*get_tile_width)(openslide_t *osr,
						     int32_t layer,
						     int64_t tile_x),
			   int32_t (*get_tile_height)(openslide_t *osr,
						      int32_t layer,
						      int64_t tile_y),
			   bool (*read_tile)(openslide_t *osr,
					     uint32_t *dest,
					     int32_t layer,
					     int64_t tile_x, int64_t tile_y,
					     int32_t tile_w, int32_t tile_h),
			   struct _openslide_cache *cache) {
  //  g_debug("dest_w: %" PRId64 ", dest_h: %" PRId64, dest_w, dest_h);

  int64_t tile_y = start_tile_y;
  int64_t dest_y = 0;
  int64_t src_y = offset_y;

  while (dest_y < dest_h) {
    int32_t src_h = get_tile_height(osr, layer, tile_y);
    int32_t copy_h = MIN(src_h - src_y, dest_h - dest_y);

    // are we at the end?
    if (!src_h) {
      break;
    }

    g_assert(copy_h > 0);

    int64_t tile_x = start_tile_x;
    int64_t dest_x = 0;
    int64_t src_x = offset_x;

    while (dest_x < dest_w) {
      int32_t src_w = get_tile_width(osr, layer, tile_x);
      int32_t copy_w = MIN(src_w - src_x, dest_w - dest_x);

      // are we at the end of the row?
      if (!src_w) {
	break;
      }

      g_assert(copy_w > 0);

      int tile_size = src_h * src_w * 4;
      uint32_t *cache_tile = _openslide_cache_get(cache, tile_x, tile_y, layer);
      uint32_t *new_tile = NULL;

      if (cache_tile != NULL) {
	// use cached tile
	copy_tile(cache_tile, dest,
		  copy_w, copy_h,
		  src_x, src_y, src_w,
		  dest_x, dest_y, dest_w);
      } else {
	// make new tile
	new_tile = g_slice_alloc(tile_size);

	// read_tile will return true only if there is data there
	if (read_tile(osr, new_tile, layer, tile_x, tile_y, src_w, src_h)) {
	  /*
	  for (int yy = 0; yy < src_h; yy++) {
	    for (int xx = 0; xx < src_w; xx++) {
	      if (xx == 0 || yy == 0) {
		new_tile[yy * src_w + xx] = 0xFF0000FF; // blue bounding box
	      }
	    }
	  }
	  */

	  copy_tile(new_tile, dest,
		    copy_w, copy_h,
		    src_x, src_y, src_w,
		    dest_x, dest_y, dest_w);
	} else {
	  g_slice_free1(tile_size, new_tile);
	  new_tile = NULL;
	}
      }

      if (new_tile != NULL) {
	// if not cached already, store into the cache
	_openslide_cache_put(cache, tile_x, tile_y, layer, new_tile, tile_size);

	/*
	for (int yy = 0; yy < src_h; yy++) {
	  for (int xx = 0; xx < src_w; xx++) {
	    if (xx == 0 || yy == 0) {
	      new_tile[yy * src_w + xx] = 0xFFFF0000; // red bounding box
	    }
	  }
	}
	*/
      }

      tile_x++;
      dest_x += copy_w;
      src_x = 0;
    }

    tile_y++;
    dest_y += copy_h;
    src_y = 0;
  }
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
