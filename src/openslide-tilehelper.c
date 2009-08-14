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
#include <math.h>
#include <cairo.h>

void _openslide_read_tiles(cairo_t *cr,
			   int32_t layer,
			   int64_t start_tile_x, int64_t start_tile_y,
			   int64_t end_tile_x, int64_t end_tile_y,
			   double offset_x, double offset_y,
			   double advance_x, double advance_y,
			   openslide_t *osr,
			   struct _openslide_cache *cache,
			   void (*read_tile)(openslide_t *osr,
					     cairo_t *cr,
					     int32_t layer,
					     int64_t tile_x, int64_t tile_y,
					     struct _openslide_cache *cache)) {
  //g_debug("offset: %g %g, advance: %g %g", offset_x, offset_y, advance_x, advance_y);
  g_return_if_fail(fabs(offset_x) < advance_x);
  g_return_if_fail(fabs(offset_y) < advance_y);

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
