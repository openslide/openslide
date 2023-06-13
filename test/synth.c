/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2022 Benjamin Gilbert
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

// for putenv
#define _XOPEN_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <openslide.h>

#include "openslide-common.h"

int main(int argc, char **argv) {
  if (argc < 2 || !g_str_equal(argv[1], "child")) {
    putenv("OPENSLIDE_DEBUG=synthetic");
    // OpenSlide already evaluated debug flags, so we need to rerun
    // ourselves.  Do it in a cross-platform way.
    char *child_argv[] = {argv[0], "child", NULL};
    GError *err = NULL;
    int status;
    if (!g_spawn_sync(NULL, child_argv, NULL, G_SPAWN_SEARCH_PATH,
                      NULL, NULL, NULL, NULL, &status, &err)) {
      fprintf(stderr, "Spawning child failed: %s\n", err->message);
      g_clear_error(&err);
      return 1;
    }
    if (!g_spawn_check_exit_status(status, NULL)) {
      // child already reported the error
      return 1;
    }
    return 0;
  }

  // open
  g_autoptr(openslide_t) osr = openslide_open("");
  if (osr == NULL) {
    fprintf(stderr, "Couldn't open synthetic slide\n");
    return 1;
  }
  const char *err = openslide_get_error(osr);
  if (err != NULL) {
    fprintf(stderr, "Opening synthetic slide: %s\n", err);
    return 1;
  }

  // read region
  g_autofree void *buf = g_malloc(4 * 1000 * 100);
  openslide_read_region(osr, buf, 0, 0, 0, 1000, 100);
  err = openslide_get_error(osr);
  if (err != NULL) {
    fprintf(stderr, "Reading region: %s\n", err);
    return 1;
  }

  // report tests
  printf("Tested:\n");
  for (const char *const *prop = openslide_get_property_names(osr);
       *prop != NULL;
       prop++) {
    if (g_str_has_prefix(*prop, "synthetic.item.")) {
      printf("- %s\n", openslide_get_property_value(osr, *prop));
    }
  }

  return 0;
}
