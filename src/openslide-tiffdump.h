/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
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

#include <stdio.h>
#include <glib.h>
#include <tiffio.h>

// constructor
struct _openslide_tiffdump *_openslide_tiffdump_create(FILE *f, GError **err);

// destructor
void _openslide_tiffdump_destroy(struct _openslide_tiffdump *tiffdump);

// helpful printout?
void _openslide_tiffdump_print(struct _openslide_tiffdump *tiffdump);

// accessors
int64_t _openslide_tiffdump_get_directory_count(struct _openslide_tiffdump *tiffdump);

int64_t _openslide_tiffdump_get_value_count(struct _openslide_tiffdump *tiffdump,
                                            int64_t dir, int32_t tag);

// accessors only set *ok on failure

// TIFF_BYTE, TIFF_SHORT, TIFF_LONG, TIFF_IFD
uint64_t _openslide_tiffdump_get_uint(struct _openslide_tiffdump *tiffdump,
                                      int64_t dir, int32_t tag, int64_t i,
                                      bool *ok);

// TIFF_SBYTE, TIFF_SSHORT, TIFF_SLONG
int64_t _openslide_tiffdump_get_sint(struct _openslide_tiffdump *tiffdump,
                                     int64_t dir, int32_t tag, int64_t i,
                                     bool *ok);

// TIFF_FLOAT, TIFF_DOUBLE, TIFF_RATIONAL, TIFF_SRATIONAL
double _openslide_tiffdump_get_float(struct _openslide_tiffdump *tiffdump,
                                     int64_t dir, int32_t tag, int64_t i,
                                     bool *ok);

// TIFF_ASCII, TIFF_UNDEFINED
const void *_openslide_tiffdump_get_buffer(struct _openslide_tiffdump *tiffdump,
                                           int64_t dir, int32_t tag);

#endif
