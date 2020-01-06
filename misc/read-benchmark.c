/* Read slide level 0 in 1000 x 1000 regions and report time in pixels
   per second. */
/* gcc -O2 -g -std=gnu99 -o read-benchmark read-benchmark.c \
   $(pkg-config --cflags --libs openslide) */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <openslide.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define REGION_WIDTH 1000
#define REGION_HEIGHT 1000
#define RUNS 5

int main(int argc, char **argv) {
  if (argc != 2) {
    printf("Arguments: slide\n");
    return 1;
  }
  const char *slide = argv[1];

  uint32_t *buf = malloc(REGION_WIDTH * REGION_HEIGHT * 4);
  openslide_t *osr = openslide_open(slide);
  assert(osr != NULL && openslide_get_error(osr) == NULL);
  int64_t w, h;
  openslide_get_level0_dimensions(osr, &w, &h);

  struct timespec start, end;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
  for (int64_t i = 0; i < RUNS; i++) {
    for (int64_t y = 0; y < h; y += REGION_HEIGHT) {
      for (int64_t x = 0; x < w; x += REGION_WIDTH) {
        openslide_read_region(osr, buf, x, y, 0,
                              MIN(w - x, REGION_WIDTH),
                              MIN(h - y, REGION_HEIGHT));
      }
    }
  }
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
  double elapsed = (end.tv_sec + end.tv_nsec / 1e9) -
                   (start.tv_sec + start.tv_nsec / 1e9);
  assert(openslide_get_error(osr) == NULL);
  printf("%.1f million pixels per CPU-second\n",
         w * h * RUNS / (elapsed * 1e6));

  openslide_close(osr);
  free(buf);
  return 0;
}
