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
  while (h) {
    if (dest != NULL) {
      memcpy(dest + (dest_y * dest_w + dest_x),
	     tile + (src_y * src_w + src_x),
	     4 * w);
    }
    src_y++;
    dest_y++;
    h--;
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
  //  g_debug("dest_w: %" PRId64 ", dest_h: %" PRId64, dest_w, dest_h);

  int64_t tile_y = start_tile_y;
  int64_t dest_y = 0;

  int64_t src_y = offset_y;

  while ((dest_y < dest_h) && (tile_y < tiles_down)) {
    int64_t tile_x = start_tile_x;
    int64_t dest_x = 0;

    int32_t src_h = (tile_y == tiles_down - 1) ? last_tile_height : tile_height;
    int32_t copy_h = MIN(src_h - src_y, dest_h - dest_y);

    int64_t src_x = offset_x;

    while ((dest_x < dest_w) && (tile_x < tiles_across)) {
      int32_t src_w = (tile_x == tiles_across - 1) ? last_tile_width : tile_width;
      int32_t copy_w = MIN(src_w - src_x, dest_w - dest_x);

      //g_debug("%" PRId64 " %" PRId64 ", %dx%d", tile_x, tile_y, src_w, src_h);

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
	if (read_tile(osr, new_tile, layer, tile_x, tile_y)) {
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
