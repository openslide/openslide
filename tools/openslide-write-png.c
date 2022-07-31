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

#include <png.h>
#include <inttypes.h>
#include <glib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>

static const char SOFTWARE[] = "Software";
static const char OPENSLIDE[] = "OpenSlide <https://openslide.org/>";

#define ENSURE_NONNEG(i) \
  if (i < 0) {					\
    fail(#i " must be non-negative");	\
  }

#define ENSURE_POS(i) \
  if (i <= 0) {					\
    fail(#i " must be positive");	\
  }

static void fail(const char *format, ...) G_GNUC_NORETURN;
static void fail(const char *format, ...) {
  va_list ap;

  va_start(ap, format);
  char *msg = g_strdup_vprintf(format, ap);
  va_end(ap);

  fprintf(stderr, "%s: %s\n", g_get_prgname(), msg);
  fflush(stderr);

  exit(1);
}


static void write_png(openslide_t *osr, FILE *f,
		      int64_t x, int64_t y, int32_t level,
		      int32_t w, const int32_t h) {
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
						NULL, NULL, NULL);
  if (!png_ptr) {
    fail("Could not initialize PNG");
  }

  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    fail("Could not initialize PNG");
  }

  if (setjmp(png_jmpbuf(png_ptr))) {
    fail("Error writing PNG");
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
    int r, g, b;
    sscanf(bgcolor, "%2x%2x%2x", &r, &g, &b);

    png_color_16 background = { 0, r, g, b, 0 };
    png_set_bKGD(png_ptr, info_ptr, &background);
  }

  // start writing
  png_write_info(png_ptr, info_ptr);

  g_autofree uint32_t *dest = g_malloc(w * 4);
  int32_t lines_to_draw = h;
  double ds = openslide_get_level_downsample(osr, level);
  int32_t yy = y / ds;
  while (lines_to_draw) {
    openslide_read_region(osr, dest,
			  x, yy * ds, level, w, 1);

    const char *err = openslide_get_error(osr);
    if (err) {
      fail("%s", err);
    }

    // un-premultiply alpha and pack into expected format
    for (int i = 0; i < w; i++) {
      uint32_t p = dest[i];
      uint8_t *p8 = (uint8_t *) (dest + i);

      uint8_t a = (p >> 24) & 0xFF;
      uint8_t r = (p >> 16) & 0xFF;
      uint8_t g = (p >> 8) & 0xFF;
      uint8_t b = p & 0xFF;

      switch (a) {
      case 0:
	r = 0;
	b = 0;
	g = 0;
	break;

      case 255:
	// no action
	break;

      default:
	r = (r * 255 + a / 2) / a;
	g = (g * 255 + a / 2) / a;
	b = (b * 255 + a / 2) / a;
	break;
      }

      // write back
      p8[0] = r;
      p8[1] = g;
      p8[2] = b;
      p8[3] = a;
    }

    png_write_row(png_ptr, (png_bytep) dest);
    yy++;
    lines_to_draw--;
  }

  // end
  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
}


static const struct common_usage_info usage_info = {
  "slide x y level width height output.png",
  "Write a region of a virtual slide to a PNG.",
};

int main (int argc, char **argv) {
  common_parse_commandline(&usage_info, &argc, &argv);
  if (argc != 8) {
    common_usage(&usage_info);
  }

  // get args
  const char *slide = argv[1];
  int64_t x = g_ascii_strtoll(argv[2], NULL, 10);
  int64_t y = g_ascii_strtoll(argv[3], NULL, 10);
  int32_t level = strtol(argv[4], NULL, 10);
  int64_t width = g_ascii_strtoll(argv[5], NULL, 10);
  int64_t height = g_ascii_strtoll(argv[6], NULL, 10);
  const char *output = argv[7];

  // open slide
  g_autoptr(openslide_t) osr = openslide_open(slide);

  // check errors
  if (osr == NULL) {
    fail("%s: Not a file that OpenSlide can recognize", slide);
  }

  const char *err = openslide_get_error(osr);
  if (err) {
    fail("%s: %s", slide, err);
  }

  // validate args
  ENSURE_NONNEG(level);
  if (level > openslide_get_level_count(osr) - 1) {
    fail("level %d out of range (level count %d)",
	 level, openslide_get_level_count(osr));
  }
  ENSURE_POS(width);
  ENSURE_POS(height);
  if (width > INT32_MAX) {
    fail("width must be <= %d for PNG", INT32_MAX);
  }
  if (height > INT32_MAX) {
    fail("height must be <= %d for PNG", INT32_MAX);
  }

  // set up output file
  FILE *png = fopen(output, "wb");
  if (!png) {
    fail("Can't open %s for writing: %s", output, strerror(errno));
  }

  write_png(osr, png, x, y, level, width, height);

  fclose(png);

  return 0;
}
