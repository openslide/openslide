#include "config.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <tiffio.h>
#include <glib.h>

#include "wholeslide-private.h"

wholeslide_t *ws_open(const char *filename) {
  // alloc memory
  wholeslide_t *wsd = g_slice_new0(wholeslide_t);

  // try to read it
  if (!(_ws_try_trestle(wsd, filename) ||
	_ws_try_hamamatsu(wsd, filename))) {
    // failure
    ws_close(wsd);
    return NULL;
  }

  // compute downsamples
  uint32_t blw, blh;
  wsd->downsamples = g_new(double, wsd->layer_count);
  ws_get_baseline_dimensions(wsd, &blw, &blh);
  for (uint32_t i = 0; i < wsd->layer_count; i++) {
    uint32_t w, h;
    ws_get_layer_dimensions(wsd, i, &w, &h);

    wsd->downsamples[i] = (double) blh / (double) h;
  }

  return wsd;
}


void ws_close(wholeslide_t *wsd) {
  if (wsd->ops) {
    (wsd->ops->destroy)(wsd);
  }

  g_free(wsd->layers);
  g_free(wsd->downsamples);
  g_slice_free(wholeslide_t, wsd);
}


void ws_get_baseline_dimensions(wholeslide_t *wsd,
				uint32_t *w, uint32_t *h) {
  ws_get_layer_dimensions(wsd, 0, w, h);
}

void ws_get_layer_dimensions(wholeslide_t *wsd, uint32_t layer,
			     uint32_t *w, uint32_t *h) {
  (wsd->ops->get_dimensions)(wsd, layer, w, h);
}

const char *ws_get_comment(wholeslide_t *wsd) {
  return (wsd->ops->get_comment)(wsd);
}


uint32_t ws_get_layer_count(wholeslide_t *wsd) {
  return wsd->layer_count;
}


uint32_t ws_get_best_layer_for_downsample(wholeslide_t *wsd,
					  double downsample) {
  // too small, return first
  if (downsample < wsd->downsamples[0]) {
    return 0;
  }

  // find where we are in the middle
  for (uint32_t i = 1; i < wsd->layer_count; i++) {
    if (downsample < wsd->downsamples[i]) {
      return i - 1;
    }
  }

  // too big, return last
  return wsd->layer_count - 1;
}


double ws_get_layer_downsample(wholeslide_t *wsd, uint32_t layer) {
  if (layer > wsd->layer_count) {
    return 0.0;
  }

  return wsd->downsamples[layer];
}


uint32_t ws_give_prefetch_hint(wholeslide_t *wsd,
			       uint32_t x, uint32_t y,
			       uint32_t layer,
			       uint32_t w, uint32_t h) {
  // TODO
  return 0;
}

void ws_cancel_prefetch_hint(wholeslide_t *wsd, uint32_t prefetch_id) {
  // TODO
  return;
}


size_t ws_get_region_num_bytes(wholeslide_t *wsd,
			       uint32_t w, uint32_t h) {
  return w * h * 4;
}


void ws_read_region(wholeslide_t *wsd,
		    uint32_t *dest,
		    uint32_t x, uint32_t y,
		    uint32_t layer,
		    uint32_t w, uint32_t h) {
  (wsd->ops->read_region)(wsd, dest, x, y, layer, w, h);
}
