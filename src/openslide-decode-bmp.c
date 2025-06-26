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

#include "openslide-private.h"
#include "openslide-decode-bmp.h"

#include <glib.h>

struct bmp_file_hdr {
  uint16_t magic;
  uint32_t file_size;
  uint32_t _reserved;
  uint32_t pixel_off;
} __attribute__((__packed__));

struct bmp_dib_hdr {
  uint32_t hdr_size;
  int32_t width;
  int32_t height;
  uint16_t planes;
  uint16_t depth;
  uint32_t compression;
  uint32_t data_size;
  int32_t _ppm_x;
  int32_t _ppm_y;
  uint32_t palette_colors;
  uint32_t _palette_important;
} __attribute__((__packed__));

#define BMP_MAGIC 0x4d42
#define BMP_FHDR_SIZE (sizeof(struct bmp_file_hdr))
#define BMP_DHDR_SIZE (sizeof(struct bmp_dib_hdr))
#define BMP_PLANES 1
#define BMP_DEPTH 24
#define BMP_COMPRESSION_RGB 0

struct bmp_io {
  bool (*read)(struct bmp_io *io, void *buf, size_t size, GError **err);
  const uint8_t *(*read_direct)(struct bmp_io *io, void *buf, size_t size,
                                GError **err);
  bool (*seek)(struct bmp_io *io, off_t off, GError **err);
  void *obj;
  int64_t off;
  int64_t len;
};

static int64_t bmp_row_bytes(int32_t w) {
  int64_t bytes = w * (BMP_DEPTH / 8);
  return !(bytes % 4) ? bytes : bytes + (4 - bytes % 4);
}

static bool bmp_validate_fhdr(const struct bmp_file_hdr *fhdr,
                              int32_t w, int32_t h,
                              uint32_t *pixel_off,
                              GError **err) {
  if (GUINT16_TO_LE(fhdr->magic) != BMP_MAGIC) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Bad BMP magic number");
    return false;
  }
  uint32_t file_size = GUINT32_TO_LE(fhdr->file_size);
  if (file_size < BMP_FHDR_SIZE + BMP_DHDR_SIZE + bmp_row_bytes(w) * h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Bad BMP file size %u", file_size);
    return false;
  }
  *pixel_off = GUINT32_TO_LE(fhdr->pixel_off);
  if (*pixel_off < BMP_FHDR_SIZE + BMP_DHDR_SIZE ||
      *pixel_off + bmp_row_bytes(w) * h > file_size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Bad BMP pixel offset %u", *pixel_off);
    return false;
  }
  return true;
}

static bool bmp_validate_dhdr(const struct bmp_dib_hdr *dhdr,
                              int32_t w, int32_t h,
                              GError **err) {
  uint32_t hdr_size = GUINT32_TO_LE(dhdr->hdr_size);
  if (hdr_size != BMP_DHDR_SIZE) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported BMP DIB header size %u", hdr_size);
    return false;
  }
  int32_t width = GINT32_TO_LE(dhdr->width);
  int32_t height = GINT32_TO_LE(dhdr->height);
  if (width != w || height != h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected BMP size %dx%d, expected %dx%d", width, height, w, h);
    return false;
  }
  uint16_t planes = GUINT16_TO_LE(dhdr->planes);
  if (planes != BMP_PLANES) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported BMP planes %u", planes);
    return false;
  }
  uint16_t depth = GUINT16_TO_LE(dhdr->depth);
  if (depth != BMP_DEPTH) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported BMP depth %u", depth);
    return false;
  }
  uint32_t compression = GUINT32_TO_LE(dhdr->compression);
  if (compression != BMP_COMPRESSION_RGB) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported BMP compression %u", compression);
    return false;
  }
  uint32_t data_size = GUINT32_TO_LE(dhdr->data_size);
  if (data_size && data_size != bmp_row_bytes(w) * h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Bad BMP data size %u", data_size);
    return false;
  }
  uint32_t palette_colors = GUINT32_TO_LE(dhdr->palette_colors);
  if (palette_colors > 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported BMP palette colors %u", palette_colors);
    return false;
  }
  return true;
}

static bool bmp_read(struct bmp_io *io, uint32_t *dest,
                     int32_t w, int32_t h, GError **err) {
  if (!io->seek(io, 0, err)) {
    return false;
  }

  struct bmp_file_hdr fhdr;
  if (!io->read(io, &fhdr, sizeof(fhdr), err)) {
    g_prefix_error(err, "Reading BMP header: ");
    return false;
  }
  uint32_t pixel_off;
  if (!bmp_validate_fhdr(&fhdr, w, h, &pixel_off, err)) {
    return false;
  }

  struct bmp_dib_hdr dhdr;
  if (!io->read(io, &dhdr, sizeof(dhdr), err)) {
    g_prefix_error(err, "Reading BMP DIB header: ");
    return false;
  }
  if (!bmp_validate_dhdr(&dhdr, w, h, err)) {
    return false;
  }

  if (!io->seek(io, pixel_off, err)) {
    return false;
  }
  int64_t bufsize = bmp_row_bytes(w);
  g_autofree void *buf = g_malloc(bufsize);
  for (int32_t y = h - 1; y >= 0; y--) {
    uint32_t *dp = dest + w * y;
    const uint8_t *sp = io->read_direct(io, buf, bufsize, err);
    if (!sp) {
      g_prefix_error(err, "Reading BMP pixel data: ");
      return false;
    }
    for (int32_t x = 0; x < w; x++) {
      *dp++ = 0xff000000 | *(sp + 2) << 16 | *(sp + 1) << 8 | *sp;
      sp += 3;
    }
  }
  return true;
}

static bool bmp_file_read(struct bmp_io *io, void *buf, size_t size,
                          GError **err) {
  return _openslide_fread_exact(io->obj, buf, size, err);
}

static const uint8_t *bmp_file_read_direct(struct bmp_io *io,
                                           void *buf, size_t size,
                                           GError **err) {
  if (!bmp_file_read(io, buf, size, err)) {
    return NULL;
  }
  return buf;
}

static bool bmp_file_seek(struct bmp_io *io, off_t off, GError **err) {
  if (!_openslide_fseek(io->obj, io->off + off, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to offset %"PRId64": ", io->off + off);
    return false;
  }
  return true;
}

bool _openslide_bmp_read_file(struct _openslide_file *f,
                              int64_t offset,
                              uint32_t *dest,
                              int32_t w, int32_t h,
                              GError **err) {
  struct bmp_io io = {
    .read = bmp_file_read,
    .read_direct = bmp_file_read_direct,
    .seek = bmp_file_seek,
    .obj = f,
    .off = offset,
  };
  if (!bmp_read(&io, dest, w, h, err)) {
    g_prefix_error(err, "BMP at offset %"PRId64": ", offset);
    return false;
  }
  return true;
}

static bool bmp_mem_read(struct bmp_io *io, void *buf, size_t size,
                         GError **err) {
  if (io->off < 0 || io->off + (int64_t) size > io->len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Read beyond EOF");
    return false;
  }
  memcpy(buf, io->obj + io->off, size);
  io->off += size;
  return true;
}

static const uint8_t *bmp_mem_read_direct(struct bmp_io *io,
                                          void *buf G_GNUC_UNUSED, size_t size,
                                          GError **err) {
  if (io->off < 0 || io->off + (int64_t) size > io->len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Read beyond EOF");
    return NULL;
  }
  void *ret = io->obj + io->off;
  io->off += size;
  return ret;
}

static bool bmp_mem_seek(struct bmp_io *io, off_t off,
                         GError **err G_GNUC_UNUSED) {
  io->off = off;
  return true;
}

bool _openslide_bmp_decode_buffer(const void *buf,
                                  int64_t length,
                                  uint32_t *dest,
                                  int32_t w, int32_t h,
                                  GError **err) {
  struct bmp_io io = {
    .read = bmp_mem_read,
    .read_direct = bmp_mem_read_direct,
    .seek = bmp_mem_seek,
    .obj = (void *) buf,
    .len = length,
  };
  return bmp_read(&io, dest, w, h, err);
}
