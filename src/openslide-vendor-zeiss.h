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
#ifndef OPENSLIDE_OPENSLIDE_VENDOR_ZEISS_H_
#define OPENSLIDE_OPENSLIDE_VENDOR_ZEISS_H_
#include "openslide-image.h"

/* could be shared with JPEG XR decoder in the future */
struct czi_decbuf {
  uint8_t *data;
  uint32_t w;
  uint32_t h;
  size_t size;
  uint32_t stride;
  uint32_t pixel_bits;
};

static bool czi_bgrn_to_xrgb32(struct czi_decbuf *p, int in_pixel_bits) {
  if (in_pixel_bits != 24 && in_pixel_bits != 48) {
    return false;
  }

  size_t new_size = p->w * p->h * 4;
  g_autofree uint8_t *buf = g_malloc(new_size);
  if (in_pixel_bits == 24) {
     _openslide_bgr24_to_xrgb32(p->data, p->size, buf);
  } else if (in_pixel_bits == 48) {
    _openslide_bgr48_to_xrgb32(p->data, p->size, buf);
  }
  g_free(p->data);
  p->stride = p->w * 4;
  p->pixel_bits = 32;
  p->size = new_size;
  p->data = g_steal_pointer(&buf);
  return true;
}

#endif
