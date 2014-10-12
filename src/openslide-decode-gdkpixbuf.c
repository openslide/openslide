/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
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

#include "openslide-private.h"
#include "openslide-decode-gdkpixbuf.h"

#include <stdio.h>
#include <glib.h>
#include <glib-object.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

// Image loading via gdk-pixbuf.  Intended for formats directly supported by
// gdk-pixbuf (BMP, PNM, etc.).  For formats that gdk-pixbuf supports via a
// separate image library, it's more efficient and flexible to create a
// decoder that uses that library directly.

#define BUFSIZE (64 << 10)

struct load_state {
  int32_t w;
  int32_t h;
  GdkPixbuf *pixbuf;  // NULL until validated, then a borrowed ref
  GError *err;
};

// Validate image size and format.  There's no point connecting to the
// size-prepared signal, since we only have a chance to stop the load
// after every BUFSIZE bytes, and we'll likely receive both signals while
// processing the first buffer.
static void area_prepared(GdkPixbufLoader *loader, void *data) {
  struct load_state *state = data;

  if (state->err) {
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
    g_set_error(&state->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported pixbuf parameters");
    return;
  }
  int w = gdk_pixbuf_get_width(pixbuf);
  int h = gdk_pixbuf_get_height(pixbuf);
  if (w != state->w || h != state->h) {
    g_set_error(&state->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Dimensional mismatch reading pixbuf: "
                "expected %dx%d, found %dx%d", state->w, state->h, w, h);
    return;
  }

  // commit
  state->pixbuf = pixbuf;
}

bool _openslide_gdkpixbuf_read(const char *format,
                               const char *filename,
                               int64_t offset,
                               int64_t length,
                               uint32_t *dest,
                               int32_t w, int32_t h,
                               GError **err) {
  GdkPixbufLoader *loader = NULL;
  uint8_t *buf = g_slice_alloc(BUFSIZE);
  bool success = false;
  struct load_state state = {
    .w = w,
    .h = h,
  };

  // open and seek
  FILE *f = _openslide_fopen(filename, "rb", err);
  if (!f) {
    goto DONE;
  }
  if (fseeko(f, offset, SEEK_SET)) {
    _openslide_io_error(err, "Couldn't fseek %s", filename);
    goto DONE;
  }

  // create loader
  loader = gdk_pixbuf_loader_new_with_type(format, err);
  if (!loader) {
    goto DONE;
  }
  g_signal_connect(loader, "area-prepared", G_CALLBACK(area_prepared), &state);

  // read data
  while (length) {
    size_t count = fread(buf, 1, MIN(length, BUFSIZE), f);
    if (!count) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Short read loading pixbuf from %s", filename);
      goto DONE;
    }
    if (!gdk_pixbuf_loader_write(loader, buf, count, err)) {
      g_prefix_error(err, "gdk-pixbuf error: ");
      goto DONE;
    }
    if (state.err) {
      goto DONE;
    }
    length -= count;
  }

  // finish load
  if (!gdk_pixbuf_loader_close(loader, err)) {
    g_prefix_error(err, "gdk-pixbuf error: ");
    goto DONE;
  }
  if (state.err) {
    goto DONE;
  }
  g_assert(state.pixbuf);

  // copy pixels
  uint8_t *pixels = gdk_pixbuf_get_pixels(state.pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(state.pixbuf);
  for (int32_t y = 0; y < h; y++) {
    for (int32_t x = 0; x < w; x++) {
      dest[y * w + x] = 0xFF000000 |                              // A
                        pixels[y * rowstride + x * 3 + 0] << 16 | // R
                        pixels[y * rowstride + x * 3 + 1] << 8 |  // G
                        pixels[y * rowstride + x * 3 + 2];        // B
    }
  }

  success = true;

DONE:
  // clean up
  if (loader) {
    gdk_pixbuf_loader_close(loader, NULL);
    g_object_unref(loader);
  }
  if (f) {
    fclose(f);
  }
  g_slice_free1(BUFSIZE, buf);

  // now that the loader is closed, we know state.err won't be set
  // behind our back
  if (state.err) {
    // signal handler validation errors override GdkPixbuf errors
    g_clear_error(err);
    g_propagate_error(err, state.err);
    // signal handler errors should have been noticed before falling through
    g_assert(!success);
  }
  return success;
}
