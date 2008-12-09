/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#include "config.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <tiffio.h>
#include <glib.h>

#include "openslide-private.h"

static bool try_all_formats(openslide_t *osr, const char *filename) {
  return
    //_openslide_try_generic_jp2k(osr, filename) ||
    _openslide_try_hamamatsu(osr, filename) ||
    _openslide_try_trestle(osr, filename) ||
    _openslide_try_aperio(osr, filename);
}

bool openslide_can_open(const char *filename) {
  // quick test
  return try_all_formats(NULL, filename);
}


openslide_t *openslide_open(const char *filename) {
  // alloc memory
  openslide_t *osr = g_slice_new0(openslide_t);

  // try to read it
  if (!try_all_formats(osr, filename)) {
    // failure
    openslide_close(osr);
    return NULL;
  }

  // compute downsamples
  int64_t blw, blh;
  osr->downsamples = g_new(double, osr->layer_count);
  openslide_get_layer0_dimensions(osr, &blw, &blh);
  for (int32_t i = 0; i < osr->layer_count; i++) {
    int64_t w, h;
    openslide_get_layer_dimensions(osr, i, &w, &h);

    osr->downsamples[i] = (double) blh / (double) h;
    g_assert(osr->downsamples[i] >= 1.0);
    if (i > 0) {
      g_assert(osr->downsamples[i] >= osr->downsamples[i - 1]);
    }
  }

  return osr;
}


void openslide_close(openslide_t *osr) {
  if (osr->ops) {
    (osr->ops->destroy)(osr);
  }

  g_free(osr->downsamples);
  g_slice_free(openslide_t, osr);
}


void openslide_get_layer0_dimensions(openslide_t *osr,
				     int64_t *w, int64_t *h) {
  openslide_get_layer_dimensions(osr, 0, w, h);
}

void openslide_get_layer_dimensions(openslide_t *osr, int32_t layer,
				    int64_t *w, int64_t *h) {
  if (layer > osr->layer_count || layer < 0) {
    *w = 0;
    *h = 0;
  } else {
    (osr->ops->get_dimensions)(osr, layer, w, h);
  }
}

const char *openslide_get_comment(openslide_t *osr) {
  return (osr->ops->get_comment)(osr);
}


int32_t openslide_get_layer_count(openslide_t *osr) {
  return osr->layer_count;
}


int32_t openslide_get_best_layer_for_downsample(openslide_t *osr,
						double downsample) {
  // too small, return first
  if (downsample < osr->downsamples[0]) {
    return 0;
  }

  // find where we are in the middle
  for (int32_t i = 1; i < osr->layer_count; i++) {
    if (downsample < osr->downsamples[i]) {
      return i - 1;
    }
  }

  // too big, return last
  return osr->layer_count - 1;
}


double openslide_get_layer_downsample(openslide_t *osr, int32_t layer) {
  if (layer > osr->layer_count || layer < 0) {
    return 0.0;
  }

  return osr->downsamples[layer];
}


int openslide_give_prefetch_hint(openslide_t *osr,
				 int64_t x, int64_t y,
				 int32_t layer,
				 int64_t w, int64_t h) {
  // TODO
  return 0;
}

void openslide_cancel_prefetch_hint(openslide_t *osr, int prefetch_id) {
  // TODO
  return;
}

void openslide_read_region(openslide_t *osr,
			   uint32_t *dest,
			   int64_t x, int64_t y,
			   int32_t layer,
			   int64_t w, int64_t h) {
  if (w <= 0 || h <= 0) {
    return;
  }

  // start cleared
  memset(dest, 0, w * h * 4);

  //  for (int64_t i = 0; i < w * h; i++) {
  //    dest[i] = 0xFFFF0000; // red
  //  }

  // check constraints
  if (layer > osr->layer_count || layer < 0 || x < 0 || y < 0) {
    return;
  }

  // we could also check to make sure that (x / ds) + w and (y / ds) + h
  // doesn't exceed the bounds of the image, but this situation is
  // less harmful and can be cleanly handled by the ops backends anyway,
  // so we allow it
  //
  // also, we don't want to introduce rounding errors here with our
  // double representation of downsampling -- backends have more precise
  // ways of representing this


  // now fully within all bounds, go for it
  (osr->ops->read_region)(osr, dest, x, y, layer, w, h);
}
