/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
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

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <glib.h>
#include <openslide.h>
#include "openslide-common.h"

#include "config.h"
#ifdef HAVE_VALGRIND
#include <callgrind.h>
#endif

#define BUFWIDTH    1000
#define BUFHEIGHT   1000
#define MAXWIDTH   10000
#define MAXHEIGHT  10000

int main(int argc, char **argv) {
  common_fix_argv(&argc, &argv);
  if (argc != 3) {
    common_fail("Usage: %s <slide> <level>", argv[0]);
  }
  const char *path = argv[1];
  int level = atoi(argv[2]);

  g_autoptr(openslide_t) osr = openslide_open(path);
  if (!osr) {
    common_fail("Couldn't open %s", path);
  }
  const char *err = openslide_get_error(osr);
  if (err) {
    common_fail("Open failed: %s", err);
  }
  if (level >= openslide_get_level_count(osr)) {
    common_fail("No such level: %d", level);
  }

  // Get dimensions
  int64_t x = 0;
  int64_t y = 0;
  int64_t w, h;
  openslide_get_level_dimensions(osr, level, &w, &h);

  // Read from active region, if denoted
  const char *bounds_x = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BOUNDS_X);
  const char *bounds_y = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BOUNDS_Y);
  const char *bounds_w = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BOUNDS_WIDTH);
  const char *bounds_h = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BOUNDS_HEIGHT);
  if (bounds_x && bounds_y) {
    x = g_ascii_strtoll(bounds_x, NULL, 10);
    y = g_ascii_strtoll(bounds_y, NULL, 10);
  }
  if (bounds_w && bounds_h) {
    double downsample = openslide_get_level_downsample(osr, level);
    w = g_ascii_strtoll(bounds_w, NULL, 10) / downsample;
    h = g_ascii_strtoll(bounds_h, NULL, 10) / downsample;
  }

  w = MIN(w, MAXWIDTH);
  h = MIN(h, MAXHEIGHT);
  g_autofree uint32_t *buf = g_new(uint32_t, BUFWIDTH * BUFHEIGHT);

  printf("Reading (%"PRId64", %"PRId64") in level %d for "
         "%"PRId64" x %"PRId64"\n\n", x, y, level, w, h);

#ifdef HAVE_VALGRIND
  CALLGRIND_START_INSTRUMENTATION;
#endif

  for (int yy = 0; yy < h; yy += BUFHEIGHT) {
    for (int xx = 0; xx < w; xx += BUFWIDTH) {
      openslide_read_region(osr, buf, x + xx, y + yy, level,
                            MIN(BUFWIDTH, w - xx), MIN(BUFHEIGHT, h - yy));
    }
  }

#ifdef HAVE_VALGRIND
  CALLGRIND_STOP_INSTRUMENTATION;
#endif

  err = openslide_get_error(osr);
  if (err) {
    common_fail("Read failed: %s", err);
  }

  return 0;
}
