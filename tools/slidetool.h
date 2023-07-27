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

#ifndef OPENSLIDE_SLIDETOOL_H_
#define OPENSLIDE_SLIDETOOL_H_

#include <stdio.h>
#include <glib.h>

struct command {
  // subcommand name
  const char *name;
  // description of positional parameters
  const char *parameter_string;
  // short description for subcommand list
  const char *summary;
  // long description for help
  const char *description;
  const GOptionEntry *options;
  const struct command *subcommands;
  // replace the current command with this one
  const struct command *command;
  int min_positional;
  int max_positional;
  int (*handler)(int narg, char **args);
};

extern const GOptionEntry legacy_opts[];

extern const struct command assoc_cmd;
extern const struct command assoc_icc_cmd;
extern const struct command prop_cmd;
extern const struct command region_cmd;
extern const struct command region_icc_cmd;
extern const struct command slide_cmd;

extern const struct command quickhash1sum_cmd;
extern const struct command show_properties_cmd;
extern const struct command write_png_cmd;

struct output {
  FILE *fp;
};
typedef struct output output;
struct output open_output(const char *filename);
void _close_output(struct output *out);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(output, _close_output);

#endif
