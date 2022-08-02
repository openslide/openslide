/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2015 Benjamin Gilbert
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

#include <string.h>
#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-jp2k.h"

#include <openjpeg.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(opj_codec_t, opj_destroy_codec)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(opj_image_t, opj_image_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(opj_stream_t, opj_stream_destroy)

struct buffer_state {
  const uint8_t *data;
  int32_t offset;
  int32_t length;
};

static inline void write_pixel_ycbcr(uint32_t *dest, uint8_t Y,
                                     int16_t R_chroma, int16_t G_chroma,
                                     int16_t B_chroma) {
  int16_t R = Y + R_chroma;
  int16_t G = Y + G_chroma;
  int16_t B = Y + B_chroma;

  R = CLAMP(R, 0, 255);
  G = CLAMP(G, 0, 255);
  B = CLAMP(B, 0, 255);

  *dest = 0xff000000 | ((uint8_t) R << 16) | ((uint8_t) G << 8) | ((uint8_t) B);
}

static inline void write_pixel_rgb(uint32_t *dest,
                                   uint8_t R, uint8_t G, uint8_t B) {
  *dest = 0xff000000 | R << 16 | G << 8 | B;
}

static void unpack_argb(enum _openslide_jp2k_colorspace space,
                        opj_image_comp_t *comps,
                        uint32_t *dest,
                        int32_t w, int32_t h) {
  int c0_sub_x = w / comps[0].w;
  int c1_sub_x = w / comps[1].w;
  int c2_sub_x = w / comps[2].w;
  int c0_sub_y = h / comps[0].h;
  int c1_sub_y = h / comps[1].h;
  int c2_sub_y = h / comps[2].h;

  //g_debug("color space %d, subsamples x %d-%d-%d y %d-%d-%d", space, c0_sub_x, c1_sub_x, c2_sub_x, c0_sub_y, c1_sub_y, c2_sub_y);

  if (space == OPENSLIDE_JP2K_YCBCR &&
      c0_sub_x == 1 && c1_sub_x == 2 && c2_sub_x == 2 &&
      c0_sub_y == 1 && c1_sub_y == 1 && c2_sub_y == 1) {
    // Aperio 33003
    for (int32_t y = 0; y < h; y++) {
      int32_t c0_row_base = y * comps[0].w;
      int32_t c1_row_base = y * comps[1].w;
      int32_t c2_row_base = y * comps[2].w;
      int32_t x;
      for (x = 0; x < w - 1; x += 2) {
        uint8_t c0 = comps[0].data[c0_row_base + x];
        uint8_t c1 = comps[1].data[c1_row_base + (x / 2)];
        uint8_t c2 = comps[2].data[c2_row_base + (x / 2)];
        int16_t R_chroma = _openslide_R_Cr[c2];
        int16_t G_chroma = (_openslide_G_Cb[c1] + _openslide_G_Cr[c2]) >> 16;
        int16_t B_chroma = _openslide_B_Cb[c1];
        write_pixel_ycbcr(dest++, c0, R_chroma, G_chroma, B_chroma);
        c0 = comps[0].data[c0_row_base + x + 1];
        write_pixel_ycbcr(dest++, c0, R_chroma, G_chroma, B_chroma);
      }
      if (x < w) {
        uint8_t c0 = comps[0].data[c0_row_base + x];
        uint8_t c1 = comps[1].data[c1_row_base + (x / 2)];
        uint8_t c2 = comps[2].data[c2_row_base + (x / 2)];
        int16_t R_chroma = _openslide_R_Cr[c2];
        int16_t G_chroma = (_openslide_G_Cb[c1] + _openslide_G_Cr[c2]) >> 16;
        int16_t B_chroma = _openslide_B_Cb[c1];
        write_pixel_ycbcr(dest++, c0, R_chroma, G_chroma, B_chroma);
      }
    }

  } else if (space == OPENSLIDE_JP2K_YCBCR) {
    // Slow fallback
    static gint warned_slowpath_ycbcr;
    _openslide_performance_warn_once(&warned_slowpath_ycbcr,
                                     "Decoding YCbCr JP2K image via "
                                     "slow fallback, subsamples "
                                     "x %d-%d-%d y %d-%d-%d",
                                     c0_sub_x, c1_sub_x, c2_sub_x,
                                     c0_sub_y, c1_sub_y, c2_sub_y);

    for (int32_t y = 0; y < h; y++) {
      int32_t c0_row_base = (y / c0_sub_y) * comps[0].w;
      int32_t c1_row_base = (y / c1_sub_y) * comps[1].w;
      int32_t c2_row_base = (y / c2_sub_y) * comps[2].w;
      for (int32_t x = 0; x < w; x++) {
        uint8_t c0 = comps[0].data[c0_row_base + (x / c0_sub_x)];
        uint8_t c1 = comps[1].data[c1_row_base + (x / c1_sub_x)];
        uint8_t c2 = comps[2].data[c2_row_base + (x / c2_sub_x)];
        int16_t R_chroma = _openslide_R_Cr[c2];
        int16_t G_chroma = (_openslide_G_Cb[c1] + _openslide_G_Cr[c2]) >> 16;
        int16_t B_chroma = _openslide_B_Cb[c1];
        write_pixel_ycbcr(dest++, c0, R_chroma, G_chroma, B_chroma);
      }
    }

  } else if (space == OPENSLIDE_JP2K_RGB &&
             c0_sub_x == 1 && c1_sub_x == 1 && c2_sub_x == 1 &&
             c0_sub_y == 1 && c1_sub_y == 1 && c2_sub_y == 1) {
    // Aperio 33005
    for (int32_t y = 0; y < h; y++) {
      int32_t c0_row_base = y * comps[0].w;
      int32_t c1_row_base = y * comps[1].w;
      int32_t c2_row_base = y * comps[2].w;
      for (int32_t x = 0; x < w; x++) {
        uint8_t c0 = comps[0].data[c0_row_base + x];
        uint8_t c1 = comps[1].data[c1_row_base + x];
        uint8_t c2 = comps[2].data[c2_row_base + x];
        write_pixel_rgb(dest++, c0, c1, c2);
      }
    }

  } else if (space == OPENSLIDE_JP2K_RGB) {
    // Slow fallback
    static gint warned_slowpath_rgb;
    _openslide_performance_warn_once(&warned_slowpath_rgb,
                                     "Decoding RGB JP2K image via "
                                     "slow fallback, subsamples "
                                     "x %d-%d-%d y %d-%d-%d",
                                     c0_sub_x, c1_sub_x, c2_sub_x,
                                     c0_sub_y, c1_sub_y, c2_sub_y);

    for (int32_t y = 0; y < h; y++) {
      int32_t c0_row_base = (y / c0_sub_y) * comps[0].w;
      int32_t c1_row_base = (y / c1_sub_y) * comps[1].w;
      int32_t c2_row_base = (y / c2_sub_y) * comps[2].w;
      for (int32_t x = 0; x < w; x++) {
        uint8_t c0 = comps[0].data[c0_row_base + (x / c0_sub_x)];
        uint8_t c1 = comps[1].data[c1_row_base + (x / c1_sub_x)];
        uint8_t c2 = comps[2].data[c2_row_base + (x / c2_sub_x)];
        write_pixel_rgb(dest++, c0, c1, c2);
      }
    }
  }
}

static void warning_callback(const char *msg G_GNUC_UNUSED,
                             void *data G_GNUC_UNUSED) {
  //g_debug("%s", msg);
}

static void error_callback(const char *msg, void *data) {
  GError **err = (GError **) data;
  if (err && !*err) {
    g_autofree char *detail = g_strdup(msg);
    g_strchomp(detail);
    // OpenJPEG can produce obscure error messages, so make sure to
    // indicate where they came from
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "OpenJPEG error: %s", detail);
  }
}

static OPJ_SIZE_T read_callback(void *buf, OPJ_SIZE_T count, void *data) {
  struct buffer_state *state = data;

  count = MIN(count, (OPJ_SIZE_T) (state->length - state->offset));
  if (!count) {
    return (OPJ_SIZE_T) -1;
  }
  memcpy(buf, state->data + state->offset, count);
  state->offset += count;
  return count;
}

static OPJ_OFF_T skip_callback(OPJ_OFF_T count, void *data) {
  struct buffer_state *state = data;

  int32_t orig_offset = state->offset;
  state->offset = CLAMP(state->offset + count, 0, state->length);
  if (count && state->offset == orig_offset) {
    return -1;
  }
  return state->offset - orig_offset;
}

static OPJ_BOOL seek_callback(OPJ_OFF_T offset, void *data) {
  struct buffer_state *state = data;

  if (offset < 0 || offset > state->length) {
    return OPJ_FALSE;
  }
  state->offset = offset;
  return OPJ_TRUE;
}

bool _openslide_jp2k_decode_buffer(uint32_t *dest,
                                   int32_t w, int32_t h,
                                   const void *data, int32_t datalen,
                                   enum _openslide_jp2k_colorspace space,
                                   GError **err) {
  g_assert(data != NULL);
  g_assert(datalen >= 0);

  // init stream
  // avoid tracking stream offset (and implementing skip callback) by having
  // OpenJPEG read the whole buffer at once
  g_autoptr(opj_stream_t) stream = opj_stream_create(datalen, true);
  struct buffer_state state = {
    .data = data,
    .length = datalen,
  };
  opj_stream_set_user_data(stream, &state, NULL);
  opj_stream_set_user_data_length(stream, datalen);
  opj_stream_set_read_function(stream, read_callback);
  opj_stream_set_skip_function(stream, skip_callback);
  opj_stream_set_seek_function(stream, seek_callback);

  // init codec
  g_autoptr(opj_codec_t) codec = opj_create_decompress(OPJ_CODEC_J2K);
  opj_dparameters_t parameters;
  opj_set_default_decoder_parameters(&parameters);
  opj_setup_decoder(codec, &parameters);

  // enable error handlers
  // note: don't use info_handler, it outputs lots of junk
  GError *tmp_err = NULL;
  opj_set_warning_handler(codec, warning_callback, &tmp_err);
  opj_set_error_handler(codec, error_callback, &tmp_err);

  // read header
  g_autoptr(opj_image_t) image = NULL;
  if (!opj_read_header(stream, codec, &image)) {
    if (tmp_err) {
      g_propagate_error(err, tmp_err);
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "opj_read_header() failed");
    }
    return false;
  }
  g_clear_error(&tmp_err);  // clear any spurious message

  // sanity checks
  if (image->x1 != (OPJ_UINT32) w || image->y1 != (OPJ_UINT32) h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Dimensional mismatch reading JP2K, "
                "expected %dx%d, got %ux%u",
                w, h, image->x1, image->y1);
    return false;
  }
  if (image->numcomps != 3) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Expected 3 image components, found %u", image->numcomps);
    return false;
  }
  // TODO more checks?

  // decode
  if (!opj_decode(codec, stream, image)) {
    if (tmp_err) {
      g_propagate_error(err, tmp_err);
    } else {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "opj_decode() failed");
    }
    return false;
  }
  g_clear_error(&tmp_err);  // clear any spurious message

  // copy pixels
  unpack_argb(space, image->comps, dest, w, h);

  return true;
}
