/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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

// public error functions
const char *openslide_get_error(openslide_t *osr) {
  return (const char *) g_atomic_pointer_get(&osr->error);
}

// private error functions
bool _openslide_set_error(openslide_t *osr, const char *format, ...) {
  g_assert(format != NULL);

  va_list args;

  // log it
  va_start(args, format);
  g_logv(G_LOG_DOMAIN, G_LOG_LEVEL_CRITICAL, format, args);
  va_end(args);

  // format it for us
  va_start(args, format);
  gpointer newmsg = g_strdup_vprintf(format, args);
  va_end(args);

  if (!g_atomic_pointer_compare_and_exchange(&osr->error, NULL, newmsg)) {
    // didn't replace the error, free it
    g_free(newmsg);
    return false;
  } else {
    // error was set
    return true;
  }
}
