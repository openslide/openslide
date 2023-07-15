/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
 *  Copyright (c) 2023      Benjamin Gilbert
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

static gboolean names_only;

static bool list_props(const char *file, int successes, int total,
                       bool values) {
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
    if (values) {
      const char *value = openslide_get_property_value(osr, name);
      printf("%s: '%s'\n", name, value);
    } else {
      printf("%s\n", name);
    }

    property_names++;
  }

  return true;
}

static int do_show_properties(int narg, char **args) {
  int successes = 0;
  for (int i = 0; i < narg; i++) {
    if (list_props(args[i], successes, narg, true)) {
      successes++;
    }
  }
  return successes != narg;
}

static int do_prop_list(int narg, char **args) {
  int successes = 0;
  for (int i = 0; i < narg; i++) {
    if (list_props(args[i], successes, narg, !names_only)) {
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
  .parameter_string = "<FILE...>",
  .description = "Print OpenSlide properties for a slide.",
  .options = legacy_opts,
  .min_positional = 1,
  .handler = do_show_properties,
};

static const GOptionEntry prop_list_opts[] = {
  {"names", 0, 0, G_OPTION_ARG_NONE, &names_only, "Omit property values", NULL},
  {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

static const struct command prop_subcmds[] = {
  {
    .name = "list",
    .parameter_string = "<FILE...>",
    .summary = "List slide properties",
    .description = "Print OpenSlide properties for a slide.",
    .options = prop_list_opts,
    .min_positional = 1,
    .handler = do_prop_list,
  },
  {}
};

const struct command prop_cmd = {
  .name = "prop",
  .summary = "Commands related to properties",
  .subcommands = prop_subcmds,
};

const struct command quickhash1sum_cmd = {
  .parameter_string = "<FILE...>",
  .description = "Print OpenSlide quickhash-1 (256-bit) checksums.",
  .options = legacy_opts,
  .min_positional = 1,
  .handler = do_quickhash1sum,
};

const struct command quickhash1_cmd = {
  .name = "quickhash1",
  .parameter_string = "<FILE...>",
  .summary = "Print quickhash-1 checksums",
  .description = "Print OpenSlide quickhash-1 (256-bit) checksums.",
  .min_positional = 1,
  .handler = do_quickhash1sum,
};
