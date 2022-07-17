/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
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

#include <config.h>

// libpng < 1.5 breaks the build if setjmp.h is included before png.h
#include <png.h>

#include "openslide-private.h"
#include "openslide-decode-png.h"

#include <glib.h>
#include <setjmp.h>
#include <stdio.h>

struct png_error_ctx {
  jmp_buf env;
  GError *err;
};

static void warning_callback(png_struct *png G_GNUC_UNUSED,
                             const char *message G_GNUC_UNUSED) {
  //g_debug("%s", message);
}

static void error_callback(png_struct *png, const char *message) {
  struct png_error_ctx *ectx = png_get_error_ptr(png);
  g_set_error(&ectx->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "PNG error: %s", message);
  longjmp(ectx->env, 1);
}

static bool png_read(png_rw_ptr read_callback, void *callback_data,
                     uint32_t *dest, int64_t w, int64_t h,
                     GError **err) {
  png_struct *png = NULL;
  png_info *info = NULL;
  volatile bool success = false;

  // allocate error context
  struct png_error_ctx *ectx = g_slice_new0(struct png_error_ctx);

  // allocate row pointers
  png_byte **rows = g_slice_alloc(h * sizeof(*rows));
  for (int64_t y = 0; y < h; y++) {
    rows[y] = (png_byte *) &dest[y * w];
  }

  // init libpng
  png = png_create_read_struct(PNG_LIBPNG_VER_STRING, ectx,
                               error_callback, warning_callback);
  if (!png) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't initialize libpng");
    goto DONE;
  }
  info = png_create_info_struct(png);
  if (!info) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't initialize PNG info");
    goto DONE;
  }

  if (!setjmp(ectx->env)) {
    // We can't use png_init_io(): passing FILE * between libraries isn't
    // safe on Windows
    png_set_read_fn(png, callback_data, read_callback);

    // read header
    png_read_info(png, info);
    int64_t width = png_get_image_width(png, info);
    int64_t height = png_get_image_height(png, info);
    if (width != w || height != h) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Dimensional mismatch reading PNG: "
                  "expected %"PRId64"x%"PRId64", found %"PRId64"x%"PRId64,
                  w, h, width, height);
      goto DONE;
    }

    // downsample 16 bits/channel to 8
    #ifdef PNG_READ_SCALE_16_TO_8_SUPPORTED
      png_set_scale_16(png);
    #else
      // less-accurate fallback
      png_set_strip_16(png);
    #endif
    // expand to 24-bit RGB or 8-bit gray
    png_set_expand(png);
    // expand gray to 24-bit RGB
    png_set_gray_to_rgb(png);
    // libpng emits bytes, but we need words, so byte order matters
    if (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
      // need BGRA
      // RGB -> BGR, RGBA -> BGRA
      png_set_bgr(png);
      // BGR -> BGRx (BGR + filler)
      png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    } else {
      // need ARGB
      // RGBA -> ARGB
      png_set_swap_alpha(png);
      // RGB -> xRGB (filler + RGB)
      png_set_filler(png, 0xff, PNG_FILLER_BEFORE);
    }

    // check buffer size
    png_read_update_info(png, info);
    uint32_t rowbytes = png_get_rowbytes(png, info);
    if (rowbytes != w * sizeof(*dest)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unexpected bufsize %u for %"PRId64" pixels",
                  rowbytes, w);
      goto DONE;
    }

    // alpha channel is not supported
    // When adding support for PNGs with alpha, we will need to premultiply
    // the RGB channels.  libpng >= 1.5.4 supports premultiplied alpha via
    // png_set_alpha_mode().
    int color_type = png_get_color_type(png, info);
    if (color_type != PNG_COLOR_TYPE_RGB) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unsupported color type %d", color_type);
      goto DONE;
    }

    // read image
    png_read_image(png, rows);

    // finish
    png_read_end(png, NULL);

    success = true;
  } else {
    // setjmp returned again
    g_propagate_error(err, ectx->err);
  }

DONE:
  png_destroy_read_struct(&png, &info, NULL);
  g_slice_free1(h * sizeof(*rows), rows);
  g_slice_free(struct png_error_ctx, ectx);
  return success;
}

static void file_read_callback(png_struct *png, png_byte *buf, png_size_t len) {
  struct _openslide_file *f = png_get_io_ptr(png);
  if (_openslide_fread(f, buf, len) != len) {
    png_error(png, "Read failed");
  }
}

bool _openslide_png_read(const char *filename,
                         int64_t offset,
                         uint32_t *dest,
                         int64_t w, int64_t h,
                         GError **err) {
  struct _openslide_file *f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }
  if (_openslide_fseek(f, offset, SEEK_SET)) {
    _openslide_io_error(err, "Couldn't fseek %s", filename);
    _openslide_fclose(f);
    return false;
  }
  bool ret = png_read(file_read_callback, f, dest, w, h, err);
  _openslide_fclose(f);
  return ret;
}

struct mem {
  const uint8_t *buf;
  png_size_t off;
  png_size_t len;
};

static void mem_read_callback(png_struct *png, png_byte *buf, png_size_t len) {
  struct mem *mem = png_get_io_ptr(png);
  if (mem->len - mem->off >= len) {
    memcpy(buf, mem->buf + mem->off, len);
    mem->off += len;
  } else {
    png_error(png, "Read past end of buffer");
  }
}

bool _openslide_png_decode_buffer(const void *buf,
                                  int64_t length,
                                  uint32_t *dest,
                                  int64_t w, int64_t h,
                                  GError **err) {
  struct mem mem = {
    .buf = buf,
    .off = 0,
    .len = length,
  };
  return png_read(mem_read_callback, &mem, dest, w, h, err);
}
