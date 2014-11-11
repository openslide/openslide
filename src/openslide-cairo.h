/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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

/**
 * @file openslide-cairo.h
 * The cairo interface to the OpenSlide library.
 */

#ifndef OPENSLIDE_OPENSLIDE_CAIRO_H_
#define OPENSLIDE_OPENSLIDE_CAIRO_H_

#include "openslide-features.h"
#include "openslide.h"

#include <cairo.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @name Cairo
 * Interface to OpenSlide from cairo.
 */
//@{

/**
 * Draw a region from a whole slide image into a cairo context.
 *
 * This function draws a region of a whole slide image into the given
 * cairo context at the origin. @p cr must be a valid cairo context.
 *
 * @param osr The OpenSlide object.
 * @param cr The destination cairo context.
 * @param x The top left x-coordinate, in the level 0 reference frame.
 * @param y The top left y-coordinate, in the level 0 reference frame.
 * @param level The desired level.
 * @param w The width of the region. Must be non-negative.
 * @param h The height of the region. Must be non-negative.
 */
// too soon to enable this, once we do there's no removing it,
// have to think about win32 implications with different cairo DLLs,
// with this issue there's no point adding it until we need it, like
// for ARGB64 or whatever if that gets into cairo
//
// to enable this, remove these comments, then update Makefile.am, Doxyfile
//OPENSLIDE_PUBLIC()
void openslide_cairo_read_region(openslide_t *osr,
				 cairo_t *cr,
				 int64_t x, int64_t y,
				 int32_t level,
				 int64_t w, int64_t h);
//@}

#ifdef __cplusplus
}
#endif

#endif
