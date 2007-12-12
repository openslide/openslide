#include "wholeslide.h"

static void test_next_biggest(wholeslide_t *wsd, double downsample) {
  uint32_t layer = ws_get_best_layer_for_downsample(wsd, downsample);
  printf("layer for downsample %g: %d (%g)\n",
	 downsample, layer, ws_get_layer_downsample(wsd, layer));
}

int main(int argc, char **argv) {
  if (argc != 2) {
    printf("give file!\n");
    return 1;
  }

  wholeslide_t *wsd = ws_open(argv[1]);

  uint32_t w, h;

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


  ws_close(wsd);

  return 0;
}
