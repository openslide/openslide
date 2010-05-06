/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "openslide-private.h"

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#include <glib.h>

#include "openslide-cache.h"
#include "openslide-tilehelper.h"

static const _openslide_vendor_fn non_tiff_formats[] = {
  _openslide_try_mirax,
  _openslide_try_hamamatsu,
  NULL
};

static const _openslide_tiff_vendor_fn tiff_formats[] = {
  _openslide_try_trestle,
  _openslide_try_aperio,
  _openslide_try_generic_tiff,
  NULL
};

static bool openslide_was_dynamically_loaded;

#ifdef _MSC_VER
  #pragma section(".CRT$XCU",read)
  #define INITIALIZER(f) \
  __declspec(allocate(".CRT$XCU")) void (DSO_DECL_SPEC*f##_)(void) = f; \
  static void __cdecl f(void)
#elif defined(__GNUC__)
  #define INITIALIZER(f) \
  static void __attribute__((constructor)) f(void)
#endif

// called from shared-library constructor!
INITIALIZER(_openslide_init) {
  // activate threads
  if (!g_thread_supported ()) g_thread_init (NULL);
  openslide_was_dynamically_loaded = true;
}

static void destroy_associated_image(gpointer data) {
  struct _openslide_associated_image *img = (struct _openslide_associated_image *) data;

  g_free(img->argb_data);
  g_slice_free(struct _openslide_associated_image, img);
}

// TODO: update when we switch to BigTIFF, or remove entirely
// if libtiff gets private error/warning callbacks
static bool quick_tiff_check(const char *filename) {
  FILE *f = fopen(filename, "rb");
  if (f == NULL) {
    return false;
  }

  // read magic
  uint8_t buf[4];
  if (fread(buf, 4, 1, f) != 1) {
    // can't read
    fclose(f);
    return false;
  }

  fclose(f);

  // check magic
  if (buf[0] != buf[1]) {
    return false;
  }

  switch (buf[0]) {
  case 'M':
    // big endian
    return (buf[2] == 0) && (buf[3] == 42);

  case 'I':
    // little endian
    return (buf[3] == 0) && (buf[2] == 42);

  default:
    return false;
  }
}

static void reset_osr(openslide_t *osr) {
  if (osr) {
    g_hash_table_remove_all(osr->properties);
    g_hash_table_remove_all(osr->associated_images);
  }
}

static void init_quickhash1_out(struct _openslide_hash **quickhash1_OUT) {
  if (quickhash1_OUT) {
    *quickhash1_OUT = _openslide_hash_quickhash1_create();
  }
}

static void free_quickhash1_if_failed(bool result,
				      struct _openslide_hash **quickhash1_OUT) {
  // if we have a hash and a false result, destroy
  if (quickhash1_OUT && !result) {
    _openslide_hash_destroy(*quickhash1_OUT);
  }
}

static bool try_format(openslide_t *osr, const char *filename,
		       struct _openslide_hash **quickhash1_OUT,
		       const _openslide_vendor_fn *format_check) {
  reset_osr(osr);
  init_quickhash1_out(quickhash1_OUT);

  bool result = (*format_check)(osr, filename, quickhash1_OUT ? *quickhash1_OUT : NULL);

  free_quickhash1_if_failed(result, quickhash1_OUT);

  return result;
}

static bool try_tiff_format(openslide_t *osr, TIFF *tiff,
			    struct _openslide_hash **quickhash1_OUT,
			    const _openslide_tiff_vendor_fn *format_check) {
  reset_osr(osr);
  init_quickhash1_out(quickhash1_OUT);

  TIFFSetDirectory(tiff, 0);
  bool result = (*format_check)(osr, tiff, quickhash1_OUT ? *quickhash1_OUT : NULL);

  free_quickhash1_if_failed(result, quickhash1_OUT);

  return result;
}

static bool try_all_formats(openslide_t *osr, const char *filename,
			    struct _openslide_hash **quickhash1_OUT) {
  // non-tiff
  const _openslide_vendor_fn *fn = non_tiff_formats;
  while (*fn) {
    if (try_format(osr, filename, quickhash1_OUT, fn)) {
      return true;
    }
    fn++;
  }


  // tiff
  TIFF *tiff;
  if (quick_tiff_check(filename) && ((tiff = TIFFOpen(filename, "r")) != NULL)) {
    const _openslide_tiff_vendor_fn *tfn = tiff_formats;
    while (*tfn) {
      if (try_tiff_format(osr, tiff, quickhash1_OUT, tfn)) {
	return true;
      }
      tfn++;
    }

    // close only if failed
    TIFFClose(tiff);
  }


  // no match
  return false;
}

bool openslide_can_open(const char *filename) {
  g_assert(openslide_was_dynamically_loaded);

  // quick test
  return try_all_formats(NULL, filename, NULL);
}


struct add_key_to_strv_data {
  int i;
  const char **strv;
};

static void add_key_to_strv(gpointer key,
			    gpointer _OPENSLIDE_UNUSED(value),
			    gpointer user_data) {
  struct add_key_to_strv_data *d = (struct add_key_to_strv_data *) user_data;

  d->strv[d->i++] = (const char *) key;
}

static const char **strv_from_hashtable_keys(GHashTable *h) {
  const char **result = g_new0(const char *, g_hash_table_size(h) + 1);

  struct add_key_to_strv_data data = { 0, result };
  g_hash_table_foreach(h, add_key_to_strv, &data);

  return result;
}

openslide_t *openslide_open(const char *filename) {
  g_assert(openslide_was_dynamically_loaded);

  // alloc memory
  openslide_t *osr = g_slice_new0(openslide_t);
  osr->properties = g_hash_table_new_full(g_str_hash, g_str_equal,
					  g_free, g_free);
  osr->associated_images = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, destroy_associated_image);

  // try to read it
  struct _openslide_hash *quickhash1 = NULL;
  if (!try_all_formats(osr, filename, &quickhash1)) {
    // failure
    openslide_close(osr);
    return NULL;
  }

  // compute downsamples if not done already
  int64_t blw, blh;
  openslide_get_layer0_dimensions(osr, &blw, &blh);

  if (!osr->downsamples) {
    osr->downsamples = g_new(double, osr->layer_count);
    osr->downsamples[0] = 1.0;
    for (int32_t i = 0; i < osr->layer_count; i++) {
      int64_t w, h;
      openslide_get_layer_dimensions(osr, i, &w, &h);

      if (i > 0) {
	osr->downsamples[i] =
	  (((double) blh / (double) h) +
	   ((double) blw / (double) w)) / 2.0;
      }
    }
  }

  // check downsamples
  for (int32_t i = 1; i < osr->layer_count; i++) {
    //g_debug("downsample: %g", osr->downsamples[i]);

    if (osr->downsamples[i] < osr->downsamples[i - 1]) {
      g_warning("Downsampled images not correctly ordered: %g < %g",
		osr->downsamples[i], osr->downsamples[i - 1]);
      openslide_close(osr);
      _openslide_hash_destroy(quickhash1);
      return NULL;
    }
  }

  // set hash property
  if (quickhash1 != NULL) {
    g_hash_table_insert(osr->properties,
			g_strdup(OPENSLIDE_PROPERTY_NAME_QUICKHASH1),
			g_strdup(_openslide_hash_get_string(quickhash1)));
    _openslide_hash_destroy(quickhash1);
  }

  // fill in names
  osr->associated_image_names = strv_from_hashtable_keys(osr->associated_images);
  osr->property_names = strv_from_hashtable_keys(osr->properties);

  // start cache
  osr->cache = _openslide_cache_create(_OPENSLIDE_USEFUL_CACHE_SIZE);
  //osr->cache = _openslide_cache_create(0);

  // validate required properties
  g_assert(openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_VENDOR));

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

  g_free(osr->downsamples);

  if (osr->cache) {
    _openslide_cache_destroy(osr->cache);
  }

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
  return openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_COMMENT);
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


int openslide_give_prefetch_hint(openslide_t *_OPENSLIDE_UNUSED(osr),
				 int64_t _OPENSLIDE_UNUSED(x),
				 int64_t _OPENSLIDE_UNUSED(y),
				 int32_t _OPENSLIDE_UNUSED(layer),
				 int64_t _OPENSLIDE_UNUSED(w),
				 int64_t _OPENSLIDE_UNUSED(h)) {
  g_warning("openslide_give_prefetch_hint has never been implemented and should not be called");
  return 0;
}

void openslide_cancel_prefetch_hint(openslide_t *_OPENSLIDE_UNUSED(osr),
				    int _OPENSLIDE_UNUSED(prefetch_id)) {
  g_warning("openslide_cancel_prefetch_hint has never been implemented and should not be called");
}

void openslide_read_region(openslide_t *osr,
			   uint32_t *dest,
			   int64_t x, int64_t y,
			   int32_t layer,
			   int64_t w, int64_t h) {
  //g_debug("openslide_read_region: %" PRId64 " %" PRId64 " %d %" PRId64 " %" PRId64, x, y, layer, w, h);

  if (w <= 0 || h <= 0) {
    //g_debug("%" PRId64 " %" PRId64, w, h);
    return;
  }

  // create the cairo surface for the dest
  cairo_surface_t *surface;
  if (dest) {
    surface = cairo_image_surface_create_for_data((unsigned char *) dest,
						  CAIRO_FORMAT_ARGB32,
						  w, h, w * 4);
  } else {
    // nil surface
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 0, 0);
  }


  // create the cairo context
  cairo_t *cr = cairo_create(surface);
  cairo_surface_destroy(surface);

  // clear
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);

  // saturate those seams away!
  cairo_set_operator(cr, CAIRO_OPERATOR_SATURATE);

  // check constraints
  if ((layer < osr->layer_count) && (layer >= 0) && (x >= 0) && (y >= 0)) {
    // don't bother checking to see if (x/ds) and (y/ds) are within
    // the bounds of the layer, we will just draw nothing below

    // now fully within all important bounds, go for it

    // paint
    (osr->ops->paint_region)(osr, cr, x, y, layer, w, h);
  }

  // fill with background
  cairo_set_source_rgb(cr,
		       osr->fill_color_r,
		       osr->fill_color_g,
		       osr->fill_color_b);
  //  cairo_set_source_rgb(cr, 1.0, 0.0, 0.0); // red
  cairo_paint(cr);

  // done
  cairo_destroy(cr);
}


const char * const *openslide_get_property_names(openslide_t *osr) {
  return osr->property_names;
}

const char *openslide_get_property_value(openslide_t *osr, const char *name) {
  return (const char *) g_hash_table_lookup(osr->properties, name);
}

const char * const *openslide_get_associated_image_names(openslide_t *osr) {
  return osr->associated_image_names;
}

void openslide_get_associated_image_dimensions(openslide_t *osr, const char *name,
					       int64_t *w, int64_t *h) {
  struct _openslide_associated_image *img =
    (struct _openslide_associated_image *) g_hash_table_lookup(osr->associated_images,
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
  struct _openslide_associated_image *img =
    (struct _openslide_associated_image *) g_hash_table_lookup(osr->associated_images,
							       name);
  if (img && dest) {
    memcpy(dest, img->argb_data, img->w * img->h * 4);
  }
}
