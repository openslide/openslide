/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2025 Benjamin Gilbert
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

#ifndef OPENSLIDE_OPENSLIDE_DECODE_BMP_H_
#define OPENSLIDE_OPENSLIDE_DECODE_BMP_H_

#include "openslide-private.h"

#include <stdint.h>
#include <glib.h>

bool _openslide_bmp_read_file(struct _openslide_file *f,
                              int64_t offset,
                              uint32_t *dest,
                              int32_t w, int32_t h,
                              GError **err);

bool _openslide_bmp_decode_buffer(const void *buf,
                                  int64_t length,
                                  uint32_t *dest,
                                  int32_t w, int32_t h,
                                  GError **err);

#endif
