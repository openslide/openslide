/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2015 Benjamin Gilbert
 *  Copyright (c) 2026 Aaron Boxer
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
#include <string.h>

#include "openslide-private.h"
#include "openslide-decode-jp2k.h"

#include <grok.h>

static void grok_error_callback(const char *msg, void *user_data) {
  GError **err = (GError **) user_data;
  if (err && !*err) {
    g_autofree char *detail = g_strdup(msg);
    g_strchomp(detail);
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Grok error: %s", detail);
  }
}

static void grok_warning_callback(const char *msg,
                                  void *user_data G_GNUC_UNUSED) {
  if (_openslide_debug(OPENSLIDE_DEBUG_DECODING)) {
    g_autofree char *detail = g_strdup(msg);
    g_strchomp(detail);
    g_warning("Grok warning: %s", detail);
  }
}

static void *grok_init_once(void *data G_GNUC_UNUSED) {
  grk_initialize(NULL, 0, NULL);
  return GINT_TO_POINTER(1);
}

static void ensure_grok_init(void) {
  static GOnce once = G_ONCE_INIT;
  g_once(&once, grok_init_once, NULL);
}

static inline void write_pixel_ycbcr(uint32_t *dest, uint8_t Y,
                                     int16_t R_chroma, int16_t G_chroma,
                                     int16_t B_chroma) {
  int16_t R = Y + R_chroma;
  int16_t G = Y + G_chroma;
  int16_t B = Y + B_chroma;

  R = CLAMP(R, 0, 255);
  G = CLAMP(G, 0, 255);
  B = CLAMP(B, 0, 255);

  *dest =
      0xff000000 | ((uint8_t) R << 16) | ((uint8_t) G << 8) | ((uint8_t) B);
}

static inline void write_pixel_rgb(uint32_t *dest, uint8_t R, uint8_t G,
                                   uint8_t B) {
  *dest = 0xff000000 | R << 16 | G << 8 | B;
}

static void unpack_argb(enum _openslide_jp2k_colorspace space,
                        grk_image_comp *comps, uint32_t *dest, int32_t w,
                        int32_t h) {
  int c0_sub_x = w / comps[0].w;
  int c1_sub_x = w / comps[1].w;
  int c2_sub_x = w / comps[2].w;
  int c0_sub_y = h / comps[0].h;
  int c1_sub_y = h / comps[1].h;
  int c2_sub_y = h / comps[2].h;

  int32_t *c0_data = (int32_t *) comps[0].data;
  int32_t *c1_data = (int32_t *) comps[1].data;
  int32_t *c2_data = (int32_t *) comps[2].data;

  if (space == OPENSLIDE_JP2K_YCBCR && c0_sub_x == 1 && c1_sub_x == 2 &&
      c2_sub_x == 2 && c0_sub_y == 1 && c1_sub_y == 1 && c2_sub_y == 1) {
    // Aperio 33003
    for (int32_t y = 0; y < h; y++) {
      int32_t c0_row_base = y * comps[0].w;
      int32_t c1_row_base = y * comps[1].w;
      int32_t c2_row_base = y * comps[2].w;
      int32_t x;
      for (x = 0; x < w - 1; x += 2) {
        uint8_t c0 = c0_data[c0_row_base + x];
        uint8_t c1 = c1_data[c1_row_base + (x / 2)];
        uint8_t c2 = c2_data[c2_row_base + (x / 2)];
        int16_t R_chroma = _openslide_R_Cr[c2];
        int16_t G_chroma =
            (_openslide_G_Cb[c1] + _openslide_G_Cr[c2]) >> 16;
        int16_t B_chroma = _openslide_B_Cb[c1];
        write_pixel_ycbcr(dest++, c0, R_chroma, G_chroma, B_chroma);
        c0 = c0_data[c0_row_base + x + 1];
        write_pixel_ycbcr(dest++, c0, R_chroma, G_chroma, B_chroma);
      }
      if (x < w) {
        uint8_t c0 = c0_data[c0_row_base + x];
        uint8_t c1 = c1_data[c1_row_base + (x / 2)];
        uint8_t c2 = c2_data[c2_row_base + (x / 2)];
        int16_t R_chroma = _openslide_R_Cr[c2];
        int16_t G_chroma =
            (_openslide_G_Cb[c1] + _openslide_G_Cr[c2]) >> 16;
        int16_t B_chroma = _openslide_B_Cb[c1];
        write_pixel_ycbcr(dest++, c0, R_chroma, G_chroma, B_chroma);
      }
    }

  } else if (space == OPENSLIDE_JP2K_YCBCR) {
    // Slow fallback
    static gint warned_slowpath_ycbcr;
    _openslide_performance_warn_once(&warned_slowpath_ycbcr,
                                     "Decoding YCbCr JP2K image via "
                                     "slow fallback (grok), subsamples "
                                     "x %d-%d-%d y %d-%d-%d",
                                     c0_sub_x, c1_sub_x, c2_sub_x,
                                     c0_sub_y, c1_sub_y, c2_sub_y);

    for (int32_t y = 0; y < h; y++) {
      int32_t c0_row_base = (y / c0_sub_y) * comps[0].w;
      int32_t c1_row_base = (y / c1_sub_y) * comps[1].w;
      int32_t c2_row_base = (y / c2_sub_y) * comps[2].w;
      for (int32_t x = 0; x < w; x++) {
        uint8_t c0 = c0_data[c0_row_base + (x / c0_sub_x)];
        uint8_t c1 = c1_data[c1_row_base + (x / c1_sub_x)];
        uint8_t c2 = c2_data[c2_row_base + (x / c2_sub_x)];
        int16_t R_chroma = _openslide_R_Cr[c2];
        int16_t G_chroma =
            (_openslide_G_Cb[c1] + _openslide_G_Cr[c2]) >> 16;
        int16_t B_chroma = _openslide_B_Cb[c1];
        write_pixel_ycbcr(dest++, c0, R_chroma, G_chroma, B_chroma);
      }
    }

  } else if (space == OPENSLIDE_JP2K_RGB && c0_sub_x == 1 && c1_sub_x == 1 &&
             c2_sub_x == 1 && c0_sub_y == 1 && c1_sub_y == 1 &&
             c2_sub_y == 1) {
    // Aperio 33005
    for (int32_t y = 0; y < h; y++) {
      int32_t c0_row_base = y * comps[0].w;
      int32_t c1_row_base = y * comps[1].w;
      int32_t c2_row_base = y * comps[2].w;
      for (int32_t x = 0; x < w; x++) {
        uint8_t c0 = c0_data[c0_row_base + x];
        uint8_t c1 = c1_data[c1_row_base + x];
        uint8_t c2 = c2_data[c2_row_base + x];
        write_pixel_rgb(dest++, c0, c1, c2);
      }
    }

  } else if (space == OPENSLIDE_JP2K_RGB) {
    // Slow fallback
    static gint warned_slowpath_rgb;
    _openslide_performance_warn_once(&warned_slowpath_rgb,
                                     "Decoding RGB JP2K image via "
                                     "slow fallback (grok), subsamples "
                                     "x %d-%d-%d y %d-%d-%d",
                                     c0_sub_x, c1_sub_x, c2_sub_x,
                                     c0_sub_y, c1_sub_y, c2_sub_y);

    for (int32_t y = 0; y < h; y++) {
      int32_t c0_row_base = (y / c0_sub_y) * comps[0].w;
      int32_t c1_row_base = (y / c1_sub_y) * comps[1].w;
      int32_t c2_row_base = (y / c2_sub_y) * comps[2].w;
      for (int32_t x = 0; x < w; x++) {
        uint8_t c0 = c0_data[c0_row_base + (x / c0_sub_x)];
        uint8_t c1 = c1_data[c1_row_base + (x / c1_sub_x)];
        uint8_t c2 = c2_data[c2_row_base + (x / c2_sub_x)];
        write_pixel_rgb(dest++, c0, c1, c2);
      }
    }
  }
}

bool _openslide_jp2k_decode_buffer_grok(uint32_t *dest, int32_t w, int32_t h,
                                        const void *data, int32_t datalen,
                                        enum _openslide_jp2k_colorspace space,
                                        GError **err) {
  g_assert(data != NULL);
  g_assert(datalen >= 0);

  ensure_grok_init();

  // set up message handlers
  GError *tmp_err = NULL;
  grk_msg_handlers handlers = {0};
  handlers.error_callback = grok_error_callback;
  handlers.error_data = &tmp_err;
  handlers.warn_callback = grok_warning_callback;
  handlers.warn_data = NULL;
  grk_set_msg_handlers(handlers);

  // set up stream parameters for buffer-based decoding
  grk_stream_params stream_params = {0};
  stream_params.buf = (uint8_t *) data;
  stream_params.buf_len = datalen;
  stream_params.is_read_stream = true;

  // set up decompression parameters
  grk_decompress_parameters decompress_params = {0};
  decompress_params.core.tile_cache_strategy = GRK_TILE_CACHE_NONE;

  // create decompressor
  grk_object *codec =
      grk_decompress_init(&stream_params, &decompress_params);
  if (!codec) {
    if (tmp_err) {
      g_propagate_error(err, tmp_err);
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "grk_decompress_init() failed");
    }
    return false;
  }

  // read header
  grk_header_info header_info = {0};
  if (!grk_decompress_read_header(codec, &header_info)) {
    if (tmp_err) {
      g_propagate_error(err, tmp_err);
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "grk_decompress_read_header() failed");
    }
    grk_object_unref(codec);
    return false;
  }
  g_clear_error(&tmp_err);

  // sanity checks
  if (header_info.header_image.x1 != (uint32_t) w ||
      header_info.header_image.y1 != (uint32_t) h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Dimensional mismatch reading JP2K, "
                "expected %dx%d, got %ux%u",
                w, h, header_info.header_image.x1,
                header_info.header_image.y1);
    grk_object_unref(codec);
    return false;
  }
  if (header_info.header_image.numcomps != 3) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Expected 3 image components, found %u",
                header_info.header_image.numcomps);
    grk_object_unref(codec);
    return false;
  }

  // decode
  if (!grk_decompress(codec, NULL)) {
    if (tmp_err) {
      g_propagate_error(err, tmp_err);
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "grk_decompress() failed");
    }
    grk_object_unref(codec);
    return false;
  }
  g_clear_error(&tmp_err);

  // get the decompressed image
  grk_image *image = grk_decompress_get_image(codec);
  if (!image) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "grk_decompress_get_image() returned NULL");
    grk_object_unref(codec);
    return false;
  }

  // copy pixels
  unpack_argb(space, image->comps, dest, w, h);

  grk_object_unref(codec);
  return true;
}
