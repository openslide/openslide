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

#include <stdbool.h>
#include <stdio.h>
#include <glib.h>
#include "openslide.h"
#include "openslide-common.h"

static bool process(const char *file, int successes, int total) {
  g_autoptr(openslide_t) osr = openslide_open(file);
  if (osr == NULL) {
    fprintf(stderr, "%s: %s: Not a file that OpenSlide can recognize\n",
	    g_get_prgname(), file);
    fflush(stderr);
    return false;
  }

  const char *err = openslide_get_error(osr);
  if (err) {
    fprintf(stderr, "%s: %s: %s\n", g_get_prgname(), file, err);
    fflush(stderr);
    return false;
  }

  // print header
  if (successes > 0) {
    printf("\n");
  }
  if (total > 1) {
    // format inspired by head(1)/tail(1)
    printf("==> %s <==\n", file);
  }

  // read properties
  const char * const *property_names = openslide_get_property_names(osr);
  while (*property_names) {
    const char *name = *property_names;
    const char *value = openslide_get_property_value(osr, name);
    printf("%s: '%s'\n", name, value);

    property_names++;
  }

  return true;
}


static const struct common_usage_info usage_info = {
  "FILE...",
  "Print OpenSlide properties for a slide.",
};

int main (int argc, char **argv) {
  common_parse_commandline(&usage_info, &argc, &argv);
  if (argc < 2) {
    common_usage(&usage_info);
  }

  int successes = 0;
  for (int i = 1; i < argc; i++) {
    if (process(argv[i], successes, argc - 1)) {
      successes++;
    }
  }

  return successes != argc - 1;
}
