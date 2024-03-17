/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
 *  Copyright (c) 2021-2022 Benjamin Gilbert
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
#include "openslide-decode-tifflike.h"

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib-object.h>
#include <cairo.h>
#include <libxml/parser.h>

#include "openslide-error.h"

const char _openslide_release_info[] = "OpenSlide " SUFFIXED_VERSION ", copyright (C) 2007-2023 Carnegie Mellon University and others.\nLicensed under the GNU Lesser General Public License, version 2.1.";

static const char * const EMPTY_STRING_ARRAY[] = { NULL };

static const struct _openslide_format *formats[] = {
  &_openslide_format_synthetic,
  &_openslide_format_mirax,
  &_openslide_format_dicom,
  &_openslide_format_hamamatsu_vms_vmu,
  &_openslide_format_hamamatsu_ndpi,
  &_openslide_format_sakura,
  &_openslide_format_trestle,
  &_openslide_format_aperio,
  &_openslide_format_leica,
  &_openslide_format_philips_tiff,
  &_openslide_format_ventana,
  &_openslide_format_generic_tiff,
  NULL,
};

static bool openslide_was_dynamically_loaded;

// called from shared-library constructor!
static void __attribute__((constructor)) _openslide_init(void) {
  // init libxml2
  xmlInitParser();
  // parse debug options
  _openslide_debug_init();
  openslide_was_dynamically_loaded = true;
}

static void destroy_associated_image(gpointer data) {
  struct _openslide_associated_image *img = data;

  img->ops->destroy(img);
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

// pixman 0.38.x produces corrupt output.  Test for this at runtime, since
// we might have been compiled with a different version, and the distro
// might have backported a fix.
// https://github.com/openslide/openslide/issues/278
// https://gitlab.freedesktop.org/pixman/pixman/-/commit/8256c235
static void *verify_pixman_works(void *arg G_GNUC_UNUSED) {
  const int DIM = 16;
  g_autofree uint32_t *dest = g_new0(uint32_t, DIM * DIM);
  g_autofree uint32_t *src = g_new(uint32_t, DIM * DIM);
  memset(src, 0xff, DIM * DIM * 4);

  {
    g_autoptr(cairo_surface_t) dest_surface =
      cairo_image_surface_create_for_data((unsigned char *) dest,
                                          CAIRO_FORMAT_ARGB32,
                                          DIM, DIM, DIM * 4);
    g_autoptr(cairo_t) cr = cairo_create(dest_surface);
    // important
    cairo_set_operator(cr, CAIRO_OPERATOR_SATURATE);

    g_autoptr(cairo_surface_t) src_surface =
      cairo_image_surface_create_for_data((unsigned char *) src,
                                          CAIRO_FORMAT_ARGB32,
                                          DIM, DIM, DIM * 4);
    // fractional Y is important
    cairo_set_source_surface(cr, src_surface, 0, 0.2);
    cairo_paint(cr);
  }

  // white pixel if working, transparent if broken
  return GINT_TO_POINTER(dest[8 * 16 + 8] != 0);
}

static const struct _openslide_format *detect_format(const char *filename,
                                                     struct _openslide_tifflike **tl_OUT) {
  GError *tmp_err = NULL;

  g_autoptr(_openslide_tifflike) tl =
    _openslide_tifflike_create(filename, &tmp_err);
  if (!tl) {
    if (_openslide_debug(OPENSLIDE_DEBUG_DETECTION)) {
      g_message("tifflike: %s", tmp_err->message);
    }
    g_clear_error(&tmp_err);
  }

  for (const struct _openslide_format **cur = formats; *cur; cur++) {
    const struct _openslide_format *format = *cur;

    g_assert(format->name && format->vendor &&
             format->detect && format->open);

    if (format->detect(filename, tl, &tmp_err)) {
      // success!
      if (tl_OUT) {
        *tl_OUT = g_steal_pointer(&tl);
      }
      return format;
    }

    // reset for next format
    if (_openslide_debug(OPENSLIDE_DEBUG_DETECTION)) {
      g_message("%s: %s", format->name, tmp_err->message);
    }
    g_clear_error(&tmp_err);
  }

  // no match
  return NULL;
}

static bool open_backend(openslide_t *osr,
                         const struct _openslide_format *format,
                         const char *filename,
                         struct _openslide_tifflike *tl,
                         struct _openslide_hash *quickhash1,
                         GError **err) {
  if (!format->open(osr, filename, tl, quickhash1, err)) {
    if (err && !*err) {
      // error-handling bug in open function
      g_warning("%s opener failed without setting error", format->name);
      // assume the worst
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unknown error");
    }
    return false;
  }
  if (err && *err) {
    // error-handling bug in open function
    g_warning("%s opener succeeded but set error", format->name);
    return false;
  }
  return true;
}

const char *openslide_detect_vendor(const char *filename) {
  g_assert(openslide_was_dynamically_loaded);

  const struct _openslide_format *format = detect_format(filename, NULL);
  if (!format) {
    return NULL;
  }
  return format->vendor;
}

static int cmpstring(const void *p1, const void *p2) {
  return strcmp(* (char * const *) p1, * (char * const *) p2);
}

static const char **strv_from_hashtable_keys(GHashTable *h) {
  guint size;
  const char **result = (const char **) g_hash_table_get_keys_as_array(h,
                                                                       &size);
  qsort(result, size, sizeof(char *), cmpstring);
  return result;
}

openslide_t *openslide_open(const char *filename) {
  g_assert(openslide_was_dynamically_loaded);

  // detect format
  g_autoptr(_openslide_tifflike) tl = NULL;
  const struct _openslide_format *format = detect_format(filename, &tl);
  if (!format) {
    // not a slide file
    return NULL;
  }

  // alloc memory
  g_autoptr(openslide_t) osr = g_new0(openslide_t, 1);
  osr->properties = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, g_free);
  osr->associated_images = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free,
                                                 destroy_associated_image);

  // refuse to run on unpatched pixman 0.38.x
  static GOnce pixman_once = G_ONCE_INIT;
  g_once(&pixman_once, verify_pixman_works, NULL);
  if (!GPOINTER_TO_INT(pixman_once.retval)) {
    GError *tmp_err = NULL;
    g_set_error(&tmp_err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "pixman 0.38.x does not render correctly; upgrade or downgrade pixman");
    _openslide_propagate_error(osr, tmp_err);
    return g_steal_pointer(&osr);
  }

  // open backend
  g_autoptr(_openslide_hash) quickhash1 = _openslide_hash_quickhash1_create();
  GError *tmp_err = NULL;
  if (!open_backend(osr, format, filename, tl, quickhash1, &tmp_err)) {
    // failed to read slide
    _openslide_propagate_error(osr, tmp_err);
    return g_steal_pointer(&osr);
  }
  g_assert(osr->levels);

  // compute downsamples if not done already
  int64_t blw, blh;
  openslide_get_level0_dimensions(osr, &blw, &blh);

  if (osr->level_count && osr->levels[0]->downsample == 0) {
    osr->levels[0]->downsample = 1.0;
  }
  for (int32_t i = 1; i < osr->level_count; i++) {
    struct _openslide_level *l = osr->levels[i];
    if (l->downsample == 0) {
      l->downsample =
        (((double) blh / (double) l->h) +
         ((double) blw / (double) l->w)) / 2.0;
    }
  }

  // check downsamples
  for (int32_t i = 1; i < osr->level_count; i++) {
    //g_debug("downsample: %g", osr->levels[i]->downsample);

    if (osr->levels[i]->downsample < osr->levels[i - 1]->downsample) {
      g_warning("Downsampled images not correctly ordered: %g < %g",
		osr->levels[i]->downsample, osr->levels[i - 1]->downsample);
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

  // set other properties
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
                      g_strdup(format->vendor));
  if (osr->icc_profile_size) {
    g_hash_table_insert(osr->properties,
                        g_strdup(OPENSLIDE_PROPERTY_NAME_ICC_SIZE),
                        g_strdup_printf("%"PRId64, osr->icc_profile_size));
  }
  g_hash_table_insert(osr->properties,
		      g_strdup(_OPENSLIDE_PROPERTY_NAME_LEVEL_COUNT),
		      g_strdup_printf("%d", osr->level_count));
  bool should_have_geometry = false;  // initialize for gcc 4.4
  for (int32_t i = 0; i < osr->level_count; i++) {
    struct _openslide_level *l = osr->levels[i];

    g_hash_table_insert(osr->properties,
			g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_WIDTH, i),
			g_strdup_printf("%"PRId64, l->w));
    g_hash_table_insert(osr->properties,
			g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_HEIGHT, i),
			g_strdup_printf("%"PRId64, l->h));
    g_hash_table_insert(osr->properties,
			g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_DOWNSAMPLE, i),
			_openslide_format_double(l->downsample));

    // tile geometry
    bool have_geometry = (l->tile_w > 0 && l->tile_h > 0);
    if (i == 0) {
      should_have_geometry = have_geometry;
    }
    if (have_geometry != should_have_geometry) {
      g_warning("Inconsistent tile geometry hints between levels");
    }
    if (have_geometry) {
      g_hash_table_insert(osr->properties,
                          g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_TILE_WIDTH, i),
                          g_strdup_printf("%"PRId64, l->tile_w));
      g_hash_table_insert(osr->properties,
                          g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_TILE_HEIGHT, i),
                          g_strdup_printf("%"PRId64, l->tile_h));
    }
  }

  // fill in associated image names and set properties
  osr->associated_image_names = strv_from_hashtable_keys(osr->associated_images);
  for (const char **name = osr->associated_image_names; *name != NULL; name++) {
    struct _openslide_associated_image *img =
      g_hash_table_lookup(osr->associated_images, *name);
    g_hash_table_insert(osr->properties,
			g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_ASSOCIATED_WIDTH, *name),
			g_strdup_printf("%"PRId64, img->w));
    g_hash_table_insert(osr->properties,
			g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_ASSOCIATED_HEIGHT, *name),
			g_strdup_printf("%"PRId64, img->h));
    if (img->icc_profile_size) {
      g_hash_table_insert(osr->properties,
                          g_strdup_printf(_OPENSLIDE_PROPERTY_NAME_TEMPLATE_ASSOCIATED_ICC_SIZE, *name),
                          g_strdup_printf("%"PRId64, img->icc_profile_size));
    }
  }

  // fill in property names
  osr->property_names = strv_from_hashtable_keys(osr->properties);

  // start cache
  osr->cache = _openslide_cache_binding_create();

  return g_steal_pointer(&osr);
}


void openslide_close(openslide_t *osr) {
  if (osr->ops) {
    (osr->ops->destroy)(osr);
  }

  g_hash_table_unref(osr->associated_images);
  g_hash_table_unref(osr->properties);

  g_free(osr->associated_image_names);
  g_free(osr->property_names);

  if (osr->cache) {
    _openslide_cache_binding_destroy(osr->cache);
  }

  g_free(g_atomic_pointer_get(&osr->error));

  g_free(osr);
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

  *w = osr->levels[level]->w;
  *h = osr->levels[level]->h;
}


int32_t openslide_get_level_count(openslide_t *osr) {
  if (openslide_get_error(osr)) {
    return -1;
  }

  return osr->level_count;
}


int32_t openslide_get_best_level_for_downsample(openslide_t *osr,
						double downsample) {
  if (openslide_get_error(osr)) {
    return -1;
  }

  // too small, return first
  if (downsample < osr->levels[0]->downsample) {
    return 0;
  }

  // find where we are in the middle
  for (int32_t i = 1; i < osr->level_count; i++) {
    if (downsample < osr->levels[i]->downsample) {
      return i - 1;
    }
  }

  // too big, return last
  return osr->level_count - 1;
}


double openslide_get_level_downsample(openslide_t *osr, int32_t level) {
  if (openslide_get_error(osr) || !level_in_range(osr, level)) {
    return -1.0;
  }

  return osr->levels[level]->downsample;
}


static bool read_region_area(openslide_t *osr,
                             uint32_t *dest, int64_t stride,
                             int64_t x, int64_t y,
                             int32_t level,
                             int64_t w, int64_t h,
                             GError **err) {
  // create the cairo surface for the dest
  g_autoptr(cairo_surface_t) surface = NULL;
  if (dest) {
    surface =
      cairo_image_surface_create_for_data((unsigned char *) dest,
                                          CAIRO_FORMAT_ARGB32,
                                          w, h, stride);
  } else {
    // nil surface
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 0, 0);
  }

  // create the cairo context
  g_autoptr(cairo_t) cr = cairo_create(surface);

  // saturate those seams away!
  cairo_set_operator(cr, CAIRO_OPERATOR_SATURATE);

  if (level_in_range(osr, level)) {
    struct _openslide_level *l = osr->levels[level];

    // offset if given negative coordinates
    double ds = l->downsample;
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
      if (!osr->ops->paint_region(osr, cr, x, y, l, w, h, err)) {
        return false;
      }
    }
  }

  // done
  if (!_openslide_check_cairo_status(cr, err)) {
    return false;
  }

  return true;
}

void openslide_read_region(openslide_t *osr,
			   uint32_t *dest,
			   int64_t x, int64_t y,
			   int32_t level,
			   int64_t w, int64_t h) {
  if (w < 0 || h < 0) {
    GError *tmp_err = g_error_new(OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                                  "negative width (%"PRId64") "
                                  "or negative height (%"PRId64") "
                                  "not allowed", w, h);
    _openslide_propagate_error(osr, tmp_err);
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
  // 2. cairo_image_surface_create_for_data() creates a surface backed by a
  //    pixman_image_t, and Pixman requires that every byte of that image
  //    be addressable in 31 bits.
  const int64_t d = 4096;
  double ds = openslide_get_level_downsample(osr, level);
  for (int64_t row = 0; row < (h + d - 1) / d; row++) {
    for (int64_t col = 0; col < (w + d - 1) / d; col++) {
      // calculate surface coordinates and size
      int64_t sx = x + col * d * ds;     // level 0 plane
      int64_t sy = y + row * d * ds;     // level 0 plane
      int64_t sw = MIN(w - col * d, d);  // level plane
      int64_t sh = MIN(h - row * d, d);  // level plane

      // paint
      GError *tmp_err = NULL;
      if (!read_region_area(osr,
                            dest ? dest + w * row * d + col * d : NULL, w * 4,
                            sx, sy, level, sw, sh,
                            &tmp_err)) {
        _openslide_propagate_error(osr, tmp_err);
        if (dest) {
          // ensure we don't return a partial result
          memset(dest, 0, w * h * 4);
        }
        return;
      }
    }
  }
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

  return g_hash_table_lookup(osr->properties, name);
}

int64_t openslide_get_icc_profile_size(openslide_t *osr) {
  if (openslide_get_error(osr)) {
    return -1;
  }

  return osr->icc_profile_size;
}

void openslide_read_icc_profile(openslide_t *osr, void *dest) {
  if (openslide_get_error(osr)) {
    memset(dest, 0, osr->icc_profile_size);
    return;
  }
  if (!osr->icc_profile_size) {
    return;
  }
  g_assert(osr->ops->read_icc_profile);

  GError *tmp_err = NULL;
  if (!osr->ops->read_icc_profile(osr, dest, &tmp_err)) {
    _openslide_propagate_error(osr, tmp_err);
    memset(dest, 0, osr->icc_profile_size);
  }
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

  struct _openslide_associated_image *img = g_hash_table_lookup(osr->associated_images,
								name);
  if (img) {
    *w = img->w;
    *h = img->h;
  }
}

void openslide_read_associated_image(openslide_t *osr,
				     const char *name,
				     uint32_t *dest) {
  struct _openslide_associated_image *img =
    g_hash_table_lookup(osr->associated_images, name);
  if (!img) {
    return;
  }
  size_t pixels = img->w * img->h;

  if (openslide_get_error(osr)) {
    memset(dest, 0, pixels * sizeof(uint32_t));
    return;
  }

  GError *tmp_err = NULL;
  if (!img->ops->get_argb_data(img, dest, &tmp_err)) {
    _openslide_propagate_error(osr, tmp_err);
    // ensure we don't return a partial result
    memset(dest, 0, pixels * sizeof(uint32_t));
  }
}

int64_t openslide_get_associated_image_icc_profile_size(openslide_t *osr,
                                                        const char *name) {
  if (openslide_get_error(osr)) {
    return -1;
  }

  struct _openslide_associated_image *img =
    g_hash_table_lookup(osr->associated_images, name);
  if (!img) {
    return -1;
  }
  return img->icc_profile_size;
}

void openslide_read_associated_image_icc_profile(openslide_t *osr,
                                                 const char *name,
                                                 void *dest) {
  struct _openslide_associated_image *img =
    g_hash_table_lookup(osr->associated_images, name);
  if (!img) {
    return;
  }

  if (openslide_get_error(osr)) {
    memset(dest, 0, img->icc_profile_size);
    return;
  }
  if (!img->icc_profile_size) {
    return;
  }
  g_assert(img->ops->read_icc_profile);

  GError *tmp_err = NULL;
  if (!img->ops->read_icc_profile(img, dest, &tmp_err)) {
    _openslide_propagate_error(osr, tmp_err);
    memset(dest, 0, img->icc_profile_size);
  }
}

openslide_cache_t *openslide_cache_create(size_t capacity) {
  return _openslide_cache_create(capacity);
}

void openslide_set_cache(openslide_t *osr, openslide_cache_t *cache) {
  if (openslide_get_error(osr)) {
    return;
  }
  _openslide_cache_binding_set(osr->cache, cache);
}

void openslide_cache_release(openslide_cache_t *cache) {
  _openslide_cache_release(cache);
}

const char *openslide_get_version(void) {
  return SUFFIXED_VERSION;
}
