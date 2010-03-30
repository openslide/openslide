/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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

#ifndef OPENSLIDE_OPENSLIDE_TIFFDUMP_H_
#define OPENSLIDE_OPENSLIDE_TIFFDUMP_H_

#include <config.h>

#include <glib.h>
#include <tiffio.h>

struct _openslide_tiffdump_item {
  TIFFDataType type;
  int64_t count;
  void *value;
};

/* returns list of hashtables of (int -> struct _openslide_tiffdump) */
GSList *_openslide_tiffdump_create(FILE *f);

void _openslide_tiffdump_destroy(GSList *tiffdump);

#endif
