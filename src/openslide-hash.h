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

#ifndef OPENSLIDE_OPENSLIDE_HASH_H_
#define OPENSLIDE_OPENSLIDE_HASH_H_

#include "openslide.h"

#include <stdbool.h>
#include <stdint.h>
#include <tiffio.h>
#include <glib.h>

void _openslide_hash_tiff_tiles(GChecksum *checksum, TIFF *tiff);
void _openslide_hash_string(GChecksum *checksum, const char *str);
void _openslide_hash_file(GChecksum *checksum, const char *filename);
void _openslide_hash_file_part(GChecksum *checksum, const char *filename,
			       int64_t offset, int size);

#endif
