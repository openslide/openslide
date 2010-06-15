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

#define _GNU_SOURCE

#include "config.h"
#include "openslide.h"

#include "callgrind.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef _MSC_VER
#include <sys/time.h>
#include <stdbool.h>
#endif

#include <glib.h>
#include <cairo.h>
#include <cairo-pdf.h>

#include <math.h>


static void print_downsamples(openslide_t *osr) {
  for (int32_t layer = 0; layer < openslide_get_layer_count(osr); layer++) {
    printf("layer %d: downsample: %g\n",
	   layer,
	   openslide_get_layer_downsample(osr, layer));
  }
}

static void test_next_biggest(openslide_t *osr, double downsample) {
  int32_t layer = openslide_get_best_layer_for_downsample(osr, downsample);
  printf("layer for downsample %g: %d (%g)\n",
	 downsample, layer, openslide_get_layer_downsample(osr, layer));
}

static void test_tile_walk(openslide_t *osr,
			   int64_t tile_size) {
  printf("test_tile_walk: %" G_GINT64_FORMAT "\n", tile_size);

  uint32_t *buf = (uint32_t *) malloc(tile_size * tile_size * 4);
  //struct timeval tv, tv2;

  int64_t w, h;
  openslide_get_layer0_dimensions(osr, &w, &h);

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

static void write_as_ppm(const char *filename,
			 int64_t w, int64_t h, uint32_t *buf) {
  FILE *f = fopen(filename, "w");
  if (f == NULL) {
    perror("Cannot open file");
    return;
  }

  fprintf(f, "P6\n%" G_GINT64_FORMAT " %" G_GINT64_FORMAT "\n255\n", w, h);
  for (int64_t i = 0; i < w * h; i++) {
    uint32_t val = buf[i];
    putc((val >> 16) & 0xFF, f); // R
    putc((val >> 8) & 0xFF, f);  // G
    putc((val >> 0) & 0xFF, f);  // B
    // no A
  }

  fclose(f);
}

static void test_image_fetch(openslide_t *osr,
			     const char *name,
			     int64_t x, int64_t y,
			     int64_t w, int64_t h,
			     bool skip_write) {
  char *filename;

  printf("test image fetch %s\n", name);
  //  for (int32_t layer = 0; layer < 1; layer++) {
  for (int32_t layer = 0; layer < openslide_get_layer_count(osr); layer++) {
    filename = g_strdup_printf("%s-%.2d.ppm", name, layer);
    int64_t num_bytes = w * h * 4;
    printf("Going to allocate %" G_GINT64_FORMAT " bytes...\n", num_bytes);
    uint32_t *buf = (uint32_t *) malloc(num_bytes);

    printf("x: %" G_GINT64_FORMAT ", y: %" G_GINT64_FORMAT ", layer: %d, w: %" G_GINT64_FORMAT ", h: %" G_GINT64_FORMAT "\n", x, y, layer, w, h);
    openslide_read_region(osr, buf, x, y, layer, w, h);

    // write as PPM
    if (!skip_write) {
      write_as_ppm(filename, w, h, buf);
    }

    free(buf);
    g_free(filename);
  }
}

static void test_horizontal_walk(openslide_t *osr,
				 int64_t start_x,
				 int64_t y,
				 int32_t layer,
				 int64_t patch_w, int64_t patch_h,
				 int stride) {
  int64_t w, h;
  openslide_get_layer_dimensions(osr, layer, &w, &h);
  int64_t d = MIN(w,h);

  uint32_t *buf = (uint32_t *) malloc(patch_w * patch_h * 4);

  for (int64_t x = start_x; x < d; x += stride) {
    openslide_read_region(osr, buf, x, y, layer, patch_w, patch_h);
    printf("%" G_GINT64_FORMAT "\r", x);
    fflush(stdout);
  }

  free(buf);
}

static void test_vertical_walk(openslide_t *osr,
			       int64_t x,
			       int64_t start_y,
			       int32_t layer,
			       int64_t patch_w, int64_t patch_h,
			       int stride) {
  int64_t w, h;
  openslide_get_layer_dimensions(osr, layer, &w, &h);
  int64_t d = MIN(w,h);

  uint32_t *buf = (uint32_t *) malloc(patch_w * patch_h * 4);

  for (int64_t y = start_y; y < d; y += stride) {
    openslide_read_region(osr, buf, x, y, layer, patch_w, patch_h);
    printf("%" G_GINT64_FORMAT "\r", y);
    fflush(stdout);
  }

  free(buf);
}

static void dump_as_tiles(openslide_t *osr, const char *name,
			  int64_t tile_w, int64_t tile_h) {
  int64_t w, h;
  openslide_get_layer0_dimensions(osr, &w, &h);

  uint32_t *buf = (uint32_t *) malloc(tile_w * tile_h * 4);

  for (int64_t y = 0; y < h; y += tile_h) {
    for (int64_t x = 0; x < w; x += tile_w) {
      char *filename;
      filename = g_strdup_printf("%s-%.10" G_GINT64_FORMAT "-%.10" G_GINT64_FORMAT ".ppm",
				 name, x, y);

      printf("%s\n", filename);

      openslide_read_region(osr, buf, x, y, 0, tile_w, tile_h);
      write_as_ppm(filename, tile_w, tile_h, buf);
      g_free(filename);
    }
  }

  free(buf);
}

/*
static void test_pdf(openslide_t *osr, const char *filename) {
  printf("test_pdf: %s\n", filename);
  cairo_surface_t *pdf = cairo_pdf_surface_create(filename, 0, 0);
  cairo_t *cr = cairo_create(pdf);
  cairo_rotate(cr, M_PI_4);

  for (int i = 0; i < openslide_get_layer_count(osr); i++) {
    int64_t orig_w, orig_h;
    openslide_get_layer_dimensions(osr, i, &orig_w, &orig_h);
    int64_t w = MIN(orig_w, 2000);
    int64_t h = MIN(orig_h, 2000);

    printf(" layer %d (%" G_GINT64_FORMAT "x%" G_GINT64_FORMAT ").",
	   i, w, h);
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
  if (argc != 2) {
    printf("give file!\n");
    return 1;
  }

  //  struct timeval start_tv;
  //  struct timeval end_tv;

  printf("openslide_can_open returns %s\n", openslide_can_open(argv[1]) ? "true" : "false");
  openslide_t *osr = openslide_open(argv[1]);

  int64_t w, h;

  if (osr == NULL) {
    printf("oh no\n");
    exit(1);
  }

  openslide_close(osr);

  osr = openslide_open(argv[1]);

  if (osr == NULL) {
    printf("oh no\n");
    exit(1);
  }

  openslide_get_layer0_dimensions(osr, &w, &h);
  printf("dimensions: %" G_GINT64_FORMAT " x %" G_GINT64_FORMAT "\n", w, h);
  printf("comment: %s\n", openslide_get_comment(osr));

  int32_t layers = openslide_get_layer_count(osr);
  printf("num layers: %d\n", layers);

  for (int32_t i = -1; i < layers + 1; i++) {
    int64_t ww, hh;
    openslide_get_layer_dimensions(osr, i, &ww, &hh);
    printf(" layer %d dimensions: %" G_GINT64_FORMAT " x %" G_GINT64_FORMAT "\n", i, ww, hh);
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
  uint32_t item[0];
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

    printf("associated image: %s -> (%" G_GINT64_FORMAT "x%" G_GINT64_FORMAT ")\n", name, w, h);

    associated_image_names++;
  }

  CALLGRIND_START_INSTRUMENTATION
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

  //  dump_as_tiles(osr, "file1", 512, 512);

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

  //  test_pdf(osr, "test0.pdf");

  CALLGRIND_STOP_INSTRUMENTATION

  openslide_close(osr);

  return 0;
}
