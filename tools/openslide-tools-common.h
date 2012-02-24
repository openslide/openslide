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

#ifndef _OPENSLIDE_TOOLS_COMMON_H
#define _OPENSLIDE_TOOLS_COMMON_H

#include <glib.h>

struct openslide_tools_usage_info {
  const char *parameter_string;
  const char *summary;
};

void _openslide_tools_parse_commandline(const struct openslide_tools_usage_info *info,
                                        int *argc,
                                        char ***argv);

void _openslide_tools_usage(const struct openslide_tools_usage_info *info)
     G_GNUC_NORETURN;

#endif
