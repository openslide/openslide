/* Test program to make a single openslide_read_region() call and write the
   result as a PPM. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <openslide.h>

int main(int argc, char **argv) {
  if (argc != 7) {
    printf("Arguments: slide out.ppm x y w h\n");
    return 1;
  }

  char *slide = argv[1];
  char *out = argv[2];
  int64_t x = atoi(argv[3]);
  int64_t y = atoi(argv[4]);
  int64_t w = atoi(argv[5]);
  int64_t h = atoi(argv[6]);

  uint32_t *buf = malloc(w * h * 4);
  openslide_t *osr = openslide_open(slide);
  assert(osr != NULL && openslide_get_error(osr) == NULL);
  openslide_read_region(osr, buf, x, y, 0, w, h);
  assert(openslide_get_error(osr) == NULL);
  openslide_close(osr);

  FILE *fp;
  fp = fopen(out, "w");
  assert(fp != NULL);
  fprintf(fp, "P6\n# Extraneous comment\n%"PRIu64" %"PRIu64" 255\n", w, h);
  for (y = 0; y < h; y++) {
    for (x = 0; x < w; x++) {
      uint32_t sample = buf[y * h + x];
      // undo premultiplication, more or less
      double alpha = ((sample >> 24) ?: 1) / 255.0;
      uint8_t pixel[3] = {
        ((sample >> 16) & 0xff) / alpha,
        ((sample >> 8) & 0xff) / alpha,
        (sample & 0xff) / alpha,
      };
      fwrite(pixel, 1, sizeof(pixel), fp);
    }
  }
  fclose(fp);
  free(buf);
}
