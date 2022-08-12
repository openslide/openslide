/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2022 Benjamin Gilbert
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

struct png_ctx {
  png_struct *png;
  png_info *info;
  png_byte **rows;
  int64_t h;
  jmp_buf env;
  GError *err;
};

static void warning_callback(png_struct *png G_GNUC_UNUSED,
                             const char *message G_GNUC_UNUSED) {
  //g_debug("%s", message);
}

static void error_callback(png_struct *png, const char *message) {
  struct png_ctx *ctx = png_get_error_ptr(png);
  g_set_error(&ctx->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "PNG error: %s", message);
  longjmp(ctx->env, 1);
}

static void png_ctx_free(struct png_ctx *ctx) {
  png_destroy_read_struct(&ctx->png, &ctx->info, NULL);
  g_slice_free1(ctx->h * sizeof(*ctx->rows), ctx->rows);
  g_slice_free(struct png_ctx, ctx);
}

// volatile pointer, to ensure clang doesn't incorrectly optimize field
// accesses after setjmp() returns again in the function allocating the struct
// https://github.com/llvm/llvm-project/issues/57110
// also avoids setjmp clobber warnings in GCC 12.1.1
typedef struct png_ctx * volatile png_ctx;
G_DEFINE_AUTO_CLEANUP_FREE_FUNC(png_ctx, png_ctx_free, NULL)

static struct png_ctx *png_ctx_new(uint32_t *dest,
                                   int64_t w, int64_t h,
                                   GError **err) {
  g_auto(png_ctx) ctx = g_slice_new0(struct png_ctx);

  // allocate row pointers
  ctx->rows = g_slice_alloc(h * sizeof(*ctx->rows));
  ctx->h = h;
  for (int64_t y = 0; y < h; y++) {
    ctx->rows[y] = (png_byte *) &dest[y * w];
  }

  // init libpng
  ctx->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, ctx,
                                    error_callback, warning_callback);
  if (!ctx->png) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't initialize libpng");
    return NULL;
  }
  ctx->info = png_create_info_struct(ctx->png);
  if (!ctx->info) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't initialize PNG info");
    return NULL;
  }

  // avoid g_steal_pointer because of volatile
  struct png_ctx *ret = ctx;
  ctx = NULL;
  return ret;
}

static bool png_read(png_rw_ptr read_callback, void *callback_data,
                     uint32_t *dest, int64_t w, int64_t h,
                     GError **err) {
  // allocate context
  g_auto(png_ctx) ctx = png_ctx_new(dest, w, h, err);
  if (ctx == NULL) {
    return false;
  }

  if (!setjmp(ctx->env)) {
    // We can't use png_init_io(): passing FILE * between libraries isn't
    // safe on Windows
    png_set_read_fn(ctx->png, callback_data, read_callback);

    // read header
    png_read_info(ctx->png, ctx->info);
    int64_t width = png_get_image_width(ctx->png, ctx->info);
    int64_t height = png_get_image_height(ctx->png, ctx->info);
    if (width != w || height != h) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Dimensional mismatch reading PNG: "
                  "expected %"PRId64"x%"PRId64", found %"PRId64"x%"PRId64,
                  w, h, width, height);
      return false;
    }

    // downsample 16 bits/channel to 8
    #ifdef PNG_READ_SCALE_16_TO_8_SUPPORTED
      png_set_scale_16(ctx->png);
    #else
      // less-accurate fallback
      png_set_strip_16(ctx->png);
    #endif
    // expand to 24-bit RGB or 8-bit gray
    png_set_expand(ctx->png);
    // expand gray to 24-bit RGB
    png_set_gray_to_rgb(ctx->png);
    // libpng emits bytes, but we need words, so byte order matters
    if (G_BYTE_ORDER == G_LITTLE_ENDIAN) {
      // need BGRA
      // RGB -> BGR, RGBA -> BGRA
      png_set_bgr(ctx->png);
      // BGR -> BGRx (BGR + filler)
      png_set_filler(ctx->png, 0xff, PNG_FILLER_AFTER);
    } else {
      // need ARGB
      // RGBA -> ARGB
      png_set_swap_alpha(ctx->png);
      // RGB -> xRGB (filler + RGB)
      png_set_filler(ctx->png, 0xff, PNG_FILLER_BEFORE);
    }

    // check buffer size
    png_read_update_info(ctx->png, ctx->info);
    uint32_t rowbytes = png_get_rowbytes(ctx->png, ctx->info);
    if (rowbytes != w * sizeof(*dest)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unexpected bufsize %u for %"PRId64" pixels",
                  rowbytes, w);
      return false;
    }

    // alpha channel is not supported
    // When adding support for PNGs with alpha, we will need to premultiply
    // the RGB channels.  libpng >= 1.5.4 supports premultiplied alpha via
    // png_set_alpha_mode().
    int color_type = png_get_color_type(ctx->png, ctx->info);
    if (color_type != PNG_COLOR_TYPE_RGB) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unsupported color type %d", color_type);
      return false;
    }

    // read image
    png_read_image(ctx->png, ctx->rows);

    // finish
    png_read_end(ctx->png, NULL);
  } else {
    // setjmp returned again
    g_propagate_error(err, ctx->err);
    return false;
  }

  return true;
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
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }
  if (!_openslide_fseek(f, offset, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't fseek %s: ", filename);
    return false;
  }
  return png_read(file_read_callback, f, dest, w, h, err);
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
