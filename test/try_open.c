/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2012-2013 Carnegie Mellon University
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

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <glib.h>
#include "openslide.h"

#define MAX_FDS 128

static gboolean have_error = FALSE;

static void fail(const char *str, ...) {
  va_list ap;

  if (!have_error) {
    va_start(ap, str);
    vfprintf(stderr, str, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    have_error = TRUE;
  }
}

static void print_log(const gchar *domain G_GNUC_UNUSED,
                      GLogLevelFlags level G_GNUC_UNUSED,
                      const gchar *message, void *data G_GNUC_UNUSED) {
  fprintf(stderr, "[log] %s\n", message);
  have_error = TRUE;
}

static void check_error(openslide_t *osr) {
  const char *error = openslide_get_error(osr);
  if (error != NULL) {
    fail("%s", error);
  }
}

int main(int argc, char **argv) {
  openslide_t *osr;
  const char *filename;
  GHashTable *fds;
  struct stat st;
  bool can_open;

  if (argc != 2) {
    fail("No slide specified");
    return 2;
  }
  filename = argv[1];

  // Record preexisting file descriptors
  fds = g_hash_table_new(g_direct_hash, g_direct_equal);
  for (int i = 0; i < MAX_FDS; i++) {
    if (!fstat(i, &st)) {
      g_hash_table_insert(fds, GINT_TO_POINTER(i), GINT_TO_POINTER(1));
    }
  }

  g_log_set_handler("Openslide", (GLogLevelFlags)
      (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING),
      print_log, NULL);

  can_open = openslide_can_open(filename);
  osr = openslide_open(filename);

  if (osr != NULL) {
    check_error(osr);
    openslide_close(osr);
  } else if (!have_error) {
    // openslide_open returned NULL but logged nothing
    have_error = TRUE;
  }

  // Make sure openslide_can_open() told the truth.
  // This may produce false warnings if messages are logged through glib,
  // but that already indicates a programming error.
  if (can_open != !have_error) {
    fail("openslide_can_open returned %s but openslide_open %s",
         can_open ? "true" : "false", have_error ? "failed" : "succeeded");
  }

  // Check for file descriptor leaks
  for (int i = 0; i < MAX_FDS; i++) {
    if (!fstat(i, &st) && !g_hash_table_lookup(fds, GINT_TO_POINTER(i))) {
      // leaked
      char *link_path = g_strdup_printf("/proc/%d/fd/%d", getpid(), i);
      char *target = g_file_read_link(link_path, NULL);
      if (target == NULL) {
        target = g_strdup("<unknown>");
      }
      fprintf(stderr, "Leaked file descriptor to %s\n", target);
      have_error = TRUE;
      g_free(target);
      g_free(link_path);
    }
  }
  g_hash_table_destroy(fds);

  return have_error ? 1 : 0;
}
