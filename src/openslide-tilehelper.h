/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
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

#ifndef OPENSLIDE_OPENSLIDE_TILEHELPER_H_
#define OPENSLIDE_OPENSLIDE_TILEHELPER_H_

#include <config.h>

#include "openslide-private.h"

#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>

void _openslide_read_tiles(cairo_t *cr,
			   struct _openslide_level *level,
			   int64_t start_tile_x, int64_t start_tile_y,
			   int64_t end_tile_x, int64_t end_tile_y,
			   double offset_x, double offset_y,
			   double advance_x, double advance_y,
			   openslide_t *osr,
			   void *arg,
			   _openslide_tileread_fn read_tile);
#endif
