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
#include "openslide-decode-jxr.h"

#define BGR24TOARGB32(p)                                                       \
  (0xFF000000 | (uint32_t)((p)[0]) | ((uint32_t)((p)[1]) << 8) |               \
   ((uint32_t)((p)[2]) << 16))

#define BGR48TOARGB32(p)                                                       \
  (0xFF000000 | (uint32_t)((p)[1]) | ((uint32_t)((p)[3]) << 8) |               \
   ((uint32_t)((p)[5]) << 16))

enum remixer {
  RMX_CAIRO24RGB,
  RMX_RGB48TOCAIRO24RGB,
};

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

static void print_err(ERR err) {
  struct wmp_err_msg *p = &msgs[0];

  if (err >= 0) {
    return;
  }
  while ((p++)->msg) {
    if (p->id == err) {
      fprintf(stderr, "_openslide_jxr_decode_buf error: %s\n", p->msg);
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

/* GUID_PKPixelFormat24bppBGR has 24bits per pixel. CAIRO_FORMAT_RGB24 has
 * 32bits, with the upper 8 bits unused
 */
bool convert_24bppbgr_to_cairo24bpprgb(struct jxr_decoded *p) {
  size_t new_size = p->w * p->h * 4;
  uint32_t *buf = g_slice_alloc(new_size);
  uint32_t *bp = buf;
  size_t i = 0;

  while (i < p->size) {
    *bp++ = BGR24TOARGB32(&p->data[i]);
    i += 3;
  }

  g_slice_free1(p->size, p->data);
  p->stride = p->w * 4;
  p->pixel_bits = 32;
  p->size = new_size;
  p->data = (uint8_t *) buf;
  return true;
}

bool convert_48bppbgr_to_cairo24bpprgb(struct jxr_decoded *p) {
  size_t new_size = p->w * p->h * 4;
  uint32_t *buf = g_slice_alloc(new_size);
  uint32_t *bp = buf;
  size_t i = 0;

  while (i < p->size) {
    *bp++ = BGR48TOARGB32(&p->data[i]);
    i += 6;
  }

  g_slice_free1(p->size, p->data);
  p->stride = p->w * 4;
  p->pixel_bits = 32;
  p->size = new_size;
  p->data = (uint8_t *) buf;
  return true;
}

bool _openslide_jxr_decode_buf(void *data, size_t datalen,
                               struct jxr_decoded *dst) {

  struct WMPStream *pStream = NULL;
  PKImageDecode *pDecoder = NULL;
  PKFormatConverter *pConverter = NULL;
  ERR err;
  PKPixelFormatGUID fmt;
  PKRect rect = {0, 0, 0, 0};
  int remixer;

  CreateWS_Memory(&pStream, (void *) data, datalen);

  // IID_PKImageWmpDecode is the only supported decoder PKIID
  err = PKCodecFactory_CreateCodec(&IID_PKImageWmpDecode, (void **) &pDecoder);
  if (err < 0) {
    goto Cleanup;
  }

  err = pDecoder->Initialize(pDecoder, pStream);
  if (err < 0) {
    goto Cleanup;
  }

  pDecoder->GetSize(pDecoder, &rect.Width, &rect.Height);
  pDecoder->GetPixelFormat(pDecoder, &fmt);

  // GUID_PKPixelFormat32bppRGBA is not supported by converter
  PKPixelFormatGUID fmt_out = GUID_PKPixelFormat24bppBGR;
  remixer = RMX_CAIRO24RGB;
  if (IsEqualGUID(&fmt, &GUID_PKPixelFormat24bppBGR)) {
    fmt_out = GUID_PKPixelFormat24bppBGR;
    remixer = RMX_CAIRO24RGB;
  } else if (IsEqualGUID(&fmt, &GUID_PKPixelFormat48bppRGB)) {
    fmt_out = GUID_PKPixelFormat48bppRGB;
    remixer = RMX_RGB48TOCAIRO24RGB;
  } else {
    printf("Unsupported jxr fmt, try decode with GUID_PKPixelFormat24bppBGR\n");
  }

  dst->w = rect.Width;
  dst->h = rect.Height;
  dst->stride = (rect.Width * MAX(get_bits_per_pixel(&fmt),
                                   get_bits_per_pixel(&fmt_out)) + 7) / 8;
  dst->size = dst->stride * dst->h;
  dst->data = g_slice_alloc(dst->size);
  dst->pixel_bits = get_bits_per_pixel(&fmt_out);

  //Create color converter
  err = PKCodecFactory_CreateFormatConverter(&pConverter);
  if (err < 0) {
    goto Cleanup;
  }

  err = pConverter->Initialize(pConverter, pDecoder, NULL, fmt_out);
  if (err < 0) {
    goto Cleanup;
  }

  err = pConverter->Copy(pConverter, &rect, dst->data, dst->stride);
  if (err < 0) {
    goto Cleanup;
  }

  switch (remixer) {
  case RMX_CAIRO24RGB:
    convert_24bppbgr_to_cairo24bpprgb(dst);
    break;
  case RMX_RGB48TOCAIRO24RGB:
    convert_48bppbgr_to_cairo24bpprgb(dst);
    break;
  }

Cleanup:
  print_err(err);
  CloseWS_Memory(&pStream);
  pDecoder->Release(&pDecoder);
  pConverter->Release(&pConverter);

  return (err < 0) ? false : true;
}

/* read JPEG XR encoded data from file
 * @pos and @len is the start and length of encoded data
 * A CZI file has many tiles encoded in JPEG XR */
bool _openslide_jxr_read(const char *filename, int64_t pos, int64_t len,
                         struct jxr_decoded *dst,
                         GError **err) {
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  if (!_openslide_fseek(f, pos, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to jxr pixel data");
    return false;
  }

  g_autofree char *buf = g_malloc(len);
  if (_openslide_fread(f, buf, (size_t) len) != (size_t) len) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read pixel data");
    return false;
  }
  return _openslide_jxr_decode_buf(buf, len, dst);
}