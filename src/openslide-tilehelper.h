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

#ifndef OPENSLIDE_OPENSLIDE_TILEHELPER_H_
#define OPENSLIDE_OPENSLIDE_TILEHELPER_H_

#include "openslide.h"
#include "openslide-cache.h"

#include <stdbool.h>
#include <stdint.h>

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
				   int32_t *offset_y_in_tile);

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
					     int64_t tile_w, int64_t tile_h),
			   struct _openslide_cache *cache);
#endif
