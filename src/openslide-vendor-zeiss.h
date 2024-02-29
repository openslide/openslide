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

#define BGR24TOARGB32(p)                                                       \
  (0xFF000000 | (uint32_t)((p)[0]) | ((uint32_t)((p)[1]) << 8) |               \
   ((uint32_t)((p)[2]) << 16))

#define BGR48TOARGB32(p)                                                       \
  (0xFF000000 | (uint32_t)((p)[1]) | ((uint32_t)((p)[3]) << 8) |               \
   ((uint32_t)((p)[5]) << 16))


/* could be shared with JPEG XR decoder in the future */
struct czi_decbuf {
  uint8_t *data;
  uint32_t w;
  uint32_t h;
  size_t size;
  uint32_t stride;
  uint32_t pixel_bits;
};


static bool _openslide_convert_24bppbgr_to_cairo24bpprgb(struct czi_decbuf *p) {
  size_t new_size = p->w * p->h * 4;
  uint32_t *buf = g_slice_alloc(new_size);
  uint32_t *bp = buf;
  size_t i = 0;

  while (i < p->size) {
    *bp++ = BGR24TOARGB32(&p->data[i]);
    i += 3;
  }

  g_slice_free1(p->size, p->data);
  p->stride = p->w * 4;
  p->pixel_bits = 32;
  p->size = new_size;
  p->data = (uint8_t *) buf;
  return true;
}

static bool _openslide_convert_48bppbgr_to_cairo24bpprgb(struct czi_decbuf *p) {
  size_t new_size = p->w * p->h * 4;
  uint32_t *buf = g_slice_alloc(new_size);
  uint32_t *bp = buf;
  size_t i = 0;

  while (i < p->size) {
    *bp++ = BGR48TOARGB32(&p->data[i]);
    i += 6;
  }

  g_slice_free1(p->size, p->data);
  p->stride = p->w * 4;
  p->pixel_bits = 32;
  p->size = new_size;
  p->data = (uint8_t *) buf;
  return true;
}

#endif
