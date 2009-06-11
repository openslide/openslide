/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
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
#include <inttypes.h>

#include <glib.h>

#include "openslide-private.h"

typedef bool (*vendor_fn)(openslide_t *osr, const char *filename);

static const vendor_fn all_formats[] = {
  _openslide_try_mirax,
  _openslide_try_hamamatsu,
  _openslide_try_trestle,
  _openslide_try_aperio,
  NULL
};

static void destroy_associated_image(gpointer data) {
  struct _openslide_associated_image *img = data;

  g_free(img->argb_data);
  g_slice_free(struct _openslide_associated_image, img);
}

static bool try_all_formats(openslide_t *osr, const char *filename) {
  const vendor_fn *fn = all_formats;
  while (*fn) {
    if (osr) {
      g_hash_table_remove_all(osr->properties);
      g_hash_table_remove_all(osr->associated_images);
    }

    if ((*fn)(osr, filename)) {
      return true;
    }
    fn++;
  }
  return false;
}

bool openslide_can_open(const char *filename) {
  // quick test
  return try_all_formats(NULL, filename);
}


struct add_key_to_strv_data {
  int i;
  const char **strv;
};

static void add_key_to_strv(gpointer key,
			    gpointer value,
			    gpointer user_data) {
  struct add_key_to_strv_data *d = user_data;

  d->strv[d->i++] = key;
}

static const char **strv_from_hashtable_keys(GHashTable *h) {
  const char **result = g_new0(const char *, g_hash_table_size(h) + 1);

  struct add_key_to_strv_data data = { 0, result };
  g_hash_table_foreach(h, add_key_to_strv, &data);

  return result;
}

openslide_t *openslide_open(const char *filename) {
  // we are threading
  if (!g_thread_supported ()) g_thread_init (NULL);

  // alloc memory
  openslide_t *osr = g_slice_new0(openslide_t);
  osr->properties = g_hash_table_new_full(g_str_hash, g_str_equal,
					  g_free, g_free);
  osr->associated_images = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, destroy_associated_image);

  // try to read it
  if (!try_all_formats(osr, filename)) {
    // failure
    openslide_close(osr);
    return NULL;
  }

  // compute downsamples
  int64_t blw, blh;
  osr->downsamples = g_new(double, osr->layer_count);
  osr->downsamples[0] = 1.0;
  openslide_get_layer0_dimensions(osr, &blw, &blh);
  for (int32_t i = 0; i < osr->layer_count; i++) {
    int64_t w, h;
    openslide_get_layer_dimensions(osr, i, &w, &h);

    if (i > 0) {
      osr->downsamples[i] =
	(((double) blh / (double) h) +
	 ((double) blw / (double) w)) / 2.0;

      g_debug("downsample: %g", osr->downsamples[i]);

      if (osr->downsamples[i] < osr->downsamples[i - 1]) {
	g_warning("Downsampled images not correctly ordered: %g < %g",
		  osr->downsamples[i], osr->downsamples[i - 1]);
	openslide_close(osr);
	return NULL;
      }
    }
  }

  // fill in names
  osr->associated_image_names = strv_from_hashtable_keys(osr->associated_images);
  osr->property_names = strv_from_hashtable_keys(osr->properties);

  g_debug("overlaps");
  for (int32_t i = 0; i < osr->layer_count; i++) {
    int32_t ox;
    int32_t oy;
    _openslide_get_overlaps(osr, i, &ox, &oy);
    g_debug(" %d %d (layer %d)", ox, oy, i);
  }

  return osr;
}


void openslide_close(openslide_t *osr) {
  if (osr->ops) {
    (osr->ops->destroy)(osr);
  }

  g_hash_table_unref(osr->associated_images);
  g_hash_table_unref(osr->properties);

  g_free(osr->associated_image_names);
  g_free(osr->property_names);

  g_free(osr->overlaps);

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
    int64_t image_w, image_h, tile_w, tile_h;
    (osr->ops->get_dimensions)(osr, layer,
			       &image_w, &image_h,
			       &tile_w, &tile_h);
    if (image_w == 0 || image_h == 0) {
      // done
      *w = 0;
      *h = 0;
      return;
    }

    // subtract overlaps and compute
    int32_t overlap_x, overlap_y;
    _openslide_get_overlaps(osr, layer, &overlap_x, &overlap_y);

    if (overlap_x && (tile_w <= image_w)) {
      int64_t overlaps_across = image_w / tile_w;
      *w = (overlaps_across * tile_w) - overlap_x * (overlaps_across - 1);
    } else {
      *w = image_w;
    }

    if (overlap_y && (tile_h <= image_h)) {
      int64_t overlaps_down = image_h / tile_h;
      *h = (overlaps_down * tile_h) - overlap_y * (overlaps_down - 1);
    } else {
      *h = image_h;
    }
    g_debug("layer %d overlap spacing: %d %d", layer, tile_w, tile_h);
  }

  g_debug("layer %d dimensions: %" PRId64 " %" PRId64, layer, *w, *h);
}

const char *openslide_get_comment(openslide_t *osr) {
  return openslide_get_property_value(osr, _OPENSLIDE_COMMENT_NAME);
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
  if (dest != NULL) {
    for (int64_t i = 0; i < w * h; i++) {
      dest[i] = osr->fill_color_argb;
    }
  }

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


const char * const *openslide_get_property_names(openslide_t *osr) {
  return osr->property_names;
}

const char *openslide_get_property_value(openslide_t *osr, const char *name) {
  return g_hash_table_lookup(osr->properties, name);
}

const char * const *openslide_get_associated_image_names(openslide_t *osr) {
  return osr->associated_image_names;
}

void openslide_get_associated_image_dimensions(openslide_t *osr, const char *name,
					       int64_t *w, int64_t *h) {
  struct _openslide_associated_image *img = g_hash_table_lookup(osr->associated_images,
								name);
  if (img) {
    *w = img->w;
    *h = img->h;
  } else {
    *w = 0;
    *h = 0;
  }
}

void openslide_read_associated_image(openslide_t *osr,
				     const char *name,
				     uint32_t *dest) {
  struct _openslide_associated_image *img = g_hash_table_lookup(osr->associated_images,
								name);
  if (img && dest) {
    memcpy(dest, img->argb_data, img->w * img->h * 4);
  }
}


void _openslide_get_overlaps(openslide_t *osr, int32_t layer,
			     int32_t *x, int32_t *y) {
  if (layer >= osr->overlap_count) {
    *x = 0;
    *y = 0;
  } else {
    *x = osr->overlaps[layer * 2];
    *y = osr->overlaps[(layer * 2) + 1];
  }
}

void _openslide_add_in_overlaps(openslide_t *osr,
				int32_t layer,
				int64_t tile_w,
				int64_t tile_h,
				int64_t total_overlaps_across,
				int64_t total_overlaps_down,
				int64_t x, int64_t y,
				int64_t *out_x, int64_t *out_y) {
  int32_t ox, oy;
  _openslide_get_overlaps(osr, layer, &ox, &oy);

  // the last tile doesn't have an overlap to skip
  int64_t max_skip_x = (total_overlaps_across - 1) * ox;
  int64_t max_skip_y = (total_overlaps_down - 1) * oy;

  int64_t skip_x = (x / (tile_w - ox)) * ox;
  int64_t skip_y = (y / (tile_h - oy)) * oy;

  *out_x = x + MIN(max_skip_x, skip_x);
  *out_y = y + MIN(max_skip_y, skip_y);
}
