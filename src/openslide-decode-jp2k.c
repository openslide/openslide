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
#include "openslide-decode-jp2k.h"

#include <openjpeg.h>

static void write_pixel_ycbcr(uint32_t *dest,
                              uint8_t c0, uint8_t c1, uint8_t c2) {
  int16_t R = c0 + _openslide_R_Cr[c2];
  int16_t G = c0 + _openslide_G_CbCr[c1][c2];
  int16_t B = c0 + _openslide_B_Cb[c1];

  R = CLAMP(R, 0, 255);
  G = CLAMP(G, 0, 255);
  B = CLAMP(B, 0, 255);

  *dest = 0xff000000 | ((uint8_t) R << 16) | ((uint8_t) G << 8) | ((uint8_t) B);
}

static void write_pixel_rgb(uint32_t *dest,
                            uint8_t c0, uint8_t c1, uint8_t c2) {
  *dest = 0xff000000 | c0 << 16 | c1 << 8 | c2;
}

static void unpack_argb(enum _openslide_jp2k_colorspace space,
                        opj_image_comp_t *comps,
                        uint32_t *dest,
                        int32_t w, int32_t h) {
  // TODO: too slow, and with duplicated code!

  int c0_sub_x = w / comps[0].w;
  int c1_sub_x = w / comps[1].w;
  int c2_sub_x = w / comps[2].w;
  int c0_sub_y = h / comps[0].h;
  int c1_sub_y = h / comps[1].h;
  int c2_sub_y = h / comps[2].h;

  int64_t i = 0;

  switch (space) {
  case OPENSLIDE_JP2K_YCBCR:
    for (int32_t y = 0; y < h; y++) {
      for (int32_t x = 0; x < w; x++) {
        uint8_t c0 = comps[0].data[(y / c0_sub_y) * comps[0].w + (x / c0_sub_x)];
        uint8_t c1 = comps[1].data[(y / c1_sub_y) * comps[1].w + (x / c1_sub_x)];
        uint8_t c2 = comps[2].data[(y / c2_sub_y) * comps[2].w + (x / c2_sub_x)];

        write_pixel_ycbcr(dest + i, c0, c1, c2);
        i++;
      }
    }

    break;

  case OPENSLIDE_JP2K_RGB:
    for (int32_t y = 0; y < h; y++) {
      for (int32_t x = 0; x < w; x++) {
        uint8_t c0 = comps[0].data[(y / c0_sub_y) * comps[0].w + (x / c0_sub_x)];
        uint8_t c1 = comps[1].data[(y / c1_sub_y) * comps[1].w + (x / c1_sub_x)];
        uint8_t c2 = comps[2].data[(y / c2_sub_y) * comps[2].w + (x / c2_sub_x)];

        write_pixel_rgb(dest + i, c0, c1, c2);
        i++;
      }
    }
    break;
  }
}

static void warning_callback(const char *msg G_GNUC_UNUSED,
                             void *data G_GNUC_UNUSED) {
  //g_debug("%s", msg);
}

static void error_callback(const char *msg, void *data) {
  GError **err = (GError **) data;
  if (err && !*err) {
    char *detail = g_strdup(msg);
    g_strchomp(detail);
    // OpenJPEG can produce obscure error messages, so make sure to
    // indicate where they came from
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "OpenJPEG error: %s", detail);
    g_free(detail);
  }
}

bool _openslide_jp2k_decode_buffer(uint32_t *dest,
                                   int32_t w, int32_t h,
                                   void *data, int32_t datalen,
                                   enum _openslide_jp2k_colorspace space,
                                   GError **err) {
  GError *tmp_err = NULL;
  bool success = false;

  // opj_cio_open interprets a NULL buffer as opening for write
  g_assert(data != NULL);

  // init decompressor
  opj_cio_t *stream = NULL;
  opj_dinfo_t *dinfo = NULL;
  opj_image_t *image = NULL;

  // note: don't use info_handler, it outputs lots of junk
  opj_event_mgr_t event_callbacks = {
    .error_handler = error_callback,
    .warning_handler = warning_callback,
  };

  opj_dparameters_t parameters;
  dinfo = opj_create_decompress(CODEC_J2K);
  opj_set_default_decoder_parameters(&parameters);
  opj_setup_decoder(dinfo, &parameters);
  stream = opj_cio_open((opj_common_ptr) dinfo, data, datalen);
  opj_set_event_mgr((opj_common_ptr) dinfo, &event_callbacks, &tmp_err);

  // decode
  image = opj_decode(dinfo, stream);

  // check error
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    goto DONE;
  }

  // sanity checks
  if (image->x1 != w || image->y1 != h) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Dimensional mismatch reading JP2K, "
                "expected %dx%d, got %dx%d",
                w, h, image->x1, image->y1);
    goto DONE;
  }
  if (image->numcomps != 3) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Expected 3 image components, found %d", image->numcomps);
    goto DONE;
  }

  // TODO more checks?

  unpack_argb(space, image->comps, dest, w, h);

  success = true;

DONE:
  if (image) {
    opj_image_destroy(image);
  }
  if (stream) {
    opj_cio_close(stream);
  }
  if (dinfo) {
    opj_destroy_decompress(dinfo);
  }
  return success;
}
