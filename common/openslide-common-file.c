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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <glib.h>
#include "openslide-common.h"
#include "config.h"

static void wrap_fclose(FILE *fp) {
  fclose(fp);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FILE, wrap_fclose)

FILE *common_fopen(const char *path, const char *mode, GError **err) {
  g_autoptr(FILE) f = NULL;

#ifdef _WIN32
  g_autofree wchar_t *path16 =
    (wchar_t *) g_utf8_to_utf16(path, -1, NULL, NULL, err);
  if (path16 == NULL) {
    g_prefix_error(err, "Couldn't open %s: ", path);
    return NULL;
  }
  g_autofree wchar_t *mode16 =
    (wchar_t *) g_utf8_to_utf16(mode, -1, NULL, NULL, err);
  if (mode16 == NULL) {
    g_prefix_error(err, "Bad file mode %s: ", mode);
    return NULL;
  }
  f = _wfopen(path16, mode16);
#else
  f = fopen(path, mode);  // ci-allow
#endif

  if (!f) {
    g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                "Couldn't open %s: %s", path, g_strerror(errno));
    return NULL;
  }

#ifndef _WIN32
  // Unnecessary if FOPEN_CLOEXEC_FLAG is non-empty, but compile-checked
  if (!FOPEN_CLOEXEC_FLAG[0]) {
    int fd = fileno(f);
    if (fd == -1) {
      g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                  "Couldn't fileno() %s: %s", path, g_strerror(errno));
      return NULL;
    }
    long flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
      g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                  "Couldn't F_GETFD %s: %s", path, g_strerror(errno));
      return NULL;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC)) {
      g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                  "Couldn't F_SETFD %s: %s", path, g_strerror(errno));
      return NULL;
    }
  }
#endif
  return g_steal_pointer(&f);
}
