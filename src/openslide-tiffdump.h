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

// returns list of hashtables of (int -> struct _openslide_tiffdump)
GSList *_openslide_tiffdump_create(FILE *f, GError **err);

// destructor
void _openslide_tiffdump_destroy(GSList *tiffdump);

// helpful printout?
void _openslide_tiffdump_print(GSList *tiffdump);

// accessors
// TIFF_BYTE
uint8_t _openslide_tiffdump_get_byte(struct _openslide_tiffdump_item *item,
				     int64_t i);

// TIFF_ASCII
const char *_openslide_tiffdump_get_ascii(struct _openslide_tiffdump_item *item);

// TIFF_SHORT
uint16_t _openslide_tiffdump_get_short(struct _openslide_tiffdump_item *item,
				       int64_t i);

// TIFF_LONG
uint32_t _openslide_tiffdump_get_long(struct _openslide_tiffdump_item *item,
				      int64_t i);

// TIFF_RATIONAL
double _openslide_tiffdump_get_rational(struct _openslide_tiffdump_item *item,
					int64_t i);

// TIFF_SBYTE
int8_t _openslide_tiffdump_get_sbyte(struct _openslide_tiffdump_item *item,
				     int64_t i);

// TIFF_UNDEFINED
uint8_t _openslide_tiffdump_get_undefined(struct _openslide_tiffdump_item *item,
					  int64_t i);

// TIFF_SSHORT
int16_t _openslide_tiffdump_get_sshort(struct _openslide_tiffdump_item *item,
				       int64_t i);

// TIFF_SLONG
int32_t _openslide_tiffdump_get_slong(struct _openslide_tiffdump_item *item,
				      int64_t i);

// TIFF_SRATIONAL
double _openslide_tiffdump_get_srational(struct _openslide_tiffdump_item *item,
					 int64_t i);

// TIFF_FLOAT
float _openslide_tiffdump_get_float(struct _openslide_tiffdump_item *item,
				    int64_t i);

// TIFF_DOUBLE
double _openslide_tiffdump_get_double(struct _openslide_tiffdump_item *item,
				      int64_t i);

// TIFF_IFD
int64_t _openslide_tiffdump_get_ifd(struct _openslide_tiffdump_item *item,
				    int64_t i);

#endif
