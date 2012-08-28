/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2012 Carnegie Mellon University
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
#include <unistd.h>
#include <glib.h>
#include "openslide.h"

static gboolean have_error = FALSE;

static void print_log(const gchar *domain G_GNUC_UNUSED,
                      GLogLevelFlags level G_GNUC_UNUSED,
                      const gchar *message, void *data G_GNUC_UNUSED) {
  fprintf(stderr, "[log] %s\n", message);
  have_error = TRUE;
}

int main(int argc, char **argv) {
  openslide_t *osr;
  const char *error;
  GHashTable *fds;
  int maxfds;
  struct stat st;

  if (argc != 2) {
    fprintf(stderr, "No slide specified\n");
    return 2;
  }

  // Record preexisting file descriptors
  fds = g_hash_table_new(g_direct_hash, g_direct_equal);
  maxfds = sysconf(_SC_OPEN_MAX);
  g_assert(maxfds != -1);
  for (int i = 0; i < maxfds; i++) {
    if (!fstat(i, &st)) {
      g_hash_table_insert(fds, GINT_TO_POINTER(i), GINT_TO_POINTER(1));
    }
  }

  g_log_set_handler("Openslide", (GLogLevelFlags)
      (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING),
      print_log, NULL);

  osr = openslide_open(argv[1]);

  if (osr != NULL) {
    error = openslide_get_error(osr);
    if (error != NULL) {
      fprintf(stderr, "%s\n", error);
      have_error = TRUE;
    }
    openslide_close(osr);
  } else if (!have_error) {
    // openslide_open returned NULL but logged nothing
    have_error = TRUE;
  }

  // Check for file descriptor leaks
  for (int i = 0; i < maxfds; i++) {
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
