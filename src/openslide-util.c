/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2015 Benjamin Gilbert
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

#ifdef HAVE_FCNTL
#include <unistd.h>
#include <fcntl.h>
#endif

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
  {"tiles", OPENSLIDE_DEBUG_TILES, "render tile outlines"},
  {NULL, 0, NULL}
};

static uint32_t debug_flags;


guint _openslide_int64_hash(gconstpointer v) {
  int64_t i = *((const int64_t *) v);
  return i ^ (i >> 32);
}

gboolean _openslide_int64_equal(gconstpointer v1, gconstpointer v2) {
  return *((int64_t *) v1) == *((int64_t *) v2);
}

void _openslide_int64_free(gpointer data) {
  g_slice_free(int64_t, data);
}

GKeyFile *_openslide_read_key_file(const char *filename, int32_t max_size,
                                   GKeyFileFlags flags, GError **err) {
  char *buf = NULL;

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

  FILE *f = _openslide_fopen(filename, "rb", err);
  if (f == NULL) {
    return NULL;
  }

  // get file size and check against maximum
  if (fseeko(f, 0, SEEK_END)) {
    _openslide_io_error(err, "Couldn't seek %s", filename);
    goto FAIL;
  }
  int64_t size = ftello(f);
  if (size == -1) {
    _openslide_io_error(err, "Couldn't get size of %s", filename);
    goto FAIL;
  }
  if (size > max_size) {
    g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                "Key file %s too large", filename);
    goto FAIL;
  }

  // read
  if (fseeko(f, 0, SEEK_SET)) {
    _openslide_io_error(err, "Couldn't seek %s", filename);
    goto FAIL;
  }
  // catch file size changes
  buf = g_malloc(size + 1);
  int64_t total = 0;
  size_t cur_len;
  while ((cur_len = fread(buf + total, 1, size + 1 - total, f)) > 0) {
    total += cur_len;
  }
  if (ferror(f) || total != size) {
    _openslide_io_error(err, "Couldn't read key file %s", filename);
    goto FAIL;
  }

  /* skip the UTF-8 BOM if it is present. */
  int offset = 0;
  if (size >= 3 && memcmp(buf, "\xef\xbb\xbf", 3) == 0) {
    offset = 3;
  }

  // parse
  GKeyFile *key_file = g_key_file_new();
  if (!g_key_file_load_from_data(key_file,
                                 buf + offset, size - offset,
                                 flags, err)) {
    g_key_file_free(key_file);
    goto FAIL;
  }
  g_free(buf);
  fclose(f);
  return key_file;

FAIL:
  g_free(buf);
  fclose(f);
  return NULL;
}

#undef fopen
static FILE *do_fopen(const char *path, const char *mode, GError **err) {
  FILE *f;

#ifdef HAVE__WFOPEN
  wchar_t *path16 = (wchar_t *) g_utf8_to_utf16(path, -1, NULL, NULL, err);
  if (path16 == NULL) {
    g_prefix_error(err, "Couldn't open %s: ", path);
    return NULL;
  }
  wchar_t *mode16 = (wchar_t *) g_utf8_to_utf16(mode, -1, NULL, NULL, err);
  if (mode16 == NULL) {
    g_prefix_error(err, "Bad file mode %s: ", mode);
    g_free(path16);
    return NULL;
  }
  f = _wfopen(path16, mode16);
  if (f == NULL) {
    _openslide_io_error(err, "Couldn't open %s", path);
  }
  g_free(mode16);
  g_free(path16);
#else
  f = fopen(path, mode);
  if (f == NULL) {
    _openslide_io_error(err, "Couldn't open %s", path);
  }
#endif

  return f;
}
#define fopen _OPENSLIDE_POISON(_openslide_fopen)

FILE *_openslide_fopen(const char *path, const char *mode, GError **err)
{
  char *m = g_strconcat(mode, FOPEN_CLOEXEC_FLAG, NULL);
  FILE *f = do_fopen(path, m, err);
  g_free(m);
  if (f == NULL) {
    return NULL;
  }

  /* Unnecessary if FOPEN_CLOEXEC_FLAG is non-empty.  Not built on Windows. */
#ifdef HAVE_FCNTL
  if (!FOPEN_CLOEXEC_FLAG[0]) {
    int fd = fileno(f);
    if (fd == -1) {
      _openslide_io_error(err, "Couldn't fileno() %s", path);
      fclose(f);
      return NULL;
    }
    long flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
      _openslide_io_error(err, "Couldn't F_GETFD %s", path);
      fclose(f);
      return NULL;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC)) {
      _openslide_io_error(err, "Couldn't F_SETFD %s", path);
      fclose(f);
      return NULL;
    }
  }
#endif

  return f;
}

#undef g_ascii_strtod
double _openslide_parse_double(const char *value) {
  // Canonicalize comma to decimal point, since the locale of the
  // originating system sometimes leaks into slide files.
  // This will break if the value includes grouping characters.
  char *canonical = g_strdup(value);
  g_strdelimit(canonical, ",", '.');

  char *endptr;
  errno = 0;
  double result = g_ascii_strtod(canonical, &endptr);
  // fail on overflow/underflow
  if (canonical[0] == 0 || endptr[0] != 0 || errno == ERANGE) {
    result = NAN;
  }

  g_free(canonical);
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

  cairo_surface_t *surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_ARGB32,
                                        tile_w, tile_h,
                                        tile_w * 4);
  cairo_t *cr = cairo_create(surface);
  cairo_surface_destroy(surface);

  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);

  cairo_rectangle(cr, clip_w, 0, tile_w - clip_w, tile_h);
  cairo_fill(cr);

  cairo_rectangle(cr, 0, clip_h, tile_w, tile_h - clip_h);
  cairo_fill(cr);

  bool success = _openslide_check_cairo_status(cr, err);
  cairo_destroy(cr);

  return success;
}

// note: g_getenv() is not reentrant
void _openslide_debug_init(void) {
  const char *debug_str = g_getenv(DEBUG_ENV_VAR);
  if (!debug_str) {
    return;
  }

  char **keywords = g_strsplit(debug_str, ",", 0);
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
  g_strfreev(keywords);
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
