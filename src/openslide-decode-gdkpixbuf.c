/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
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

#include "openslide-private.h"
#include "openslide-decode-gdkpixbuf.h"

#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

// Image loading via gdk-pixbuf.  Intended for formats directly supported by
// gdk-pixbuf (BMP, PNM, etc.).  For formats that gdk-pixbuf supports via a
// separate image library, it's more efficient and flexible to create a
// decoder that uses that library directly.

#define BUFSIZE (64 << 10)

struct gdkpixbuf_ctx {
  GdkPixbufLoader *loader;
  int32_t w;
  int32_t h;
  GdkPixbuf *pixbuf;  // NULL until validated, then a borrowed ref
  GError *err;        // from area_prepared
};

// Validate image size and format.  There's no point connecting to the
// size-prepared signal, since we only have a chance to stop the load
// after every BUFSIZE bytes, and we'll likely receive both signals while
// processing the first buffer.
static void area_prepared(GdkPixbufLoader *loader, void *data) {
  struct gdkpixbuf_ctx *ctx = data;

  if (ctx->err) {
    return;
  }

  GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);

  // validate image parameters
  // when adding RGBA support, note that gdk-pixbuf does not
  // premultiply alpha
  if (gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB ||
      gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
      gdk_pixbuf_get_has_alpha(pixbuf) ||
      gdk_pixbuf_get_n_channels(pixbuf) != 3) {
    g_set_error(&ctx->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported pixbuf parameters");
    return;
  }
  int w = gdk_pixbuf_get_width(pixbuf);
  int h = gdk_pixbuf_get_height(pixbuf);
  if (w != ctx->w || h != ctx->h) {
    g_set_error(&ctx->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Dimensional mismatch reading pixbuf: "
                "expected %dx%d, found %dx%d", ctx->w, ctx->h, w, h);
    return;
  }

  // commit
  ctx->pixbuf = pixbuf;
}

static void gdkpixbuf_ctx_free(struct gdkpixbuf_ctx *ctx) {
  if (ctx->loader) {
    gdk_pixbuf_loader_close(ctx->loader, NULL);
    g_object_unref(ctx->loader);
  }
  g_clear_error(&ctx->err);
  g_slice_free(struct gdkpixbuf_ctx, ctx);
}

typedef struct gdkpixbuf_ctx gdkpixbuf_ctx;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(gdkpixbuf_ctx, gdkpixbuf_ctx_free)

// Fix up an existing gdk-pixbuf error in *err, or possibly propagate a new
// one.  Return false in the latter case.  Unlike the typical GError rules,
// *err may be non-NULL.
static bool gdkpixbuf_ctx_check_error(struct gdkpixbuf_ctx *ctx,
                                      GError **err) {
  g_prefix_error(err, "gdk-pixbuf error: ");
  if (ctx->err) {
    // error from area_prepared takes precedence
    g_clear_error(err);
    g_propagate_error(err, g_steal_pointer(&ctx->err));
    return false;
  }
  return true;
}

static struct gdkpixbuf_ctx *gdkpixbuf_ctx_new(const char *format,
                                               int32_t w, int32_t h,
                                               GError **err) {
  g_autoptr(gdkpixbuf_ctx) ctx = g_slice_new0(struct gdkpixbuf_ctx);
  ctx->w = w;
  ctx->h = h;

  ctx->loader = gdk_pixbuf_loader_new_with_type(format, err);
  if (!ctx->loader) {
    return NULL;
  }
  g_signal_connect(ctx->loader,
                   "area-prepared", G_CALLBACK(area_prepared), ctx);

  return g_steal_pointer(&ctx);
}

static bool gdkpixbuf_read(const char *format,
                           size_t (*read_callback)(void *out, void *in, size_t size),
                           void *callback_data,
                           uint64_t length,
                           uint32_t *dest,
                           int32_t w, int32_t h,
                           GError **err) {
  // create loader
  g_autoptr(gdkpixbuf_ctx) ctx = gdkpixbuf_ctx_new(format, w, h, err);
  if (!ctx) {
    return false;
  }

  // read data
  g_auto(_openslide_slice) box = _openslide_slice_alloc(BUFSIZE);
  while (length) {
    size_t count = read_callback(box.p, callback_data, MIN(length, box.len));
    if (!count) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Short read loading pixbuf");
      return false;
    }
    if (!gdk_pixbuf_loader_write(ctx->loader, box.p, count, err)) {
      gdkpixbuf_ctx_check_error(ctx, err);
      return false;
    }
    if (!gdkpixbuf_ctx_check_error(ctx, err)) {
      return false;
    }
    length -= count;
  }

  // finish load
  if (!gdk_pixbuf_loader_close(ctx->loader, err)) {
    gdkpixbuf_ctx_check_error(ctx, err);
    return false;
  }
  if (!gdkpixbuf_ctx_check_error(ctx, err)) {
    return false;
  }
  g_assert(ctx->pixbuf);

  // copy pixels
  uint8_t *pixels = gdk_pixbuf_get_pixels(ctx->pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(ctx->pixbuf);
  for (int32_t y = 0; y < h; y++) {
    for (int32_t x = 0; x < w; x++) {
      dest[y * w + x] = 0xFF000000 |                              // A
                        pixels[y * rowstride + x * 3 + 0] << 16 | // R
                        pixels[y * rowstride + x * 3 + 1] << 8 |  // G
                        pixels[y * rowstride + x * 3 + 2];        // B
    }
  }

  return true;
}

static size_t file_read_callback(void *out, void *in, size_t size) {
  return _openslide_fread(in, out, size);
}

bool _openslide_gdkpixbuf_read(const char *format,
                               const char *filename,
                               int64_t offset,
                               int64_t length,
                               uint32_t *dest,
                               int32_t w, int32_t h,
                               GError **err) {
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }
  if (!_openslide_fseek(f, offset, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't fseek %s: ", filename);
    return false;
  }
  return gdkpixbuf_read(format, file_read_callback, f, length,
                        dest, w, h, err);
}

struct mem {
  const uint8_t *buf;
  size_t off;
  size_t len;
};

static size_t mem_read_callback(void *out, void *in, size_t size) {
  struct mem *mem = in;
  size_t count = MIN(size, mem->len - mem->off);
  memcpy(out, mem->buf + mem->off, count);
  mem->off += count;
  return count;
}

bool _openslide_gdkpixbuf_decode_buffer(const char *format,
                                        const void *buf,
                                        int64_t length,
                                        uint32_t *dest,
                                        int32_t w, int32_t h,
                                        GError **err) {
  struct mem mem = {
    .buf = buf,
    .off = 0,
    .len = length,
  };
  return gdkpixbuf_read(format, mem_read_callback, &mem, length,
                        dest, w, h, err);
}
