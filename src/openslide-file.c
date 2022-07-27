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

#define NO_POISON_FSEEKO
#include "openslide-private.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <glib.h>

#ifdef HAVE_FCNTL
#include <unistd.h>
#include <fcntl.h>
#endif

struct _openslide_file {
  FILE *fp;
};

#undef fopen
#undef fread
#undef fclose
#undef g_file_test

static void wrap_fclose(FILE *fp) {
  fclose(fp);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FILE, wrap_fclose)

static void io_error(GError **err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void io_error(GError **err, const char *fmt, ...) {
  int my_errno = errno;
  va_list ap;

  va_start(ap, fmt);
  g_autofree char *msg = g_strdup_vprintf(fmt, ap);
  g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(my_errno),
              "%s: %s", msg, g_strerror(my_errno));
  va_end(ap);
}

static FILE *do_fopen(const char *path, const char *mode, GError **err) {
  FILE *f;

#ifdef HAVE__WFOPEN
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
  if (f == NULL) {
    io_error(err, "Couldn't open %s", path);
  }
#else
  f = fopen(path, mode);
  if (f == NULL) {
    io_error(err, "Couldn't open %s", path);
  }
#endif

  return f;
}

struct _openslide_file *_openslide_fopen(const char *path, GError **err)
{
  g_autoptr(FILE) f = do_fopen(path, "rb" FOPEN_CLOEXEC_FLAG, err);
  if (f == NULL) {
    return NULL;
  }

  /* Unnecessary if FOPEN_CLOEXEC_FLAG is non-empty.  Not built on Windows. */
#ifdef HAVE_FCNTL
  if (!FOPEN_CLOEXEC_FLAG[0]) {
    int fd = fileno(f);
    if (fd == -1) {
      io_error(err, "Couldn't fileno() %s", path);
      return NULL;
    }
    long flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
      io_error(err, "Couldn't F_GETFD %s", path);
      return NULL;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC)) {
      io_error(err, "Couldn't F_SETFD %s", path);
      return NULL;
    }
  }
#endif

  struct _openslide_file *file = g_slice_new0(struct _openslide_file);
  file->fp = g_steal_pointer(&f);
  return file;
}

size_t _openslide_fread(struct _openslide_file *file, void *buf, size_t size) {
  char *bufp = buf;
  size_t total = 0;
  while (total < size) {
    size_t count = fread(bufp + total, 1, size - total, file->fp);
    if (count == 0) {
      return total;
    }
    total += count;
  }
  return total;
}

bool _openslide_fseek(struct _openslide_file *file, off_t offset, int whence,
                      GError **err) {
  if (fseeko(file->fp, offset, whence)) {
    g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                "%s", g_strerror(errno));
    return false;
  }
  return true;
}

off_t _openslide_ftell(struct _openslide_file *file, GError **err) {
  off_t ret = ftello(file->fp);
  if (ret == -1) {
    g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                "%s", g_strerror(errno));
  }
  return ret;
}

off_t _openslide_fsize(struct _openslide_file *file, GError **err) {
  off_t orig = _openslide_ftell(file, err);
  if (orig == -1) {
    return -1;
  }
  if (!_openslide_fseek(file, 0, SEEK_END, err)) {
    return -1;
  }
  off_t ret = _openslide_ftell(file, err);
  if (ret == -1) {
    return -1;
  }
  if (!_openslide_fseek(file, orig, SEEK_SET, err)) {
    return -1;
  }
  return ret;
}

void _openslide_fclose(struct _openslide_file *file) {
  fclose(file->fp);
  g_slice_free(struct _openslide_file, file);
}

bool _openslide_fexists(const char *path, GError **err G_GNUC_UNUSED) {
  return g_file_test(path, G_FILE_TEST_EXISTS);
}
