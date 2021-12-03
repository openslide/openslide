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

#ifndef OPENSLIDE_OPENSLIDE_DECODE_ZIP_H_
#define OPENSLIDE_OPENSLIDE_DECODE_ZIP_H_

#include "openslide-private.h"

#include <glib.h>
#include <zip.h>

// opens zip archive by file name
zip_t* _openslide_zip_open_archive(const char *filename, GError **err);

// opens zip archive from "zip source"
zip_t* _openslide_zip_open_archive_from_source(zip_source_t *zs, GError **err);

// closes zip archive and sets handle to NULL
bool _openslide_zip_close_archive(zip_t *z);

// searches file by name in archive, checking both backslashes and forward slashes
zip_int64_t _openslide_zip_name_locate(zip_t *z, const char *filename, zip_flags_t flags);

// reads file from zip archive and stores unpacked data in a buffer
bool _openslide_zip_read_file_data(zip_t *z, zip_uint64_t index, gpointer *file_buf, gsize *file_len, GError **err);

#endif //OPENSLIDE_OPENSLIDE_DECODE_ZIP_H_
