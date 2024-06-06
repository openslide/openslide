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

static void set_synthetic_debug_flag(void *arg G_GNUC_UNUSED) {
  putenv("OPENSLIDE_DEBUG=synthetic");
}

int main(int argc, char **argv) {
  common_fix_argv(&argc, &argv);
  if (argc < 2) {
    // OpenSlide already evaluated debug flags, so we need to rerun
    // ourselves.  Do it in a cross-platform way.
    char *child_argv[] = {argv[0], "child", NULL};
    GError *tmp_err = NULL;
    int status;
    if (!g_spawn_sync(NULL, child_argv, NULL, G_SPAWN_SEARCH_PATH,
                      set_synthetic_debug_flag, NULL, NULL, NULL, &status,
                      &tmp_err)) {
      common_fail("Spawning child failed: %s", tmp_err->message);
    }
    if (!g_spawn_check_exit_status(status, NULL)) {
      // child already reported the error
      return 1;
    }
    return 0;
  } else if (!g_str_equal(argv[1], "child")) {
    common_fail("Found unexpected argument");
  }

  // open
  g_autoptr(openslide_t) osr = openslide_open("");
  common_fail_on_error(osr, "Opening synthetic slide");

  // read region
  g_autofree void *buf = g_malloc(4 * 1000 * 100);
  openslide_read_region(osr, buf, 0, 0, 0, 1000, 100);
  common_fail_on_error(osr, "Reading region");

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
