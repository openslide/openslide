/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2015-2022 Benjamin Gilbert
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

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <glib.h>
#include <cairo.h>
#include <zlib.h>

#define KEY_FILE_HARD_MAX_SIZE (100 << 20)

static const char DEBUG_ENV_VAR[] = "OPENSLIDE_DEBUG";

static const struct debug_option {
  const char *kw;
  enum _openslide_debug_flag flag;
  const char *desc;
} debug_options[] = {
  {"detection", OPENSLIDE_DEBUG_DETECTION, "log format detection errors"},
  {"jpeg-markers", OPENSLIDE_DEBUG_JPEG_MARKERS,
   "verify Hamamatsu restart markers"},
  {"performance", OPENSLIDE_DEBUG_PERFORMANCE,
   "log conditions causing poor performance"},
  {"sql", OPENSLIDE_DEBUG_SQL,
   "log SQL queries"},
  {"synthetic", OPENSLIDE_DEBUG_SYNTHETIC,
   "openslide_open(\"\") opens a synthetic test slide"},
  {"tiles", OPENSLIDE_DEBUG_TILES, "render tile outlines"},
  {NULL, 0, NULL}
};

static uint32_t debug_flags;

void _openslide_int64_free(gpointer data) {
  g_slice_free(int64_t, data);
}

GKeyFile *_openslide_read_key_file(const char *filename, int32_t max_size,
                                   GKeyFileFlags flags, GError **err) {
  /* We load the whole key file into memory and parse it with
   * g_key_file_load_from_data instead of using g_key_file_load_from_file
   * because the load_from_file function incorrectly parses a value when
   * the terminating '\r\n' falls across a 4KB boundary.
   * https://bugzilla.redhat.com/show_bug.cgi?id=649936 */

  /* this also allows us to skip a UTF-8 BOM which the g_key_file parser
   * does not expect to find. */

  /* Hamamatsu attempts to load the slide file as a key file.  We impose
     a maximum file size to avoid loading an entire slide into RAM. */

  // hard limit
  if (max_size <= 0) {
    max_size = KEY_FILE_HARD_MAX_SIZE;
  }
  max_size = MIN(max_size, KEY_FILE_HARD_MAX_SIZE);

  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (f == NULL) {
    return NULL;
  }

  // get file size and check against maximum
  int64_t size = _openslide_fsize(f, err);
  if (size == -1) {
    g_prefix_error(err, "Couldn't get size of %s: ", filename);
    return NULL;
  }
  if (size > max_size) {
    g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                "Key file %s too large", filename);
    return NULL;
  }

  // read
  // catch file size changes
  g_autofree char *buf = g_malloc(size + 1);
  int64_t total = 0;
  size_t cur_len;
  while ((cur_len = _openslide_fread(f, buf + total, size + 1 - total)) > 0) {
    total += cur_len;
  }
  if (total != size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read key file %s", filename);
    return NULL;
  }

  /* skip the UTF-8 BOM if it is present. */
  int offset = 0;
  if (size >= 3 && memcmp(buf, "\xef\xbb\xbf", 3) == 0) {
    offset = 3;
  }

  // parse
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  if (!g_key_file_load_from_data(key_file,
                                 buf + offset, size - offset,
                                 flags, err)) {
    return NULL;
  }
  return g_steal_pointer(&key_file);
}

static void zlib_error(z_stream *strm, int64_t dst_len, int error_code,
                       GError **err) {
  if (error_code == Z_STREAM_END) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Short read while decompressing: %lu/%"PRId64,
                strm->total_out, dst_len);
  } else if (strm->msg) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Decompression failure: %s (%s)",
                zError(error_code), strm->msg);
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Decompression failure: %s", zError(error_code));
  }
}

void *_openslide_inflate_buffer(const void *src, int64_t src_len,
                                int64_t dst_len,
                                GError **err) {
  g_autofree void *dst = g_malloc(dst_len);
  z_stream strm = {
    .avail_in = src_len,
    .avail_out = dst_len,
    .next_in = (Bytef *) src,
    .next_out = (Bytef *) dst
  };

  int64_t error_code = inflateInit(&strm);
  if (error_code != Z_OK) {
    zlib_error(&strm, dst_len, error_code, err);
    return NULL;
  }
  error_code = inflate(&strm, Z_FINISH);
  if (error_code != Z_STREAM_END || (int64_t) strm.total_out != dst_len) {
    inflateEnd(&strm);
    zlib_error(&strm, dst_len, error_code, err);
    return NULL;
  }
  error_code = inflateEnd(&strm);
  if (error_code != Z_OK) {
    zlib_error(&strm, dst_len, error_code, err);
    return NULL;
  }
  return g_steal_pointer(&dst);
}

#undef g_ascii_strtod
double _openslide_parse_double(const char *value) {
  // Canonicalize comma to decimal point, since the locale of the
  // originating system sometimes leaks into slide files.
  // This will break if the value includes grouping characters.
  g_autofree char *canonical = g_strdup(value);
  g_strdelimit(canonical, ",", '.');

  char *endptr;
  errno = 0;
  double result = g_ascii_strtod(canonical, &endptr);
  // fail on overflow/underflow
  if (canonical[0] == 0 || endptr[0] != 0 || errno == ERANGE) {
    return NAN;
  }
  return result;
}
#define g_ascii_strtod _OPENSLIDE_POISON(_openslide_parse_double)

char *_openslide_format_double(double d) {
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_dtostr(buf, sizeof buf, d);
  return g_strdup(buf);
}

// if the src prop is an int, canonicalize it and copy it to dest
void _openslide_duplicate_int_prop(openslide_t *osr, const char *src,
                                   const char *dest) {
  g_return_if_fail(g_hash_table_lookup(osr->properties, dest) == NULL);

  char *value = g_hash_table_lookup(osr->properties, src);
  if (value && value[0]) {
    char *endptr;
    int64_t result = g_ascii_strtoll(value, &endptr, 10);
    if (endptr[0] == 0) {
      g_hash_table_insert(osr->properties,
                          g_strdup(dest),
                          g_strdup_printf("%"PRId64, result));
    }
  }
}

// if the src prop is a double, canonicalize it and copy it to dest
void _openslide_duplicate_double_prop(openslide_t *osr, const char *src,
                                      const char *dest) {
  g_return_if_fail(g_hash_table_lookup(osr->properties, dest) == NULL);

  char *value = g_hash_table_lookup(osr->properties, src);
  if (value) {
    double result = _openslide_parse_double(value);
    if (!isnan(result)) {
      g_hash_table_insert(osr->properties, g_strdup(dest),
                          _openslide_format_double(result));
    }
  }
}

void _openslide_set_background_color_prop(openslide_t *osr,
                                          uint8_t r, uint8_t g, uint8_t b) {
  g_return_if_fail(g_hash_table_lookup(osr->properties,
                                       OPENSLIDE_PROPERTY_NAME_BACKGROUND_COLOR) == NULL);

  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_BACKGROUND_COLOR),
                      g_strdup_printf("%.02X%.02X%.02X", r, g, b));
}

void _openslide_set_bounds_props_from_grid(openslide_t *osr,
                                           struct _openslide_grid *grid) {
  g_return_if_fail(g_hash_table_lookup(osr->properties,
                                       OPENSLIDE_PROPERTY_NAME_BOUNDS_X) == NULL);

  double x, y, w, h;
  _openslide_grid_get_bounds(grid, &x, &y, &w, &h);

  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_BOUNDS_X),
                      g_strdup_printf("%"PRId64,
                                      (int64_t) floor(x)));
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_BOUNDS_Y),
                      g_strdup_printf("%"PRId64,
                                      (int64_t) floor(y)));
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_BOUNDS_WIDTH),
                      g_strdup_printf("%"PRId64,
                                      (int64_t) (ceil(x + w) - floor(x))));
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_BOUNDS_HEIGHT),
                      g_strdup_printf("%"PRId64,
                                      (int64_t) (ceil(y + h) - floor(y))));
}

bool _openslide_clip_tile(uint32_t *tiledata,
                          int64_t tile_w, int64_t tile_h,
                          int64_t clip_w, int64_t clip_h,
                          GError **err) {
  if (clip_w >= tile_w && clip_h >= tile_h) {
    return true;
  }

  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_ARGB32,
                                        tile_w, tile_h,
                                        tile_w * 4);
  g_autoptr(cairo_t) cr = cairo_create(surface);

  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);

  cairo_rectangle(cr, clip_w, 0, tile_w - clip_w, tile_h);
  cairo_fill(cr);

  cairo_rectangle(cr, 0, clip_h, tile_w, tile_h - clip_h);
  cairo_fill(cr);

  return _openslide_check_cairo_status(cr, err);
}

// note: g_getenv() is not reentrant
void _openslide_debug_init(void) {
  const char *debug_str = g_getenv(DEBUG_ENV_VAR);
  if (!debug_str) {
    return;
  }

  g_auto(GStrv) keywords = g_strsplit(debug_str, ",", 0);
  bool printed_help = false;
  for (char **kw = keywords; *kw; kw++) {
    g_strstrip(*kw);
    bool found = false;
    for (const struct debug_option *opt = debug_options; opt->kw; opt++) {
      if (!g_ascii_strcasecmp(*kw, opt->kw)) {
        debug_flags |= 1 << opt->flag;
        found = true;
        break;
      }
    }
    if (!found && !printed_help) {
      printed_help = true;
      g_message("%s options (comma-delimited):", DEBUG_ENV_VAR);
      for (const struct debug_option *opt = debug_options; opt->kw; opt++) {
        g_message("   %-15s - %s", opt->kw, opt->desc);
      }
    }
  }
}

bool _openslide_debug(enum _openslide_debug_flag flag) {
  return !!(debug_flags & (1 << flag));
}

void _openslide_performance_warn_once(gint *warned_flag,
                                      const char *str, ...) {
  if (_openslide_debug(OPENSLIDE_DEBUG_PERFORMANCE)) {
    if (warned_flag == NULL ||
        g_atomic_int_compare_and_exchange(warned_flag, 0, 1)) {
      va_list ap;
      va_start(ap, str);
      g_logv(G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, str, ap);
      va_end(ap);
    }
  }
}

struct _openslide_slice _openslide_slice_alloc(gsize len) {
  struct _openslide_slice box = {
    .p = g_slice_alloc(len),
    .len = len,
  };
  return box;
}

void *_openslide_slice_steal(struct _openslide_slice *box) {
  void *p = box->p;
  box->p = NULL;
  return p;
}

void _openslide_slice_free(struct _openslide_slice *box) {
  if (box && box->p) {
    g_slice_free1(box->len, box->p);
    box->p = NULL;
  }
}
