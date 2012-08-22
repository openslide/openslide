/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
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

#include <glib.h>
#include <stdarg.h>
#include <errno.h>

// public error functions
const char *openslide_get_error(openslide_t *osr) {
  return g_atomic_pointer_get(&osr->error);
}

// private error functions
bool _openslide_set_error(openslide_t *osr, const char *format, ...) {
  g_assert(format != NULL);

  va_list args;

  // format it for us
  va_start(args, format);
  char *newmsg = g_strdup_vprintf(format, args);
  va_end(args);

  if (!g_atomic_pointer_compare_and_exchange(&osr->error, NULL, newmsg)) {
    // didn't replace the error, free it
    g_free(newmsg);
    return false;
  } else {
    // error was set, log it
    g_critical("%s", newmsg);
    return true;
  }
}

bool _openslide_check_cairo_status_possibly_set_error(openslide_t *osr, cairo_t *cr) {
  cairo_status_t status = cairo_status(cr);
  if (!status) {
    return false;
  }

  // cairo has error, let's set our error from it
  return _openslide_set_error(osr, "cairo error: %s",
			      cairo_status_to_string(status));
}

// internal error propagation
void _openslide_io_error(GError **err, const char *fmt, ...) {
  int my_errno = errno;
  va_list ap;

  va_start(ap, fmt);
  char *msg = g_strdup_vprintf(fmt, ap);
  g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(my_errno),
              "%s: %s", msg, g_strerror(my_errno));
  g_free(msg);
  va_end(ap);
}
