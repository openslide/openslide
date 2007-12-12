#include "wholeslide.h"

int main(int argc, char **argv) {
  wholeslide_t *wsd = ws_open(argv[1]);

  uint32_t w, h;

  ws_get_baseline_dimensions(wsd, &w, &h);
  printf("dimensions: %d x %d\n", w, h);
  printf("comment: %s\n", ws_get_comment(wsd));

  uint32_t layers = ws_get_layer_count(wsd);
  printf("num layers: %d\n", layers);

  ws_close(wsd);
}
