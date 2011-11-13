/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2011 Carnegie Mellon University
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

#include <stdint.h>
#include <string.h>
#include <glib.h>
#include <errno.h>

#ifdef HAVE_FCNTL
#include <unistd.h>
#include <fcntl.h>
#endif


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

gboolean _openslide_read_key_file(GKeyFile *key_file, const char *filename,
                                  GKeyFileFlags flags, GError **error)
{
  FILE *f;
  gchar *buf;
  gsize cur_len;
  gsize len = 0;
  gsize alloc_len = 64 << 10;
  int offset = 0;
  gboolean result;

  /* We load the whole key file into memory and parse it with
   * g_key_file_load_from_data instead of using g_key_file_load_from_file
   * because the load_from_file function incorrectly parses a value when
   * the terminating '\r\n' falls across a 4KB boundary.
   * https://bugzilla.redhat.com/show_bug.cgi?id=649936 */

  /* this also allows us to skip a UTF-8 BOM which the g_key_file parser
   * does not expect to find. */

  /* Hamamatsu attempts to load the slide file as a key file.  We impose
     a maximum file size to avoid loading an entire slide into RAM. */

  f = _openslide_fopen(filename, "rb");
  if (f == NULL) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Couldn't open key file %s: %s", filename, g_strerror(errno));
    return false;
  }

  buf = (gchar *) g_malloc(alloc_len);
  while ((cur_len = fread(buf + len, 1, alloc_len - len, f)) > 0) {
    len += cur_len;
    if (len == alloc_len) {
      if (alloc_len >= (1 << 20)) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                    "Key file %s too large", filename);
        g_free(buf);
        fclose(f);
        return false;
      }
      alloc_len *= 2;
      buf = (gchar *) g_realloc(buf, alloc_len);
    }
  }

  if (ferror(f)) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Couldn't read key file %s: %s", filename, g_strerror(errno));
    g_free(buf);
    fclose(f);
    return false;
  }

  if (fclose(f)) {
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Failed closing key file %s: %s", filename, g_strerror(errno));
    g_free(buf);
    return false;
  }

  /* skip the UTF-8 BOM if it is present. */
  if (memcmp(buf, "\xef\xbb\xbf", 3) == 0) {
    offset = 3;
  }

  result = g_key_file_load_from_data(key_file, buf + offset, len - offset,
                                     flags, error);
  g_free(buf);
  return result;
}

FILE *_openslide_fopen(const char *path, const char *mode)
{
  char *m = g_strconcat(mode, FOPEN_CLOEXEC_FLAG, NULL);
  FILE *f = fopen(path, m);
  g_free(m);

  /* Redundant if FOPEN_CLOEXEC_FLAG is non-empty.  Not built on Windows. */
#ifdef HAVE_FCNTL
  if (f != NULL) {
    int fd = fileno(f);
    if (fd != -1) {
      long flags = fcntl(fd, F_GETFD);
      if (flags != -1) {
        if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC)) {
          g_warning("_openslide_fopen couldn't F_SETFD");
        }
      } else {
        g_warning("_openslide_fopen couldn't F_GETFD");
      }
    } else {
      g_warning("_openslide_fopen couldn't fileno()");
    }
  }
#endif

  return f;
}
