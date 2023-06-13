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

#ifndef OPENSLIDE_OPENSLIDE_DECODE_JP2K_H_
#define OPENSLIDE_OPENSLIDE_DECODE_JP2K_H_

#include <stdint.h>
#include <glib.h>

/* JPEG 2000 support */

enum _openslide_jp2k_colorspace {
  OPENSLIDE_JP2K_RGB,
  OPENSLIDE_JP2K_YCBCR,
};

bool _openslide_jp2k_decode_buffer(uint32_t *dest,
                                   int32_t w, int32_t h,
                                   const void *data, int32_t datalen,
                                   enum _openslide_jp2k_colorspace space,
                                   GError **err);

#endif
