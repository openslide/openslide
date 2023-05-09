/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
 *  Copyright (c) 2023 Alexandr Virodov
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
#include <setjmp.h>

static const char SOFTWARE[] = "Software";
static const char OPENSLIDE[] = "OpenSlide <https://openslide.org/>";

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


static void write_png(FILE *f, int64_t w, int64_t h, uint32_t* data) {
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
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

  // start writing
  png_write_info(png_ptr, info_ptr);
  for (int i = 0; i < h; ++i) {
    png_write_row(png_ptr, (png_bytep) &data[w * i]);
  }

  // end
  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
}


static const struct common_usage_info usage_info = {
  "\n\tslide - to list associated images.\n\tslide associated_image output.png - to write an associated image.",
  "Write an associated image of a virtual slide to a PNG.",
};

enum mode_t {
  LIST_IMAGES,
  WRITE_IMAGE,
};

int main (int argc, char **argv) {
  enum mode_t mode;
  common_parse_commandline(&usage_info, &argc, &argv);

  switch (argc) {
    case 2: mode = LIST_IMAGES; break;
    case 4: mode = WRITE_IMAGE; break;
    default: common_usage(&usage_info); // calls exit().
  }

  const char *slide = argv[1];
  printf("Opening slide: '%s'\n", slide);

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

  if (mode == LIST_IMAGES) {
    printf("Listing associated images:\n");
    const char *const *associated_image_names = openslide_get_associated_image_names(osr);
    while (*associated_image_names) {
      printf("associated image: '%s'\n", *associated_image_names);
      associated_image_names++;
    }
    printf("Done listing.\n");

  } else if (mode == WRITE_IMAGE) {
    const char *associated_image_name = argv[2];
    const char *output_file = argv[3];
    printf("Extracting associated image: '%s'\n", associated_image_name);

    int64_t w;
    int64_t h;
    openslide_get_associated_image_dimensions(osr, associated_image_name, &w, &h);
    printf("Dimensions: %ld, %ld\n", w, h);

    g_autofree uint32_t *buffer = g_malloc(w * h * 4);
    openslide_read_associated_image(osr, associated_image_name, buffer);

    printf("Writing output file: '%s'\n", output_file);
    FILE* f = fopen(output_file, "wb");
    write_png(f, w, h, buffer);
    fclose(f);
  }

  // openslide_close(osr);

  return 0;
}
