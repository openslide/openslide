/*
 *  Wholeslide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  Wholeslide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  Wholeslide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Wholeslide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking Wholeslide statically or dynamically with other modules is
 *  making a combined work based on Wholeslide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#define _GNU_SOURCE

#include "wholeslide.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/time.h>
#include <inttypes.h>

static void test_next_biggest(wholeslide_t *wsd, double downsample) {
  int32_t layer = ws_get_best_layer_for_downsample(wsd, downsample);
  printf("layer for downsample %g: %d (%g)\n",
	 downsample, layer, ws_get_layer_downsample(wsd, layer));
}

static void test_tile_walk(wholeslide_t *wsd,
			   int64_t tile_size) {
  printf("test_tile_walk: %" PRId64 "\n", tile_size);

  struct timeval tv, tv2;
  uint32_t *buf = malloc(tile_size * tile_size * 4);

  int64_t w, h;
  ws_get_layer0_dimensions(wsd, &w, &h);

  for (int64_t y = 0; y < h; y += tile_size) {
    for (int64_t x = 0; x < w; x += tile_size) {
      gettimeofday(&tv, NULL);
      ws_read_region(wsd, buf, x, y, 0, tile_size, tile_size);
      gettimeofday(&tv2, NULL);
      //      printf("time: %d\n", (tv2.tv_sec - tv.tv_sec) * 1000 + (tv2.tv_usec - tv.tv_usec) / 1000);
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

  fprintf(f, "P6\n%" PRId64 " %" PRId64 "\n255\n", w, h);
  for (int64_t i = 0; i < w * h; i++) {
    uint32_t val = buf[i];
    putc((val >> 16) & 0xFF, f); // R
    putc((val >> 8) & 0xFF, f);  // G
    putc((val >> 0) & 0xFF, f);  // B
    // no A
  }

  fclose(f);
}

static void test_image_fetch(wholeslide_t *wsd,
			     const char *name,
			     int64_t x, int64_t y,
			     int64_t w, int64_t h,
			     bool skip_write) {
  char *filename;

  printf("test image fetch %s\n", name);
  //  for (int32_t layer = 0; layer < 1; layer++) {
  for (int32_t layer = 0; layer < ws_get_layer_count(wsd); layer++) {
    asprintf(&filename, "%s-%.2d.ppm", name, layer);
    int64_t num_bytes = w * h * 4;
    printf("Going to allocate %" PRId64 " bytes...\n", num_bytes);
    uint32_t *buf = malloc(num_bytes);

    printf("x: %" PRId64 ", y: %" PRId64 ", layer: %d, w: %" PRId64 ", h: %" PRId64 "\n", x, y, layer, w, h);
    ws_read_region(wsd, buf, x, y, layer, w, h);

    // write as PPM
    if (!skip_write) {
      write_as_ppm(filename, w, h, buf);
    }

    free(buf);
    free(filename);
  }
}

static void dump_as_tiles(wholeslide_t *wsd, const char *name,
			  int64_t tile_w, int64_t tile_h) {
  int64_t w, h;
  ws_get_layer0_dimensions(wsd, &w, &h);

  uint32_t *buf = malloc(tile_w * tile_h * 4);

  for (int64_t y = 0; y < h; y += tile_h) {
    for (int64_t x = 0; x < w; x += tile_w) {
      char *filename;
      asprintf(&filename, "%s-%.10" PRId64 "-%.10" PRId64 ".ppm",
	       name, x, y);

      printf("%s\n", filename);

      ws_read_region(wsd, buf, x, y, 0, tile_w, tile_h);
      write_as_ppm(filename, tile_w, tile_h, buf);
      free(filename);
    }
  }

  free(buf);
}


int main(int argc, char **argv) {
  if (argc != 2) {
    printf("give file!\n");
    return 1;
  }

  printf("ws_can_open returns %s\n", ws_can_open(argv[1]) ? "true" : "false");
  wholeslide_t *wsd = ws_open(argv[1]);

  int64_t w, h;

  if (wsd == NULL) {
    printf("oh no\n");
    exit(1);
  }

  ws_get_layer0_dimensions(wsd, &w, &h);
  printf("dimensions: %" PRId64 " x %" PRId64 "\n", w, h);
  printf("comment: %s\n", ws_get_comment(wsd));

  int32_t layers = ws_get_layer_count(wsd);
  printf("num layers: %d\n", layers);

  test_next_biggest(wsd, 0.8);
  test_next_biggest(wsd, 1.0);
  test_next_biggest(wsd, 1.5);
  test_next_biggest(wsd, 2.0);
  test_next_biggest(wsd, 3.0);
  test_next_biggest(wsd, 3.1);
  test_next_biggest(wsd, 10);
  test_next_biggest(wsd, 20);
  test_next_biggest(wsd, 25);
  test_next_biggest(wsd, 100);
  test_next_biggest(wsd, 1000);
  test_next_biggest(wsd, 10000);

  int prefetch_hint = ws_give_prefetch_hint(wsd, 0, 0, 0, 5, 5);
  ws_cancel_prefetch_hint(wsd, prefetch_hint);

  //  dump_as_tiles(wsd, "file1", 512, 512);

  //  return 0;

  bool skip = true;

  //  test_tile_walk(wsd, 16);
  //test_tile_walk(wsd, 4096);
  //test_tile_walk(wsd, 256);

  test_image_fetch(wsd, "test0", 61000, 61000, 1024, 1024, skip);
  test_image_fetch(wsd, "test1", w/2, h/2, 1024, 1024, skip);
  test_image_fetch(wsd, "test2", w - 500, h - 300, 900, 800, skip);
  test_image_fetch(wsd, "test3", w*2, h*2, 900, 800, skip);
  test_image_fetch(wsd, "test4", 10, 10, 1900, 800, skip);
  test_image_fetch(wsd, "test5", w - 20, 0, 40, 100, skip);
  test_image_fetch(wsd, "test6", 0, h - 20, 100, 40, skip);

  ws_close(wsd);

  return 0;
}
