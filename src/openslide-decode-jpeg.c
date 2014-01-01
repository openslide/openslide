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
#include "openslide-decode-jpeg.h"

#include <glib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <jpeglib.h>
#include <jerror.h>

struct associated_image {
  struct _openslide_associated_image base;
  char *filename;
  int64_t offset;
};


static void my_error_exit(j_common_ptr cinfo) {
  struct _openslide_jpeg_error_mgr *jerr =
    (struct _openslide_jpeg_error_mgr *) cinfo->err;

  (jerr->pub.output_message) (cinfo);

  //  g_debug("JUMP");
  longjmp(*(jerr->env), 1);
}

static void my_output_message(j_common_ptr cinfo) {
  struct _openslide_jpeg_error_mgr *jerr =
    (struct _openslide_jpeg_error_mgr *) cinfo->err;
  char buffer[JMSG_LENGTH_MAX];

  (*cinfo->err->format_message) (cinfo, buffer);

  g_set_error(&jerr->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "%s", buffer);
}

static void my_emit_message(j_common_ptr cinfo, int msg_level) {
  if (msg_level < 0) {
    // Warning message.  Convert to fatal error.
    (*cinfo->err->error_exit) (cinfo);
  }
}

// jerr->err will be set when setjmp returns again
struct jpeg_error_mgr *_openslide_jpeg_set_error_handler(struct _openslide_jpeg_error_mgr *jerr,
							 jmp_buf *env) {
  jpeg_std_error(&(jerr->pub));
  jerr->pub.error_exit = my_error_exit;
  jerr->pub.output_message = my_output_message;
  jerr->pub.emit_message = my_emit_message;
  jerr->env = env;
  jerr->err = NULL;

  return (struct jpeg_error_mgr *) jerr;
}

bool _openslide_jpeg_read_dimensions(const char *filename,
                                     int64_t offset,
                                     int32_t *w, int32_t *h,
                                     GError **err) {
  bool result = false;
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  FILE *f;
  jmp_buf env;

  // open file
  f = _openslide_fopen(filename, "rb", err);
  if (f == NULL) {
    return false;
  }
  if (offset && fseeko(f, offset, SEEK_SET) == -1) {
    _openslide_io_error(err, "Cannot seek to offset");
    fclose(f);
    return false;
  }

  if (setjmp(env) == 0) {
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
    jpeg_create_decompress(&cinfo);

    int header_result;

    _openslide_jpeg_stdio_src(&cinfo, f);
    header_result = jpeg_read_header(&cinfo, TRUE);
    if ((header_result != JPEG_HEADER_OK
	 && header_result != JPEG_HEADER_TABLES_ONLY)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG header");
      goto DONE;
    }

    jpeg_calc_output_dimensions(&cinfo);

    *w = cinfo.output_width;
    *h = cinfo.output_height;
    result = true;
  } else {
    // setjmp returned again
    g_propagate_error(err, jerr.err);
  }

DONE:
  // free buffers
  jpeg_destroy_decompress(&cinfo);
  fclose(f);

  return result;
}

static bool jpeg_decode(FILE *f,  // or:
                        const void *buf, uint32_t buflen,
                        void * const _dest, bool grayscale,
                        int32_t w, int32_t h,
                        GError **err) {
  bool result = false;
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  jmp_buf env;

  JSAMPARRAY buffer = (JSAMPARRAY) g_slice_alloc0(sizeof(JSAMPROW) * MAX_SAMP_FACTOR);

  if (setjmp(env) == 0) {
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
    jpeg_create_decompress(&cinfo);

    // set up I/O
    if (f) {
      _openslide_jpeg_stdio_src(&cinfo, f);
    } else {
      _openslide_jpeg_mem_src(&cinfo, (void *) buf, buflen);
    }

    // read header
    int header_result = jpeg_read_header(&cinfo, TRUE);
    if ((header_result != JPEG_HEADER_OK
	 && header_result != JPEG_HEADER_TABLES_ONLY)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG header");
      goto DONE;
    }

    cinfo.out_color_space = grayscale ? JCS_GRAYSCALE : JCS_RGB;

    jpeg_start_decompress(&cinfo);

    // ensure buffer dimensions are correct
    int32_t width = cinfo.output_width;
    int32_t height = cinfo.output_height;
    if (w != width || h != height) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Dimensional mismatch reading JPEG, "
                  "expected %dx%d, got %dx%d",
                  w, h, cinfo.output_width, cinfo.output_height);
      goto DONE;
    }

    // allocate scanline buffers
    for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
      buffer[i] = (JSAMPROW) g_malloc(sizeof(JSAMPLE)
				      * cinfo.output_width
				      * cinfo.output_components);
    }

    // decompress
    uint32_t *dest32 = _dest;
    uint8_t *dest8 = _dest;
    while (cinfo.output_scanline < cinfo.output_height) {
      JDIMENSION rows_read = jpeg_read_scanlines(&cinfo,
						 buffer,
						 cinfo.rec_outbuf_height);
      int cur_buffer = 0;
      while (rows_read > 0) {
        // copy a row
        int32_t i;
        if (cinfo.output_components == 1) {
          // grayscale
          for (i = 0; i < (int32_t) cinfo.output_width; i++) {
            dest8[i] = buffer[cur_buffer][i];
          }
          dest8 += cinfo.output_width;
        } else {
          // RGB
          for (i = 0; i < (int32_t) cinfo.output_width; i++) {
            dest32[i] = 0xFF000000 |                // A
              buffer[cur_buffer][i * 3 + 0] << 16 | // R
              buffer[cur_buffer][i * 3 + 1] << 8 |  // G
              buffer[cur_buffer][i * 3 + 2];        // B
          }
          dest32 += cinfo.output_width;
        }

	// advance 1 row
	rows_read--;
	cur_buffer++;
      }
    }
    result = true;
  } else {
    // setjmp has returned again
    g_propagate_error(err, jerr.err);
  }

DONE:
  // free buffers
  for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
    g_free(buffer[i]);
  }
  g_slice_free1(sizeof(JSAMPROW) * MAX_SAMP_FACTOR, buffer);

  jpeg_destroy_decompress(&cinfo);

  return result;
}

bool _openslide_jpeg_read(const char *filename,
                          int64_t offset,
                          uint32_t *dest,
                          int32_t w, int32_t h,
                          GError **err) {
  //g_debug("read JPEG: %s %" G_GINT64_FORMAT, filename, offset);

  FILE *f = _openslide_fopen(filename, "rb", err);
  if (f == NULL) {
    return false;
  }
  if (offset && fseeko(f, offset, SEEK_SET) == -1) {
    _openslide_io_error(err, "Cannot seek to offset");
    fclose(f);
    return false;
  }

  bool success = jpeg_decode(f, NULL, 0, dest, false, w, h, err);

  fclose(f);
  return success;
}

bool _openslide_jpeg_decode_buffer(const void *buf, uint32_t len,
                                   uint32_t *dest,
                                   int32_t w, int32_t h,
                                   GError **err) {
  //g_debug("decode JPEG buffer: %x %u", buf, len);

  return jpeg_decode(NULL, buf, len, dest, false, w, h, err);
}

bool _openslide_jpeg_decode_buffer_gray(const void *buf, uint32_t len,
                                        uint8_t *dest,
                                        int32_t w, int32_t h,
                                        GError **err) {
  //g_debug("decode grayscale JPEG buffer: %x %u", buf, len);

  return jpeg_decode(NULL, buf, len, dest, true, w, h, err);
}

static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dest,
                                      GError **err) {
  struct associated_image *img = (struct associated_image *) _img;

  //g_debug("read JPEG associated image: %s %" G_GINT64_FORMAT, img->filename, img->offset);

  return _openslide_jpeg_read(img->filename, img->offset, dest,
                              img->base.w, img->base.h, err);
}

static void destroy_associated_image(struct _openslide_associated_image *_img) {
  struct associated_image *img = (struct associated_image *) _img;

  g_free(img->filename);
  g_slice_free(struct associated_image, img);
}

static const struct _openslide_associated_image_ops jpeg_associated_ops = {
  .get_argb_data = get_associated_image_data,
  .destroy = destroy_associated_image,
};

bool _openslide_jpeg_add_associated_image(openslide_t *osr,
					  const char *name,
					  const char *filename,
					  int64_t offset,
					  GError **err) {
  int32_t w, h;
  if (!_openslide_jpeg_read_dimensions(filename, offset, &w, &h, err)) {
    return false;
  }

  struct associated_image *img = g_slice_new0(struct associated_image);
  img->base.ops = &jpeg_associated_ops;
  img->base.w = w;
  img->base.h = h;
  img->filename = g_strdup(filename);
  img->offset = offset;

  g_hash_table_insert(osr->associated_images, g_strdup(name), img);

  return true;
}
