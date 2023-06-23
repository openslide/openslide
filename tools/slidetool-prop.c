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
#include "slidetool.h"

static bool list_props(const char *file, int successes, int total) {
  g_autoptr(openslide_t) osr = openslide_open(file);
  if (common_warn_on_error(osr, "%s", file)) {
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

static int do_show_properties(int narg, char **args) {
  int successes = 0;
  for (int i = 0; i < narg; i++) {
    if (list_props(args[i], successes, narg)) {
      successes++;
    }
  }
  return successes != narg;
}

static bool quickhash1sum(const char *file) {
  g_autoptr(openslide_t) osr = openslide_open(file);
  if (common_warn_on_error(osr, "%s", file)) {
    return false;
  }

  const char *hash =
    openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_QUICKHASH1);
  if (hash == NULL) {
    common_warn("%s: No quickhash-1 available", file);
    return false;
  }

  printf("%s  %s\n", hash, file);
  return true;
}

static int do_quickhash1sum(int narg, char **args) {
  int ret = 0;
  for (int i = 0; i < narg; i++) {
    if (!quickhash1sum(args[i])) {
      ret = 1;
    }
  }
  return ret;
}

const struct command show_properties_cmd = {
  .parameter_string = "FILE...",
  .summary = "Print OpenSlide properties for a slide.",
  .options = legacy_opts,
  .min_positional = 1,
  .handler = do_show_properties,
};

const struct command quickhash1sum_cmd = {
  .parameter_string = "FILE...",
  .summary = "Print OpenSlide quickhash-1 (256-bit) checksums.",
  .options = legacy_opts,
  .min_positional = 1,
  .handler = do_quickhash1sum,
};
