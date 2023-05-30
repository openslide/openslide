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

#include <string.h>
#include <glib.h>
#include "openslide-common.h"
#include "slidetool.h"

static char *get_progname(void) {
  const char *prgname = g_get_prgname();
#ifdef _WIN32
  size_t len = strlen(prgname);
  char *ret = g_ascii_strdown(prgname, len);
  if (g_str_has_suffix(ret, ".exe")) {
    ret[len - 4] = 0;
  }
  return ret;
#else
  return g_strdup(prgname);
#endif
}

int main(int argc, char **argv) {
  // properly handle Unicode arguments on Windows; set prgname
  common_fix_argv(&argc, &argv);
  g_autofree char *cmd_name = get_progname();
  if (g_str_equal(cmd_name, "openslide-quickhash1sum")) {
    return do_quickhash1sum(argc, argv);
  } else if (g_str_equal(cmd_name, "openslide-write-png")) {
    return do_write_png(argc, argv);
  } else {
    return do_show_properties(argc, argv);
  }
}
