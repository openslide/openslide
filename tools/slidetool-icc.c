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

#include <stdio.h>
#include <glib.h>
#include <errno.h>
#include "openslide.h"
#include "openslide-common.h"
#include "slidetool.h"

static int do_icc_read(int narg, char **args) {
  g_assert(narg >= 1);
  const char *slide = args[0];
  const char *outfile = narg >= 2 ? args[1] : NULL;

  g_autoptr(openslide_t) osr = openslide_open(slide);
  common_fail_on_error(osr, "%s", slide);

  int64_t icc_size = openslide_get_icc_profile_size(osr);
  if (!icc_size) {
    common_fail("%s: No ICC profile", slide);
  }

  g_autofree void *icc = g_malloc(icc_size);
  openslide_read_icc_profile(osr, icc);
  common_fail_on_error(osr, "%s", slide);

  g_auto(output) out = open_output(outfile);
  if (fwrite(icc, icc_size, 1, out.fp) < 1) {
    common_fail("Can't write %s: %s", outfile, strerror(errno));
  }

  return 0;
}

static int do_assoc_icc_read(int narg, char **args) {
  g_assert(narg >= 2);
  const char *slide = args[0];
  const char *name = args[1];
  const char *outfile = narg >= 3 ? args[2] : NULL;

  g_autoptr(openslide_t) osr = openslide_open(slide);
  common_fail_on_error(osr, "%s", slide);

  int64_t icc_size = openslide_get_associated_image_icc_profile_size(osr, name);
  if (icc_size == -1) {
    common_fail("%s: %s: No such associated image", slide, name);
  } else if (!icc_size) {
    common_fail("%s: %s: No ICC profile", slide, name);
  }

  g_autofree void *icc = g_malloc(icc_size);
  openslide_read_associated_image_icc_profile(osr, name, icc);
  common_fail_on_error(osr, "%s: %s", slide, name);

  g_auto(output) out = open_output(outfile);
  if (fwrite(icc, icc_size, 1, out.fp) < 1) {
    common_fail("Can't write %s: %s", outfile, strerror(errno));
  }

  return 0;
}

static const struct command region_icc_subcmds[] = {
  {
    .name = "read",
    .parameter_string = "<FILE> [OUTPUT-FILE]",
    .summary = "Write ICC profile to a file",
    .description = "Copy a slide's ICC profile to a file.",
    .min_positional = 1,
    .max_positional = 2,
    .handler = do_icc_read,
  },
  {}
};

const struct command region_icc_cmd = {
  .name = "icc",
  .summary = "Commands related to slide region ICC profiles",
  .subcommands = region_icc_subcmds,
};

static const struct command assoc_icc_subcmds[] = {
  {
    .name = "read",
    .parameter_string = "<FILE> <NAME> [OUTPUT-FILE]",
    .summary = "Write an associated image ICC profile to a file",
    .description = "Copy an associated image's ICC profile to a file.",
    .min_positional = 2,
    .max_positional = 3,
    .handler = do_assoc_icc_read,
  },
  {}
};

const struct command assoc_icc_cmd = {
  .name = "icc",
  .summary = "Commands related to associated image ICC profiles",
  .subcommands = assoc_icc_subcmds,
};
