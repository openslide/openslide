/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "openslide-common.h"

static void warn(const char *fmt, va_list ap) {
  fprintf(stderr, "%s: ", g_get_prgname());
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

void common_warn(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  warn(fmt, ap);
  va_end(ap);
}

void common_fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  warn(fmt, ap);
  va_end(ap);
  exit(1);
}

static bool warn_on_error(openslide_t *osr, const char *fmt, va_list ap) {
  const char *err;
  if (osr) {
    err = openslide_get_error(osr);
  } else {
    err = "Not a file that OpenSlide can recognize";
  }
  if (err) {
    fprintf(stderr, "%s: ", g_get_prgname());
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ": %s\n", err);
    return true;
  }
  return false;
}

bool common_warn_on_error(openslide_t *osr, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bool ret = warn_on_error(osr, fmt, ap);
  va_end(ap);
  return ret;
}

void common_fail_on_error(openslide_t *osr, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bool warned = warn_on_error(osr, fmt, ap);
  va_end(ap);
  if (warned) {
    exit(1);
  }
}
