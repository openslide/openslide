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
#include <unistd.h>
#include <errno.h>
#include "openslide-common.h"
#include "slidetool.h"

struct output open_output(const char *filename) {
  struct output out;
  if (filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
      common_fail("Can't open %s for writing: %s", filename, strerror(errno));
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
      common_fail("Can't close output: %s", strerror(errno));
    }
  } else {
    if (fflush(out->fp)) {
      common_fail("Can't flush stdout: %s", strerror(errno));
    }
  }
}
