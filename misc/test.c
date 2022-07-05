/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
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

#define _GNU_SOURCE

#include "config.h"
#include "openslide.h"
#include "openslide-common.h"

#ifdef HAVE_VALGRIND
#include <callgrind.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/time.h>

#include <glib.h>
#include <cairo.h>
#include <cairo-pdf.h>

#include <math.h>


static void print_downsamples(openslide_t *osr) {
  for (int32_t level = 0; level < openslide_get_level_count(osr); level++) {
    printf("level %d: downsample: %g\n",
	   level,
	   openslide_get_level_downsample(osr, level));
  }
}

static void test_next_biggest(openslide_t *osr, double downsample) {
  int32_t level = openslide_get_best_level_for_downsample(osr, downsample);
  printf("level for downsample %g: %d (%g)\n",
	 downsample, level, openslide_get_level_downsample(osr, level));
}

/*
static void test_tile_walk(openslide_t *osr,
			   int64_t tile_size) {
  printf("test_tile_walk: %"PRId64"\n", tile_size);

  uint32_t *buf = malloc(tile_size * tile_size * 4);
  //struct timeval tv, tv2;

  int64_t w, h;
  openslide_get_level0_dimensions(osr, &w, &h);

  for (int64_t y = 0; y < h; y += tile_size) {
    for (int64_t x = 0; x < w; x += tile_size) {
      //gettimeofday(&tv, NULL);
      openslide_read_region(osr, buf, x, y, 0, tile_size, tile_size);
      //gettimeofday(&tv2, NULL);
      //printf("time: %d\n", (tv2.tv_sec - tv.tv_sec) * 1000 + (tv2.tv_usec - tv.tv_usec) / 1000);
    }
  }

  free(buf);
}
*/

static uint8_t apply_alpha(uint8_t s, uint8_t a, uint8_t d) {
  double ss = s / 255.0;
  double aa = a / 255.0;
  double dd = d / 255.0;
  return round((ss + (1 - aa) * dd) * 255.0);
}

static void write_as_ppm(const char *filename,
			 int64_t w, int64_t h, uint32_t *buf,
			 uint8_t br, uint8_t bg, uint8_t bb) {
  FILE *f = fopen(filename, "wb");
  if (f == NULL) {
    perror("Cannot open file");
    return;
  }

  fprintf(f, "P6\n%"PRId64" %"PRId64"\n255\n", w, h);
  for (int64_t i = 0; i < w * h; i++) {
    uint32_t val = buf[i];
    uint8_t a = (val >> 24) & 0xFF;
    uint8_t r = (val >> 16) & 0xFF;
    uint8_t g = (val >> 8) & 0xFF;
    uint8_t b = (val >> 0) & 0xFF;

    // composite against background with OVER
    r = apply_alpha(r, a, br);
    g = apply_alpha(g, a, bg);
    b = apply_alpha(b, a, bb);

    putc(r, f);
    putc(g, f);
    putc(b, f);
  }

  fclose(f);
}

static void test_image_fetch(openslide_t *osr,
			     const char *name,
			     int64_t x, int64_t y,
			     int64_t w, int64_t h,
			     bool skip_write) {
  char *filename;

  uint8_t bg_r = 0xFF;
  uint8_t bg_g = 0xFF;
  uint8_t bg_b = 0xFF;

  const char *bgcolor = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BACKGROUND_COLOR);
  if (bgcolor) {
    uint64_t bg = g_ascii_strtoull(bgcolor, NULL, 16);
    bg_r = (bg >> 16) & 0xFF;
    bg_g = (bg >> 8) & 0xFF;
    bg_b = bg & 0xFF;
    printf("background: (%d, %d, %d)\n", bg_r, bg_g, bg_b);
  }

  printf("test image fetch %s\n", name);
  //  for (int32_t level = 0; level < 1; level++) {
  for (int32_t level = 0; level < openslide_get_level_count(osr); level++) {
    filename = g_strdup_printf("%s-%.2d.ppm", name, level);
    int64_t num_bytes = w * h * 4;
    printf("Going to allocate %"PRId64" bytes...\n", num_bytes);
    uint32_t *buf = malloc(num_bytes);

    printf("x: %"PRId64", y: %"PRId64", level: %d, w: %"PRId64", h: %"PRId64"\n", x, y, level, w, h);
    openslide_read_region(osr, buf, x, y, level, w, h);

    // write as PPM
    if (!skip_write) {
      write_as_ppm(filename, w, h, buf, bg_r, bg_g, bg_b);
    }

    free(buf);
    g_free(filename);
  }
}

/*
static void test_horizontal_walk(openslide_t *osr,
				 int64_t start_x,
				 int64_t y,
				 int32_t level,
				 int64_t patch_w, int64_t patch_h,
				 int stride) {
  int64_t w, h;
  openslide_get_level_dimensions(osr, level, &w, &h);
  int64_t d = MIN(w,h);

  uint32_t *buf = malloc(patch_w * patch_h * 4);

  for (int64_t x = start_x; x < d; x += stride) {
    openslide_read_region(osr, buf, x, y, level, patch_w, patch_h);
    printf("%"PRId64"\r", x);
    fflush(stdout);
  }

  free(buf);
}

static void test_vertical_walk(openslide_t *osr,
			       int64_t x,
			       int64_t start_y,
			       int32_t level,
			       int64_t patch_w, int64_t patch_h,
			       int stride) {
  int64_t w, h;
  openslide_get_level_dimensions(osr, level, &w, &h);
  int64_t d = MIN(w,h);

  uint32_t *buf = malloc(patch_w * patch_h * 4);

  for (int64_t y = start_y; y < d; y += stride) {
    openslide_read_region(osr, buf, x, y, level, patch_w, patch_h);
    printf("%"PRId64"\r", y);
    fflush(stdout);
  }

  free(buf);
}

static void test_pdf(openslide_t *osr, const char *filename) {
  printf("test_pdf: %s\n", filename);
  cairo_surface_t *pdf = cairo_pdf_surface_create(filename, 0, 0);
  cairo_t *cr = cairo_create(pdf);
  cairo_rotate(cr, M_PI_4);

  for (int i = 0; i < openslide_get_level_count(osr); i++) {
    int64_t orig_w, orig_h;
    openslide_get_level_dimensions(osr, i, &orig_w, &orig_h);
    int64_t w = MIN(orig_w, 2000);
    int64_t h = MIN(orig_h, 2000);

    printf(" level %d (%"PRId64"x%"PRId64").", i, w, h);
    fflush(stdout);

    cairo_pdf_surface_set_size(pdf, w, h);
    printf(".");
    fflush(stdout);

    cairo_set_source_rgb(cr, 0.0, 0.0, 1.0);
    cairo_paint(cr);

    openslide_cairo_read_region(osr, cr, (orig_w - w) / 2, (orig_h - h) / 2, i, w, h);
    printf(".");
    fflush(stdout);

    cairo_show_page(cr);
    printf(" done\n");
  }

  cairo_surface_destroy(pdf);
  cairo_destroy(cr);
  printf(" done with pdf\n");
}
*/

int main(int argc, char **argv) {
  common_fix_argv(&argc, &argv);
  if (argc != 2) {
    printf("give file!\n");
    return 1;
  }

  //  struct timeval start_tv;
  //  struct timeval end_tv;

  printf("version: %s\n", openslide_get_version());

  printf("openslide_detect_vendor returns %s\n", openslide_detect_vendor(argv[1]));
  openslide_t *osr = openslide_open(argv[1]);

  int64_t w, h;

  if (osr == NULL || openslide_get_error(osr) != NULL) {
    printf("oh no\n");
    exit(1);
  }

  openslide_close(osr);

  osr = openslide_open(argv[1]);

  if (osr == NULL || openslide_get_error(osr) != NULL) {
    printf("oh no\n");
    exit(1);
  }

  openslide_get_level0_dimensions(osr, &w, &h);
  printf("dimensions: %"PRId64" x %"PRId64"\n", w, h);

  int32_t levels = openslide_get_level_count(osr);
  printf("num levels: %d\n", levels);

  for (int32_t i = -1; i < levels + 1; i++) {
    int64_t ww, hh;
    openslide_get_level_dimensions(osr, i, &ww, &hh);
    printf(" level %d dimensions: %"PRId64" x %"PRId64"\n", i, ww, hh);
  }

  print_downsamples(osr);

  test_next_biggest(osr, 0.8);
  test_next_biggest(osr, 1.0);
  test_next_biggest(osr, 1.5);
  test_next_biggest(osr, 2.0);
  test_next_biggest(osr, 3.0);
  test_next_biggest(osr, 3.1);
  test_next_biggest(osr, 10);
  test_next_biggest(osr, 20);
  test_next_biggest(osr, 25);
  test_next_biggest(osr, 100);
  test_next_biggest(osr, 1000);
  test_next_biggest(osr, 10000);

  //  int64_t elapsed;

  // test NULL dest
  openslide_read_region(osr, NULL, 0, 0, 0, 1000, 1000);

  // test empty dest
  uint32_t* item = 0;
  openslide_read_region(osr, item, 0, 0, 0, 0, 0);

  /*
  // test empty surface
  cairo_surface_t *surface =
    cairo_image_surface_create(CAIRO_FORMAT_RGB24, 0, 0);
  cairo_t *cr = cairo_create(surface);
  cairo_surface_destroy(surface);
  openslide_cairo_read_region(osr, cr, 0, 0, 0, 1000, 1000);
  cairo_destroy(cr);
  */

  // read properties
  const char * const *property_names = openslide_get_property_names(osr);
  while (*property_names) {
    const char *name = *property_names;
    const char *value = openslide_get_property_value(osr, name);
    printf("property: %s -> %s\n", name, value);

    property_names++;
  }

  // read associated images
  const char * const *associated_image_names = openslide_get_associated_image_names(osr);
  while (*associated_image_names) {
    int64_t w;
    int64_t h;
    const char *name = *associated_image_names;
    openslide_get_associated_image_dimensions(osr, name, &w, &h);

    printf("associated image: %s -> (%"PRId64"x%"PRId64")\n", name, w, h);

    uint32_t *buf = g_new(uint32_t, w * h);
    openslide_read_associated_image(osr, name, buf);
    g_free(buf);

    associated_image_names++;
  }

#ifdef HAVE_VALGRIND
  CALLGRIND_START_INSTRUMENTATION;
#endif
  /*
  // simulate horizonal scrolling?
  gettimeofday(&start_tv, NULL);
  printf("test_horizontal_walk start\n");
  test_horizontal_walk(osr, 0, 0, 0, 10, 400, 10);
  gettimeofday(&end_tv, NULL);
  elapsed = (end_tv.tv_sec * 1000 + end_tv.tv_usec / 1000) -
    (start_tv.tv_sec * 1000 + start_tv.tv_usec / 1000);

  printf("test_horizontal_walk end: %d\n", elapsed);


  // simulate vertical scrolling?
  gettimeofday(&start_tv, NULL);
  printf("test_vertical_walk start\n");
  test_vertical_walk(osr, 0, 0, 0, 400, 10, 10);
  gettimeofday(&end_tv, NULL);
  elapsed = (end_tv.tv_sec * 1000 + end_tv.tv_usec / 1000) -
    (start_tv.tv_sec * 1000 + start_tv.tv_usec / 1000);

  printf("test_vertical_walk end: %d\n", elapsed);

  */

  //  return 0;

  bool skip = true;

  //test_tile_walk(osr, 16);
  //test_tile_walk(osr, 4096);
  //test_tile_walk(osr, 256);

  //test_image_fetch(osr, "test0", 61000, 61000, 1024, 1024, skip);
  //test_image_fetch(osr, "test1", w/2, h/2, 1024, 1024, skip);
  //test_image_fetch(osr, "test2", w - 500, h - 300, 900, 800, skip);
  //test_image_fetch(osr, "test3", w*2, h*2, 900, 800, skip);
  //test_image_fetch(osr, "test4", 10, 10, 1900, 800, skip);
  //test_image_fetch(osr, "test5", w - 20, 0, 40, 100, skip);
  //test_image_fetch(osr, "test6", 0, h - 20, 100, 40, skip);
  test_image_fetch(osr, "test7", 0, 0, 200, 200, skip);

  // active region
  const char *bounds_x = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BOUNDS_X);
  const char *bounds_y = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BOUNDS_Y);
  if (bounds_x && bounds_y) {
    int64_t x = g_ascii_strtoll(bounds_x, NULL, 10);
    int64_t y = g_ascii_strtoll(bounds_y, NULL, 10);
    test_image_fetch(osr, "test8", x, y, 200, 200, skip);
  }

  //  test_pdf(osr, "test0.pdf");

#ifdef HAVE_VALGRIND
  CALLGRIND_STOP_INSTRUMENTATION;
#endif

  openslide_close(osr);

  return 0;
}
