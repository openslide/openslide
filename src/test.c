#define _GNU_SOURCE

#include "wholeslide.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

static void test_next_biggest(wholeslide_t *wsd, double downsample) {
  uint32_t layer = ws_get_best_layer_for_downsample(wsd, downsample);
  printf("layer for downsample %g: %d (%g)\n",
	 downsample, layer, ws_get_layer_downsample(wsd, layer));
}

static void test_image_fetch(wholeslide_t *wsd,
			     const char *name,
			     uint32_t x, uint32_t y,
			     uint32_t w, uint32_t h,
			     bool skip_write) {
  char *filename;

  //  for (uint32_t layer = 0; layer < 1; layer++) {
  for (uint32_t layer = 0; layer < ws_get_layer_count(wsd); layer++) {
    asprintf(&filename, "%s-%.2d.ppm", name, layer);
    FILE *f = fopen(filename, "w");
    if (f == NULL) {
      perror("Cannot open file");
      goto end;
    }

    size_t num_bytes = ws_get_region_num_bytes(wsd, w, h);
    printf("Going to allocate %d bytes...\n", num_bytes);
    uint32_t *buf = malloc(num_bytes);

    ws_read_region(wsd, buf, x, y, layer, w, h);

    // write as PPM
    if (!skip_write) {
      fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (int i = 0; i < num_bytes / 4; i++) {
	uint32_t val = buf[i];
	putc((val >> 16) & 0xFF, f); // R
	putc((val >> 8) & 0xFF, f);  // G
	putc((val >> 0) & 0xFF, f);  // B
	// no A
      }
    }

    free(buf);
    fclose(f);
  end:
    free(filename);
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    printf("give file!\n");
    return 1;
  }

  wholeslide_t *wsd = ws_open(argv[1]);

  uint32_t w, h;

  if (wsd == NULL) {
    printf("oh no\n");
    exit(1);
  }

  ws_get_baseline_dimensions(wsd, &w, &h);
  printf("dimensions: %d x %d\n", w, h);
  printf("comment: %s\n", ws_get_comment(wsd));

  uint32_t layers = ws_get_layer_count(wsd);
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

  uint32_t prefetch_hint = ws_give_prefetch_hint(wsd, 0, 0, 0, 5, 5);
  ws_cancel_prefetch_hint(wsd, prefetch_hint);


  test_image_fetch(wsd, "test1", w/2, h/2, 1024, 1024, true);
  test_image_fetch(wsd, "test1", w/2, h/2, 1024, 1024, true);
  test_image_fetch(wsd, "test1", w/2, h/2, 1024, 1024, true);
  test_image_fetch(wsd, "test1", w/2, h/2, 1024, 1024, true);
  test_image_fetch(wsd, "test1", w/2, h/2, 1024, 1024, true);

  //  test_image_fetch(wsd, "test2", 0, 0, 400, 300);
  //  test_image_fetch(wsd, "test3", 12, 11, 400, 300);

  ws_close(wsd);

  return 0;
}
