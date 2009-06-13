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

static void copy_tile(const uint32_t *tile,
		      uint32_t *dest,
		      int64_t src_w, int64_t src_h,
		      int64_t dest_origin_x, int64_t dest_origin_y,
		      int64_t dest_w, int64_t dest_h) {
  if (dest == NULL) {
    return;
  }

  int64_t src_origin_y;
  if (dest_origin_y < 0) {  // off the top
    src_origin_y = -dest_origin_y;
  } else {
    src_origin_y = 0;
  }

  //  g_debug("src_origin_y: %d, dest_origin_y: %d", src_origin_y, dest_origin_y);

  int64_t src_origin_x;
  if (dest_origin_x < 0) {  // off the left
    src_origin_x = -dest_origin_x;
  } else {
    src_origin_x = 0;
  }

  //  g_debug("src_origin_x: %d, dest_origin_x: %d", src_origin_x, dest_origin_x);

  //  g_debug("");

  int64_t src_y = src_origin_y;
  int64_t dest_y = dest_origin_y + src_y;

  while ((src_y < src_h) && (dest_y < dest_h)) {
    int64_t dest_x = dest_origin_x + src_origin_x;

    memcpy(dest + (dest_y * dest_w + dest_x),
	   tile + (src_y * src_w + src_origin_x),
	   4 * MIN(src_w - src_origin_x, dest_w - dest_x));

    src_y++;
    dest_y++;
  }
}


void _openslide_read_tiles(int64_t start_tile_x, int64_t start_tile_y,
			   int32_t offset_x, int32_t offset_y,
			   int64_t dest_w, int64_t dest_h,
			   int32_t layer,
			   int32_t tile_width, int32_t tile_height,
			   int32_t last_tile_width, int32_t last_tile_height,
			   int64_t tiles_across, int64_t tiles_down,
			   bool (*read_tile)(openslide_t *osr,
					     uint32_t *dest,
					     int32_t layer,
					     int64_t tile_x, int64_t tile_y),
			   openslide_t *osr,
			   uint32_t *dest,
			   struct _openslide_cache *cache) {
  int64_t tile_y = start_tile_y;
  int64_t dst_y = 0;

  while (dst_y < dest_h) {
    int64_t tile_x = start_tile_x;
    int64_t dst_x = 0;

    int32_t th = (tile_y == tiles_down - 1) ? last_tile_height : tile_height;

    while (dst_x < dest_w) {
      int32_t tw = (tile_x == tiles_across - 1) ? last_tile_width : tile_width;

      int tile_size = th * tw * 4;
      uint32_t *cache_tile = _openslide_cache_get(cache, tile_x, tile_y, layer);
      uint32_t *new_tile = NULL;

      if (cache_tile != NULL) {
	// use cached tile
	copy_tile(cache_tile, dest, tw, th,
		  dst_x - offset_x, dst_y - offset_y,
		  dest_w, dest_h);
      } else {
	// make new tile
	new_tile = g_slice_alloc(tile_size);

	// read_tile will return true only if there is data there
	if (read_tile(osr, new_tile, layer, tile_x, tile_y)) {
	  /*
	  for (int yy = 0; yy < th; yy++) {
	    for (int xx = 0; xx < tw; xx++) {
	      if (xx == 0 || yy == 0 || xx == (tw - 1) || yy == (th - 1)) {
		new_tile[yy * tw + xx] = 0xFF0000FF; // blue bounding box
	      }
	    }
	  }
	  */
	  copy_tile(new_tile, dest, tw, th,
		    dst_x - offset_x, dst_y - offset_y,
		    dest_w, dest_h);
	} else {
	  g_slice_free1(tile_size, new_tile);
	  new_tile = NULL;
	}
      }

      if (new_tile != NULL) {
	// if not cached already, store into the cache
	_openslide_cache_put(cache, tile_x, tile_y, layer, new_tile, tile_size);
      }

      tile_x++;
      dst_x += tw;
    }

    tile_y++;
    dst_y += th;
  }
}
