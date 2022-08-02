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

#ifdef OPENSLIDE_PUBLIC
G_DEFINE_AUTOPTR_CLEANUP_FUNC(openslide_t, openslide_close)
#endif

// cmdline

struct common_usage_info {
  const char *parameter_string;
  const char *summary;
};

void common_fix_argv(int *argc, char ***argv);

bool common_parse_options(GOptionContext *ctx,
                          int *argc, char ***argv,
                          GError **err);

void common_parse_commandline(const struct common_usage_info *info,
                              int *argc, char ***argv);

void common_usage(const struct common_usage_info *info) G_GNUC_NORETURN;

// fail

void common_fail(const char *fmt, ...) G_GNUC_NORETURN;

// fd

char *common_get_fd_path(int fd);

#endif
