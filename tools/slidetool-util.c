/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2023 Benjamin Gilbert
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

#include <stdio.h>
#include <errno.h>
#include "openslide-common.h"
#include "slidetool.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

struct output open_output(const char *filename) {
  struct output out;
  if (filename) {
#ifdef _WIN32
    GError *tmp_err = NULL;
    g_autofree wchar_t *filename16 =
      (wchar_t *) g_utf8_to_utf16(filename, -1, NULL, NULL, &tmp_err);
    if (filename16 == NULL) {
      common_fail("Couldn't open %s: %s", filename, tmp_err->message);
    }
    FILE *fp = _wfopen(filename16, L"wb");
#else
    FILE *fp = fopen(filename, "wb");
#endif
    if (!fp) {
      common_fail("Can't open %s for writing: %s", filename, g_strerror(errno));
    }
    out.fp = fp;
  } else {
    if (isatty(1)) {
      common_fail("Will not write binary output to terminal");
    }
    out.fp = stdout;
  }
  return out;
}

void _close_output(struct output *out) {
  if (out->fp != stdout) {
    if (fclose(out->fp)) {
      common_fail("Can't close output: %s", g_strerror(errno));
    }
  } else {
    if (fflush(out->fp)) {
      common_fail("Can't flush stdout: %s", g_strerror(errno));
    }
  }
}
