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

struct command {
  const char *parameter_string;
  const char *summary;
  int min_positional;
  int max_positional;
  int (*handler)(int narg, char **args);
};

extern const struct command quickhash1sum_cmd;
extern const struct command show_properties_cmd;
extern const struct command write_png_cmd;

#endif
