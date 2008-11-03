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

#include "config.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <tiffio.h>
#include <glib.h>

#include "wholeslide-private.h"

static bool try_all_formats(wholeslide_t *wsd, const char *filename) {
  return
    //_ws_try_generic_jp2k(wsd, filename) ||
    _ws_try_hamamatsu(wsd, filename) ||
    _ws_try_trestle(wsd, filename) ||
    _ws_try_aperio(wsd, filename);
}

bool ws_can_open(const char *filename) {
  // quick test
  return try_all_formats(NULL, filename);
}


wholeslide_t *ws_open(const char *filename) {
  // alloc memory
  wholeslide_t *wsd = g_slice_new0(wholeslide_t);

  // try to read it
  if (!try_all_formats(wsd, filename)) {
    // failure
    ws_close(wsd);
    return NULL;
  }

  // compute downsamples
  int64_t blw, blh;
  wsd->downsamples = g_new(double, wsd->layer_count);
  ws_get_layer0_dimensions(wsd, &blw, &blh);
  for (int32_t i = 0; i < wsd->layer_count; i++) {
    int64_t w, h;
    ws_get_layer_dimensions(wsd, i, &w, &h);

    wsd->downsamples[i] = (double) blh / (double) h;
    g_assert(wsd->downsamples[i] >= 1.0);
  }

  return wsd;
}


void ws_close(wholeslide_t *wsd) {
  if (wsd->ops) {
    (wsd->ops->destroy)(wsd);
  }

  g_free(wsd->downsamples);
  g_slice_free(wholeslide_t, wsd);
}


void ws_get_layer0_dimensions(wholeslide_t *wsd,
			      int64_t *w, int64_t *h) {
  ws_get_layer_dimensions(wsd, 0, w, h);
}

void ws_get_layer_dimensions(wholeslide_t *wsd, int32_t layer,
			     int64_t *w, int64_t *h) {
  if (layer > wsd->layer_count || layer < 0) {
    *w = 0;
    *h = 0;
  } else {
    (wsd->ops->get_dimensions)(wsd, layer, w, h);
  }
}

const char *ws_get_comment(wholeslide_t *wsd) {
  return (wsd->ops->get_comment)(wsd);
}


int32_t ws_get_layer_count(wholeslide_t *wsd) {
  return wsd->layer_count;
}


int32_t ws_get_best_layer_for_downsample(wholeslide_t *wsd,
					 double downsample) {
  // too small, return first
  if (downsample < wsd->downsamples[0]) {
    return 0;
  }

  // find where we are in the middle
  for (int32_t i = 1; i < wsd->layer_count; i++) {
    if (downsample < wsd->downsamples[i]) {
      return i - 1;
    }
  }

  // too big, return last
  return wsd->layer_count - 1;
}


double ws_get_layer_downsample(wholeslide_t *wsd, int32_t layer) {
  if (layer > wsd->layer_count || layer < 0) {
    return 0.0;
  }

  return wsd->downsamples[layer];
}


int ws_give_prefetch_hint(wholeslide_t *wsd,
			  int64_t x, int64_t y,
			  int32_t layer,
			  int64_t w, int64_t h) {
  // TODO
  return 0;
}

void ws_cancel_prefetch_hint(wholeslide_t *wsd, int prefetch_id) {
  // TODO
  return;
}

void ws_read_region(wholeslide_t *wsd,
		    uint32_t *dest,
		    int64_t x, int64_t y,
		    int32_t layer,
		    int64_t w, int64_t h) {
  // start cleared
  memset(dest, 0, w * h * 4);

  //  for (int64_t i = 0; i < w * h; i++) {
  //    dest[i] = 0xFFFF0000; // red
  //  }

  if (layer > wsd->layer_count || layer < 0) {
    return;
  }

  // translate if x or y are negative
  int64_t new_w = w;
  int64_t new_x = x;
  if (x < 0) {
    new_w += x;
    new_x = 0;
  }
  int64_t new_h = h;
  int64_t new_y = y;
  if (y < 0) {
    new_h += y;
    new_y = 0;
  }

  // is there anything left to paint?
  if (w <= 0 || h <= 0) {
    return;
  }

  uint32_t *new_dest = dest + ((new_y - y) * w + (new_x - x));
  (wsd->ops->read_region)(wsd, new_dest, new_x, new_y, layer, new_w, new_h);
}
