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

#include <config.h>

#include "openslide-tilehelper.h"
#include "openslide-private.h"

#include <glib.h>
#include <string.h>
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
					     double translate_x, double translate_y,
					     struct _openslide_cache *cache)) {
  //g_debug("offset: %g %g, advance: %g %g", offset_x, offset_y, advance_x, advance_y);
  if (fabs(offset_x) >= advance_x) {
    _openslide_set_error(osr, "fabs(offset_x) >= advance_x");
    return;
  }
  if (fabs(offset_y) >= advance_y) {
    _openslide_set_error(osr, "fabs(offset_y) >= advance_y");
    return;
  }

  //  cairo_set_source_rgb(cr, 0, 1, 0);
  //  cairo_paint(cr);
  //g_debug("offset: %d %d", offset_x, offset_y);

  //g_debug("start: %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, start_tile_x, start_tile_y);
  //g_debug("end: %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, end_tile_x, end_tile_y);

  int64_t tile_y = end_tile_y - 1;

  while (tile_y >= start_tile_y) {
    double translate_y = ((tile_y - start_tile_y) * advance_y) - offset_y;
    int64_t tile_x = end_tile_x - 1;

    while (tile_x >= start_tile_x) {
      double translate_x = ((tile_x - start_tile_x) * advance_x) - offset_x;
      //      g_debug("read_tiles %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, tile_x, tile_y);
      read_tile(osr, cr, layer, tile_x, tile_y, translate_x, translate_y, cache);
      tile_x--;
    }

    tile_y--;
  }
}
