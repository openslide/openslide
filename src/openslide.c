/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
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

#include <glib.h>

#include "openslide-cache.h"
#include "openslide-tilehelper.h"
#include "openslide-cairo.h"

static const char * const EMPTY_STRING_ARRAY[] = { NULL };

static const _openslide_vendor_fn non_tiff_formats[] = {
  _openslide_try_mirax,
  _openslide_try_hamamatsu,
  //_openslide_try_hamamatsu_ndpi, // it is a tiff format? but not to libtiff
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
  static void __cdecl f(void); \
  __declspec(allocate(".CRT$XCU")) void (__cdecl*f##_)(void) = f; \
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

  if (img->destroy_ctx != NULL && img->ctx != NULL) {
    img->destroy_ctx(img->ctx);
  }
  g_slice_free(struct _openslide_associated_image, img);
}

static bool level_in_range(openslide_t *osr, int32_t level) {
  if (level < 0) {
    return false;
  }

  if (level > osr->level_count - 1) {
    return false;
  }

  return true;
}

// TODO: remove entirely if libtiff gets private error/warning callbacks
static bool quick_tiff_check(const char *filename) {
  FILE *f = _openslide_fopen(filename, "rb");
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
    return (buf[2] == 0) && ((buf[3] == 42) || (buf[3] == 43));

  case 'I':
    // little endian
    return (buf[3] == 0) && ((buf[2] == 42) || (buf[2] == 43));

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
  // TIFFOpen: m disables mmap to avoid sigbus and other mmap fragility
  if (quick_tiff_check(filename) && ((tiff = TIFFOpen(filename, "rm")) != NULL)) {
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
			    gpointer value G_GNUC_UNUSED,
			    gpointer user_data) {
  struct add_key_to_strv_data *d = (struct add_key_to_strv_data *) user_data;

  d->strv[d->i++] = (const char *) key;
}

static int cmpstring(const void *p1, const void *p2) {
  return strcmp(* (char * const *) p1, * (char * const *) p2);
}

static const char **strv_from_hashtable_keys(GHashTable *h) {
  int size = g_hash_table_size(h);
  const char **result = g_new0(const char *, size + 1);

  struct add_key_to_strv_data data = { 0, result };
  g_hash_table_foreach(h, add_key_to_strv, &data);

  qsort(result, size, sizeof(char *), cmpstring);

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
  openslide_get_level0_dimensions(osr, &blw, &blh);

  if (!osr->downsamples) {
    osr->downsamples = g_new0(double, osr->level_count);
  }
  if (osr->downsamples[0] == 0) {
    osr->downsamples[0] = 1.0;
  }
  for (int32_t i = 1; i < osr->level_count; i++) {
    if (osr->downsamples[i] == 0) {
      int64_t w, h;
      openslide_get_level_dimensions(osr, i, &w, &h);

      osr->downsamples[i] =
        (((double) blh / (double) h) +
         ((double) blw / (double) w)) / 2.0;
    }
  }

  // check downsamples
  for (int32_t i = 1; i < osr->level_count; i++) {
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
  const char *hash_str = _openslide_hash_get_string(quickhash1);
  if (hash_str != NULL) {
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_QUICKHASH1),
                        g_strdup(hash_str));
  }
  _openslide_hash_destroy(quickhash1);

  // set other properties
  g_hash_table_insert(osr->properties,
		      g_strdup(_OPENSLIDE_PROPERTY_NAME_LEVEL_COUNT),
		      g_strdup_printf("%d", osr->level_count));
  for (int32_t i = 0; i < osr->level_count; i++) {
    int64_t w, h;
    openslide_get_level_dimensions(osr, i, &w, &h);

    char downsample[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_dtostr(downsample, sizeof(downsample), osr->downsamples[i]);

    g_hash_table_insert(osr->properties,
			g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_WIDTH, i),
			g_strdup_printf("%" G_GINT64_FORMAT, w));
    g_hash_table_insert(osr->properties,
			g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_HEIGHT, i),
			g_strdup_printf("%" G_GINT64_FORMAT, h));
    g_hash_table_insert(osr->properties,
			g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_DOWNSAMPLE, i),
			g_strdup(downsample));
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

  g_free(g_atomic_pointer_get(&osr->error));

  g_slice_free(openslide_t, osr);
}


void openslide_get_level0_dimensions(openslide_t *osr,
                                     int64_t *w, int64_t *h) {
  openslide_get_level_dimensions(osr, 0, w, h);
}

void openslide_get_level_dimensions(openslide_t *osr, int32_t level,
				    int64_t *w, int64_t *h) {
  *w = -1;
  *h = -1;

  if (openslide_get_error(osr)) {
    return;
  }

  if (!level_in_range(osr, level)) {
    return;
  }

  (osr->ops->get_dimensions)(osr, level, w, h);
}

void openslide_get_layer0_dimensions(openslide_t *osr,
                                     int64_t *w, int64_t *h) {
  openslide_get_level0_dimensions(osr, w, h);
}

void openslide_get_layer_dimensions(openslide_t *osr, int32_t level,
                                    int64_t *w, int64_t *h) {
  openslide_get_level_dimensions(osr, level, w, h);
}


const char *openslide_get_comment(openslide_t *osr) {
  return openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_COMMENT);
}


int32_t openslide_get_level_count(openslide_t *osr) {
  if (openslide_get_error(osr)) {
    return -1;
  }

  return osr->level_count;
}

int32_t openslide_get_layer_count(openslide_t *osr) {
  return openslide_get_level_count(osr);
}


int32_t openslide_get_best_level_for_downsample(openslide_t *osr,
						double downsample) {
  if (openslide_get_error(osr)) {
    return -1;
  }

  // too small, return first
  if (downsample < osr->downsamples[0]) {
    return 0;
  }

  // find where we are in the middle
  for (int32_t i = 1; i < osr->level_count; i++) {
    if (downsample < osr->downsamples[i]) {
      return i - 1;
    }
  }

  // too big, return last
  return osr->level_count - 1;
}

int32_t openslide_get_best_layer_for_downsample(openslide_t *osr,
						double downsample) {
  return openslide_get_best_level_for_downsample(osr, downsample);
}


double openslide_get_level_downsample(openslide_t *osr, int32_t level) {
  if (openslide_get_error(osr) || !level_in_range(osr, level)) {
    return -1.0;
  }

  return osr->downsamples[level];
}

double openslide_get_layer_downsample(openslide_t *osr, int32_t level) {
  return openslide_get_level_downsample(osr, level);
}


int openslide_give_prefetch_hint(openslide_t *osr G_GNUC_UNUSED,
				 int64_t x G_GNUC_UNUSED,
				 int64_t y G_GNUC_UNUSED,
				 int32_t level G_GNUC_UNUSED,
				 int64_t w G_GNUC_UNUSED,
				 int64_t h G_GNUC_UNUSED) {
  g_warning("openslide_give_prefetch_hint has never been implemented and should not be called");
  return 0;
}

void openslide_cancel_prefetch_hint(openslide_t *osr G_GNUC_UNUSED,
				    int prefetch_id G_GNUC_UNUSED) {
  g_warning("openslide_cancel_prefetch_hint has never been implemented and should not be called");
}

static void read_region(openslide_t *osr,
			cairo_t *cr,
			int64_t x, int64_t y,
			int32_t level,
			int64_t w, int64_t h) {
  // save the old pattern, it's the only thing push/pop won't restore
  cairo_pattern_t *old_source = cairo_get_source(cr);
  cairo_pattern_reference(old_source);

  // push, so that saturate works with all sorts of backends
  cairo_push_group(cr);

  // clear to set the bounds of the group (seems to be a recent cairo bug)
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // saturate those seams away!
  cairo_set_operator(cr, CAIRO_OPERATOR_SATURATE);

  if (level_in_range(osr, level)) {
    // offset if given negative coordinates
    double ds = openslide_get_level_downsample(osr, level);
    int64_t tx = 0;
    int64_t ty = 0;
    if (x < 0) {
      tx = (-x) / ds;
      x = 0;
      w -= tx;
    }
    if (y < 0) {
      ty = (-y) / ds;
      y = 0;
      h -= ty;
    }
    cairo_translate(cr, tx, ty);

    // paint
    if (w > 0 && h > 0) {
      (osr->ops->paint_region)(osr, cr, x, y, level, w, h);
    }
  }

  cairo_pop_group_to_source(cr);

  if (!openslide_get_error(osr)) {
    // commit, nothing went wrong
    cairo_paint(cr);
  }

  // restore old source
  cairo_set_source(cr, old_source);
  cairo_pattern_destroy(old_source);
}

static bool ensure_nonnegative_dimensions(openslide_t *osr, int64_t w, int64_t h) {
  if (w < 0 || h < 0) {
    _openslide_set_error(osr,
			 "negative width (%" G_GINT64_FORMAT ") or negative height (%"
			 G_GINT64_FORMAT ") not allowed", w, h);
    return false;
  }
  return true;
}

void openslide_read_region(openslide_t *osr,
			   uint32_t *dest,
			   int64_t x, int64_t y,
			   int32_t level,
			   int64_t w, int64_t h) {
  if (!ensure_nonnegative_dimensions(osr, w, h)) {
    return;
  }

  // clear the dest
  if (dest) {
    memset(dest, 0, w * h * 4);
  }

  // now that it's cleared, return if an error occurred
  if (openslide_get_error(osr)) {
    return;
  }

  // Break the work into smaller pieces if the region is large, because:
  // 1. Cairo will not allow surfaces larger than 32767 pixels on a side.
  // 2. cairo_push_group() creates an intermediate surface backed by a
  //    pixman_image_t, and Pixman requires that every byte of that image
  //    be addressable in 31 bits.
  // 3. We would like to constrain the intermediate surface to a reasonable
  //    amount of RAM.
  const int64_t d = 4096;
  double ds = openslide_get_level_downsample(osr, level);
  for (int64_t row = 0; !openslide_get_error(osr) && row < (h + d - 1) / d;
          row++) {
    for (int64_t col = 0; !openslide_get_error(osr) && col < (w + d - 1) / d;
            col++) {
      // calculate surface coordinates and size
      int64_t sx = x + col * d * ds;     // level 0 plane
      int64_t sy = y + row * d * ds;     // level 0 plane
      int64_t sw = MIN(w - col * d, d);  // level plane
      int64_t sh = MIN(h - row * d, d);  // level plane

      // create the cairo surface for the dest
      cairo_surface_t *surface;
      if (dest) {
        surface = cairo_image_surface_create_for_data(
                (unsigned char *) (dest + w * row * d + col * d),
                CAIRO_FORMAT_ARGB32, sw, sh, w * 4);
      } else {
        // nil surface
        surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 0, 0);
      }

      // create the cairo context
      cairo_t *cr = cairo_create(surface);
      cairo_surface_destroy(surface);

      // paint
      read_region(osr, cr, sx, sy, level, sw, sh);

      // done
      _openslide_check_cairo_status_possibly_set_error(osr, cr);
      cairo_destroy(cr);
    }
  }

  // ensure we don't return a partial result
  if (openslide_get_error(osr)) {
    memset(dest, 0, w * h * 4);
  }
}


void openslide_cairo_read_region(openslide_t *osr,
				 cairo_t *cr,
				 int64_t x, int64_t y,
				 int32_t level,
				 int64_t w, int64_t h) {
  if (!ensure_nonnegative_dimensions(osr, w, h)) {
    return;
  }

  read_region(osr, cr, x, y, level, w, h);

  _openslide_check_cairo_status_possibly_set_error(osr, cr);
}


const char * const *openslide_get_property_names(openslide_t *osr) {
  if (openslide_get_error(osr)) {
    return EMPTY_STRING_ARRAY;
  }

  return osr->property_names;
}

const char *openslide_get_property_value(openslide_t *osr, const char *name) {
  if (openslide_get_error(osr)) {
    return NULL;
  }

  return (const char *) g_hash_table_lookup(osr->properties, name);
}

const char * const *openslide_get_associated_image_names(openslide_t *osr) {
  if (openslide_get_error(osr)) {
    return EMPTY_STRING_ARRAY;
  }

  return osr->associated_image_names;
}

void openslide_get_associated_image_dimensions(openslide_t *osr, const char *name,
					       int64_t *w, int64_t *h) {
  *w = -1;
  *h = -1;

  if (openslide_get_error(osr)) {
    return;
  }

  struct _openslide_associated_image *img =
    (struct _openslide_associated_image *) g_hash_table_lookup(osr->associated_images,
							      name);
  if (img) {
    *w = img->w;
    *h = img->h;
  }
}

void openslide_read_associated_image(openslide_t *osr,
				     const char *name,
				     uint32_t *dest) {
  if (openslide_get_error(osr)) {
    return;
  }

  struct _openslide_associated_image *img =
    (struct _openslide_associated_image *) g_hash_table_lookup(osr->associated_images,
							       name);
  if (img) {
    // this function is documented to do nothing on failure, so we need an
    // extra memcpy
    size_t pixels = img->w * img->h;
    uint32_t *buf = g_new(uint32_t, pixels);

    img->get_argb_data(osr, img->ctx, buf, img->w, img->h);
    if (dest && !openslide_get_error(osr)) {
      memcpy(dest, buf, pixels * sizeof(uint32_t));
    }

    g_free(buf);
  }
}

void _openslide_set_background_color_property(GHashTable *ht,
					      uint8_t r, uint8_t g, uint8_t b) {
  g_return_if_fail(g_hash_table_lookup(ht,
				       OPENSLIDE_PROPERTY_NAME_BACKGROUND_COLOR) == NULL);

  g_hash_table_insert(ht, g_strdup(OPENSLIDE_PROPERTY_NAME_BACKGROUND_COLOR),
		      g_strdup_printf("%.02X%.02X%.02X", r, g, b));
}

const char *openslide_get_version(void) {
  return PACKAGE_VERSION;
}
