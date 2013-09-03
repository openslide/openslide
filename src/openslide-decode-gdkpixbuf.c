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

#include "openslide-private.h"

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

// Image loading via gdk-pixbuf.  Intended for formats directly supported by
// gdk-pixbuf (BMP, PNM, etc.).  For formats that gdk-pixbuf supports via a
// separate image library, it's more efficient and flexible to create a
// decoder that uses that library directly.

bool _openslide_gdkpixbuf_read(const char *filename,
                               int64_t offset,
                               uint32_t *dest,
                               int32_t w, int32_t h,
                               GError **err) {
  GFileInputStream *input = NULL;
  GInputStream *buffered = NULL;
  GdkPixbuf *pixbuf = NULL;
  bool success = false;

  // open
  GFile *file = g_file_new_for_path(filename);
  input = g_file_read(file, NULL, err);
  g_object_unref(file);
  if (!input) {
    goto DONE;
  }

  // seek
  if (!g_seekable_seek(G_SEEKABLE(input), offset, G_SEEK_SET, NULL, err)) {
    goto DONE;
  }

  // wrap in buffer
  buffered = g_buffered_input_stream_new(G_INPUT_STREAM(input));

  // load
  pixbuf = gdk_pixbuf_new_from_stream(buffered, NULL, err);
  if (!pixbuf) {
    g_prefix_error(err, "gdk-pixbuf error: ");
    goto DONE;
  }

  // validate image parameters
  // when adding RGBA support, note that gdk-pixbuf does not
  // premultiply alpha
  if (gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB ||
      gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
      gdk_pixbuf_get_has_alpha(pixbuf) ||
      gdk_pixbuf_get_n_channels(pixbuf) != 3) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Unsupported pixbuf parameters");
    goto DONE;
  }
  int width = gdk_pixbuf_get_width(pixbuf);
  int height = gdk_pixbuf_get_height(pixbuf);
  if (w != width || h != height) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Dimensional mismatch reading pixbuf: "
                "expected %dx%d, found %dx%d", w, h, width, height);
    goto DONE;
  }

  // copy pixels
  uint8_t *pixels = gdk_pixbuf_get_pixels(pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
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
  if (pixbuf) {
    g_object_unref(pixbuf);
  }
  if (buffered) {
    g_object_unref(buffered);
  }
  if (input) {
    g_object_unref(input);
  }
  return success;
}
