/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
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

#include <stdint.h>

void _openslide_read_tiles(int64_t start_x, int64_t start_y, int64_t end_x, int64_t end_y,
			   int32_t ovr_x, int32_t ovr_y,
			   int64_t dest_w, int64_t dest_h,
			   int32_t layer,
			   int64_t tw, int64_t th,
			   void (*tilereader_read)(void *tilereader_data,
						   uint32_t *dest, int64_t x, int64_t y),
			   void *tilereader_data,
			   uint32_t *dest,
			   struct _openslide_cache *cache);

#endif
