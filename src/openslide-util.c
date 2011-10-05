/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
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
  gchar *tmpbuf = NULL;
  gsize tmplen = 0;
  int offset = 0;
  gboolean result;

  /* We load the whole key file into memory and parse it with
   * g_key_file_load_from_data instead of using g_key_file_load_from_file
   * because the load_from_file function incorrectly parses a value when
   * the terminating '\r\n' falls across a 4KB boundary.
   * https://bugzilla.redhat.com/show_bug.cgi?id=649936 */

  /* this also allows us to skip a UTF-8 BOM which the g_key_file parser
   * does not expect to find. */

  if (!g_file_get_contents(filename, &tmpbuf, &tmplen, error)) {
    return false;
  }

  /* skip the UTF-8 BOM if it is present. */
  if (memcmp(tmpbuf, "\xef\xbb\xbf", 3) == 0) {
    offset = 3;
  }

  result = g_key_file_load_from_data(key_file, tmpbuf + offset, tmplen - offset,
                                     flags, error);
  g_free(tmpbuf);
  return result;
}
