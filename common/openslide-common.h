/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
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

#ifndef OPENSLIDE_COMMON_H
#define OPENSLIDE_COMMON_H

#include <stdbool.h>
#include <glib.h>

// cmdline

struct common_usage_info {
  const char *parameter_string;
  const char *summary;
};

void common_parse_commandline(const struct common_usage_info *info,
                              int *argc, char ***argv);

void common_usage(const struct common_usage_info *info) G_GNUC_NORETURN;

bool common_fix_argv(int *argc, char ***argv);

// fd

char *common_get_fd_path(int fd);

#endif
