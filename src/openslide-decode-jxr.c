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
#include <glib.h>
#include <JXRGlue.h>

#include "openslide-private.h"
#include "openslide-image.h"
#include "openslide-decode-jxr.h"

static struct wmp_err_msg {
  ERR id;
  char *msg;
} msgs[] = {
  {WMP_errFail, "WMP_errFail"},
  {WMP_errNotYetImplemented, "WMP_errNotYetImplemented"},
  {WMP_errAbstractMethod, "WMP_errAbstractMethod"},
  {WMP_errOutOfMemory, "WMP_errOutOfMemory"},
  {WMP_errFileIO, "WMP_errFileIO"},
  {WMP_errBufferOverflow, "WMP_errBufferOverflow"},
  {WMP_errInvalidParameter, "WMP_errInvalidParameter"},
  {WMP_errInvalidArgument, "WMP_errInvalidArgument"},
  {WMP_errUnsupportedFormat, "WMP_errUnsupportedFormat"},
  {WMP_errIncorrectCodecVersion, "WMP_errIncorrectCodecVersion"},
  {WMP_errIndexNotFound, "WMP_errIndexNotFound"},
  {WMP_errOutOfSequence, "WMP_errOutOfSequence"},
  {WMP_errNotInitialized, "WMP_errNotInitialized"},
  {WMP_errMustBeMultipleOf16LinesUntilLastCall, "WMP_errMustBeMultipleOf16LinesUntilLastCall"},
  {WMP_errPlanarAlphaBandedEncRequiresTempFile, "WMP_errPlanarAlphaBandedEncRequiresTempFile"},
  {WMP_errAlphaModeCannotBeTranscoded, "WMP_errAlphaModeCannotBeTranscoded"},
  {WMP_errIncorrectCodecSubVersion, "WMP_errIncorrectCodecSubVersion"},
  {0, NULL}
};

static void print_err(ERR jerr, GError **err) {
  struct wmp_err_msg *p = &msgs[0];
  if (jerr >= 0) {
    return;
  }
  while ((p++)->msg) {
    if (p->id == jerr) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "JXR decode error: %s", p->msg);
      break;
    }
  }
}

static guint get_bits_per_pixel(const PKPixelFormatGUID *pixel_format) {
  PKPixelInfo pixel_info;

  pixel_info.pGUIDPixFmt = pixel_format;
  PixelFormatLookup(&pixel_info, LOOKUP_FORWARD);
  return pixel_info.cbitUnit;
}

bool _openslide_jxr_decode_buf(const void *src, int64_t src_len, uint32_t *dst,
                               int64_t dst_len, GError **err) {
  struct WMPStream *pStream = NULL;
  PKImageDecode *pDecoder = NULL;
  PKFormatConverter *pConverter = NULL;
  ERR jerr;
  PKPixelFormatGUID fmt;
  PKRect rect = {0, 0, 0, 0};
  g_autofree uint8_t *unjxr = NULL;

  CreateWS_Memory(&pStream, (void *) src, src_len);
  // IID_PKImageWmpDecode is the only supported decoder PKIID
  jerr = PKCodecFactory_CreateCodec(&IID_PKImageWmpDecode, (void **) &pDecoder);
  if (jerr < 0) {
    // jxrlib uses lots of goto
    goto Cleanup;
  }

  jerr = pDecoder->Initialize(pDecoder, pStream);
  if (jerr < 0) {
    goto Cleanup;
  }

  pDecoder->GetSize(pDecoder, &rect.Width, &rect.Height);
  int64_t out_len = rect.Width * rect.Height * 4;
  // JXR tile size may be incorrect in czi directory entries
  g_assert(out_len <= dst_len);

  pDecoder->GetPixelFormat(pDecoder, &fmt);
  PKPixelFormatGUID fmt_out;
  void (*convert)(uint8_t *, size_t, uint32_t *);
  if (IsEqualGUID(&fmt, &GUID_PKPixelFormat24bppBGR)) {
    fmt_out = GUID_PKPixelFormat24bppBGR;
    convert = _openslide_bgr24_to_argb32;
  } else if (IsEqualGUID(&fmt, &GUID_PKPixelFormat48bppRGB)) {
    /* Although the format called 48bppRGB in JXR, its color order is BGR for
     * czi. Use 48bppRGB as it is and prefer openslide function for converting
     * to argb32.
     */
    fmt_out = GUID_PKPixelFormat48bppRGB;
    convert = _openslide_bgr48_to_argb32;
  } else if (IsEqualGUID(&fmt, &GUID_PKPixelFormat8bppGray)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "GUID_PKPixelFormat8bppGray is not supported");
    goto Cleanup;
  } else if (IsEqualGUID(&fmt, &GUID_PKPixelFormat16bppGray)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "GUID_PKPixelFormat16bppGray is not supported");
    goto Cleanup;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Currently only support GUID_PKPixelFormat24bppBGR and "
                "GUID_PKPixelFormat48bppRGB");
    goto Cleanup;
  }

  uint32_t stride =
      rect.Width *
      ((MAX(get_bits_per_pixel(&fmt), get_bits_per_pixel(&fmt_out)) + 7) / 8);
  size_t unjxr_len = stride * rect.Height;
  unjxr = g_try_malloc(unjxr_len);
  if (!unjxr) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't allocate %" PRId64 " bytes for decoding JXR",
                dst_len);
    return false;
  }

  // Create color converter
  jerr = PKCodecFactory_CreateFormatConverter(&pConverter);
  if (jerr < 0) {
    goto Cleanup;
  }

  jerr = pConverter->Initialize(pConverter, pDecoder, NULL, fmt_out);
  if (jerr < 0) {
    goto Cleanup;
  }

  jerr = pConverter->Copy(pConverter, &rect, unjxr, stride);
  if (jerr < 0) {
    goto Cleanup;
  }

  convert(unjxr, unjxr_len, dst);

Cleanup:
  print_err(jerr, err);
  CloseWS_Memory(&pStream);
  pDecoder->Release(&pDecoder);
  pConverter->Release(&pConverter);

  return (jerr < 0) ? false : true;
}

static bool short_header_flag(uint8_t *data) {
  return data[10] & 0x80;
}

/* parse jpeg xr header to get width and height */
bool _openslide_jxr_dim(const void *data, size_t data_len, uint32_t *width,
                        uint32_t *height) {
  uint8_t *p = (uint8_t *) data;
  uint8_t *s = NULL;
  /* locate beginning of JXR image stream instead of parsing IFD. Cannot use
   * strstr() because there may be many zeros before WMPHOTO magic. */
  for (size_t i = 0; i < (data_len - 8); i++, p++) {
    if (*(p + 0) == 'W' && *(p + 1) == 'M' && *(p + 2) == 'P' &&
        *(p + 3) == 'H' && *(p + 4) == 'O' && *(p + 5) == 'T' &&
        *(p + 6) == 'O' && *(p + 7) == '\0') {

      s = p;
      break;
    }
  }

  if (s == NULL) {
    g_warning("JPEG XR magic WMPHOTO not found");
    return false;
  }

  uint32_t width1, height1; /* width, height minus 1 */
  if (short_header_flag(s)) {
    /* per JXR doc: u(n) unsigned integer using n bits, where MSB is the left
     * most bit. */
    width1 = GUINT16_FROM_BE(*(uint16_t *) (s + 12));
    height1 = GUINT16_FROM_BE(*(uint16_t *) (s + 14));
  } else {
    width1 = GUINT32_FROM_BE(*(uint32_t *) (s + 12));
    height1 = GUINT32_FROM_BE(*(uint32_t *) (s + 16));
  }

  *width = width1 + 1;
  *height = height1 + 1;
  return true;
}
