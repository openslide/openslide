/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
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

#include "openslide.h"
#include "openslide-common.h"
#include "slidetool.h"

#include <png.h>
#include <inttypes.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>

static const char SOFTWARE[] = "Software";
static const char OPENSLIDE[] = "OpenSlide <https://openslide.org/>";
static const uint32_t BUFSIZE = 16 << 20;

#define ENSURE_NONNEG(i) \
  if (i < 0) {                               \
    common_fail(#i " must be non-negative"); \
  }

#define ENSURE_POS(i) \
  if (i <= 0) {                          \
    common_fail(#i " must be positive"); \
  }

static void write_png(openslide_t *osr, FILE *f,
                      int64_t x, int64_t y, int32_t level,
                      int32_t w, const int32_t h) {
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                                NULL, NULL, NULL);
  if (!png_ptr) {
    common_fail("Could not initialize PNG");
  }

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    common_fail("Could not initialize PNG");
  }

  if (setjmp(png_jmpbuf(png_ptr))) {
    common_fail("Error writing PNG");
  }

  png_init_io(png_ptr, f);

  png_set_IHDR(png_ptr, info_ptr, w, h, 8,
               PNG_COLOR_TYPE_RGB_ALPHA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);

  // text
  png_text text_ptr[1];
  memset(text_ptr, 0, sizeof text_ptr);
  text_ptr[0].compression = PNG_TEXT_COMPRESSION_NONE;
  g_autofree char *key = g_strdup(SOFTWARE);
  text_ptr[0].key = key;
  g_autofree char *text = g_strdup(OPENSLIDE);
  text_ptr[0].text = text;
  text_ptr[0].text_length = strlen(text);

  png_set_text(png_ptr, info_ptr, text_ptr, 1);

  // background
  const char *bgcolor =
    openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BACKGROUND_COLOR);
  if (bgcolor) {
    unsigned r, g, b;
    sscanf(bgcolor, "%2x%2x%2x", &r, &g, &b);

    png_color_16 background = { 0, r, g, b, 0 };
    png_set_bKGD(png_ptr, info_ptr, &background);
  }

  // start writing
  png_write_info(png_ptr, info_ptr);

  const int32_t lines_at_a_time = MAX(BUFSIZE / (w * 4), 1);
  g_autofree uint32_t *dest = g_malloc(lines_at_a_time * w * 4);
  int32_t lines_to_draw = h;
  double ds = openslide_get_level_downsample(osr, level);
  int32_t yy = y / ds;
  while (lines_to_draw) {
    const int32_t lines = MIN(lines_at_a_time, lines_to_draw);
    openslide_read_region(osr, dest,
                          x, yy * ds, level, w, lines);

    common_fail_on_error(osr, "Reading region");

    // un-premultiply alpha and pack into expected format
    for (int32_t i = 0; i < w * lines; i++) {
      uint32_t p = dest[i];

      uint8_t a = p >> 24;
      switch (a) {
      case 0:
        dest[i] = 0;
        break;

      case 255:
        dest[i] = GUINT32_TO_BE(p << 8 | p >> 24);
        break;

      default:
        ; // make compiler happy
        uint8_t r = (((p >> 16) & 0xff) * 255 + a / 2) / a;
        uint8_t g = (((p >> 8) & 0xff) * 255 + a / 2) / a;
        uint8_t b = ((p & 0xff) * 255 + a / 2) / a;
        dest[i] = GUINT32_TO_BE(r << 24 | g << 16 | b << 8 | a);
      }
    }

    for (int32_t i = 0; i < lines; i++) {
      png_write_row(png_ptr, (png_bytep) &dest[w * i]);
    }
    yy += lines;
    lines_to_draw -= lines;
  }

  // end
  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
}

static int do_write_png(int narg, char **args) {
  // get args
  g_assert(narg == 7);
  const char *slide = args[0];
  int64_t x = g_ascii_strtoll(args[1], NULL, 10);
  int64_t y = g_ascii_strtoll(args[2], NULL, 10);
  int32_t level = strtol(args[3], NULL, 10);
  int64_t width = g_ascii_strtoll(args[4], NULL, 10);
  int64_t height = g_ascii_strtoll(args[5], NULL, 10);
  const char *output = args[6];

  // open slide
  g_autoptr(openslide_t) osr = openslide_open(slide);

  // check errors
  common_fail_on_error(osr, "%s", slide);

  // validate args
  ENSURE_NONNEG(level);
  if (level > openslide_get_level_count(osr) - 1) {
    common_fail("level %d out of range (level count %d)",
                level, openslide_get_level_count(osr));
  }
  ENSURE_POS(width);
  ENSURE_POS(height);
  if (width > INT32_MAX) {
    common_fail("width must be <= %d for PNG", INT32_MAX);
  }
  if (height > INT32_MAX) {
    common_fail("height must be <= %d for PNG", INT32_MAX);
  }

  // set up output file
  FILE *png = fopen(output, "wb");
  if (!png) {
    common_fail("Can't open %s for writing: %s", output, strerror(errno));
  }

  write_png(osr, png, x, y, level, width, height);

  fclose(png);

  return 0;
}

const struct command write_png_cmd = {
  .parameter_string = "slide x y level width height output.png",
  .summary = "Write a region of a virtual slide to a PNG.",
  .min_positional = 7,
  .max_positional = 7,
  .handler = do_write_png,
};
