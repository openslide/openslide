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

/*
 * Part of this file is:
 *
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * This file is part of the Independent JPEG Group's software.
 * For conditions of distribution and use, see the accompanying README file.
 */

/*
 * Hamamatsu (VMS, VMU, NDPI) support
 *
 * VMS/VMU quickhash comes from VMS/VMU file and map2 file
 * NDPI quickhash comes from _openslide_tifflike_init_properties_and_hash
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-tifflike.h"

#include <glib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <jpeglib.h>
#include <tiff.h>
#include <cairo.h>

#include "openslide-hash.h"

#define NGR_TILE_HEIGHT 64

// VMS/VMU
static const char GROUP_VMS[] = "Virtual Microscope Specimen";
static const char GROUP_VMU[] = "Uncompressed Virtual Microscope Specimen";
static const char KEY_MAP_FILE[] = "MapFile";
static const char KEY_IMAGE_FILE[] = "ImageFile";
static const char KEY_NUM_LAYERS[] = "NoLayers";
static const char KEY_NUM_JPEG_COLS[] = "NoJpegColumns";
static const char KEY_NUM_JPEG_ROWS[] = "NoJpegRows";
static const char KEY_OPTIMISATION_FILE[] = "OptimisationFile";
static const char KEY_MACRO_IMAGE[] = "MacroImage";
static const char KEY_BITS_PER_PIXEL[] = "BitsPerPixel";
static const char KEY_PIXEL_ORDER[] = "PixelOrder";

// NDPI
static const char NDPI_SOFTWARE[] = "NDP.scan";
#define NDPI_SOURCELENS 65421
#define NDPI_XOFFSET 65422
#define NDPI_YOFFSET 65423
#define NDPI_MCU_STARTS 65426
#define NDPI_REFERENCE 65427
#define NDPI_PROPERTY_MAP 65449
#define JPEG_MAX_DIMENSION_HIGH ((JPEG_MAX_DIMENSION >> 8) & 0xff)
#define JPEG_MAX_DIMENSION_LOW (JPEG_MAX_DIMENSION & 0xff)

struct jpeg {
  char *filename;
  int64_t start_in_file;
  int64_t end_in_file;

  int32_t width;
  int32_t height;
  int32_t tiles_across;
  int32_t tiles_down;
  int32_t tile_width;
  int32_t tile_height;

  int32_t tile_count;
  int64_t *mcu_starts;
  int64_t *unreliable_mcu_starts;

  int64_t sof_position;
};

struct jpeg_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  struct jpeg **jpegs;  // doesn't own the JPEGs
  int32_t jpegs_across;
  int32_t jpegs_down;

  int32_t tiles_across;
  int32_t tiles_down;
  int32_t tile_width;
  int32_t tile_height;

  int32_t scale_denom;
};

struct hamamatsu_jpeg_ops_data {
  int32_t jpeg_count;
  struct jpeg **all_jpegs;

  // thread stuff, for background search of restart markers
  GTimer *restart_marker_timer;
  GMutex *restart_marker_mutex;
  GThread *restart_marker_thread;

  GCond *restart_marker_cond;
  GMutex *restart_marker_cond_mutex;
  uint32_t restart_marker_users;
  bool restart_marker_thread_stop;
  GError *restart_marker_thread_error;
};

struct ngr_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  char *filename;

  int64_t start_in_file;

  int32_t column_width;
};

enum OpenSlideHamamatsuError {
  // JPEG does not contain restart markers
  OPENSLIDE_HAMAMATSU_ERROR_NO_RESTART_MARKERS,
};
static GQuark _openslide_hamamatsu_error_quark(void) {
  return g_quark_from_string("openslide-hamamatsu-error-quark");
}
#define OPENSLIDE_HAMAMATSU_ERROR _openslide_hamamatsu_error_quark()

/*
 * Source manager for doing fancy things with libjpeg and restart markers,
 * initially copied from jdatasrc.c from IJG libjpeg.
 */
struct my_src_mgr {
  struct jpeg_source_mgr pub;   /* public fields */

  JOCTET *buffer;               /* start of buffer */
  int buffer_size;
};

static void init_source (j_decompress_ptr cinfo G_GNUC_UNUSED) {
  /* nothing to be done */
}

static void skip_input_data (j_decompress_ptr cinfo, long num_bytes) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;

  src->pub.next_input_byte += (size_t) num_bytes;
  src->pub.bytes_in_buffer -= (size_t) num_bytes;
}


static void term_source (j_decompress_ptr cinfo G_GNUC_UNUSED) {
  /* nothing to do */
}

static bool jpeg_random_access_src(j_decompress_ptr cinfo,
                                   FILE *infile,
                                   int64_t header_start_position,
                                   int64_t sof_position,
                                   int64_t header_stop_position,
                                   int64_t start_position,
                                   int64_t stop_position,
                                   GError **err) {
  struct my_src_mgr *src;

  if (cinfo->src == NULL) {     /* first time for this JPEG object? */
    cinfo->src = (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo,
                                             JPOOL_PERMANENT,
                                             sizeof(struct my_src_mgr));
  }

  src = (struct my_src_mgr *) cinfo->src;
  src->pub.init_source = init_source;
  src->pub.fill_input_buffer = NULL;  /* this should never be called */
  src->pub.skip_input_data = skip_input_data;
  src->pub.resync_to_restart = jpeg_resync_to_restart; /* use default method */
  src->pub.term_source = term_source;

  // check for problems
  if ((0 > header_start_position) ||
      (header_start_position >= sof_position) ||
      (sof_position + 9 >= header_stop_position) ||
      (start_position != -1 &&
       ((header_stop_position > start_position) ||
        (start_position >= stop_position)))) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
               "Can't do random access JPEG read: "
	       "header_start_position: %" G_GINT64_FORMAT ", "
	       "sof_position: %" G_GINT64_FORMAT ", "
	       "header_stop_position: %" G_GINT64_FORMAT ", "
	       "start_position: %" G_GINT64_FORMAT ", "
	       "stop_position: %" G_GINT64_FORMAT,
	       header_start_position, sof_position, header_stop_position,
	       start_position, stop_position);

    src->buffer_size = 0;
    src->pub.bytes_in_buffer = 0;
    src->buffer = NULL;
    return false;
  }

  // compute size of buffer and allocate
  int header_length = header_stop_position - header_start_position;
  int data_length = 0;
  if (start_position != -1) {
    data_length = stop_position - start_position;
  }

  src->buffer_size = header_length + data_length;
  src->pub.bytes_in_buffer = src->buffer_size;
  src->buffer = g_slice_alloc(src->buffer_size);

  src->pub.next_input_byte = src->buffer;

  // read in the 2 parts
  //  g_debug("reading header from %"G_GINT64_FORMAT, header_start_position);
  fseeko(infile, header_start_position, SEEK_SET);
  if (!fread(src->buffer, header_length, 1, infile)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot read header in JPEG");
    return false;
  }

  if (data_length) {
    //  g_debug("reading from %" G_GINT64_FORMAT, start_position);
    fseeko(infile, start_position, SEEK_SET);
    if (!fread(src->buffer + header_length, data_length, 1, infile)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot read data in JPEG");
      return false;
    }

    // change the final byte to EOI
    if (src->buffer[src->buffer_size - 2] != 0xFF) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Expected 0xFF byte at end of JPEG data");
      return false;
    }
    src->buffer[src->buffer_size - 1] = JPEG_EOI;
  }

  // check for overlarge or 0 X/Y in SOF (some NDPI JPEGs have this)
  // change them to a value libjpeg will accept
  int64_t size_offset = sof_position - header_start_position + 5;
  uint16_t y = (src->buffer[size_offset + 0] << 8) +
                src->buffer[size_offset + 1];
  if (y > JPEG_MAX_DIMENSION || y == 0) {
    //g_debug("fixing up SOF Y");
    src->buffer[size_offset + 0] = JPEG_MAX_DIMENSION_HIGH;
    src->buffer[size_offset + 1] = JPEG_MAX_DIMENSION_LOW;
  }
  uint16_t x = (src->buffer[size_offset + 2] << 8) +
                src->buffer[size_offset + 3];
  if (x > JPEG_MAX_DIMENSION || x == 0) {
    //g_debug("fixing up SOF X");
    src->buffer[size_offset + 2] = JPEG_MAX_DIMENSION_HIGH;
    src->buffer[size_offset + 3] = JPEG_MAX_DIMENSION_LOW;
  }

  return true;
}

// does not destroy cinfo
static void random_access_src_destroy(j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;   // sorry
  g_slice_free1(src->buffer_size, src->buffer);
}

static void jpeg_level_free(gpointer data) {
  //g_debug("level_free: %p", data);
  struct jpeg_level *l = data;
  g_free(l->jpegs);
  _openslide_grid_destroy(l->grid);
  g_slice_free(struct jpeg_level, l);
}

static void jpeg_destroy_data(int32_t num_jpegs, struct jpeg **jpegs,
                              int32_t level_count,
                              struct jpeg_level **levels) {
  // each jpeg in turn
  for (int32_t i = 0; i < num_jpegs; i++) {
    struct jpeg *jpeg = jpegs[i];
    g_free(jpeg->filename);
    g_free(jpeg->mcu_starts);
    g_free(jpeg->unreliable_mcu_starts);
    g_slice_free(struct jpeg, jpeg);
  }

  // the JPEG array
  g_free(jpegs);

  // each level in turn
  for (int32_t i = 0; i < level_count; i++) {
    jpeg_level_free(levels[i]);
  }

  // the level array
  g_free(levels);
}

static bool find_bitstream_start(FILE *f,
                                 int64_t *sof_position,
                                 GError **err) {
  uint8_t buf[2];
  uint8_t marker_byte;
  uint16_t len;
  int64_t pos;

  while (true) {
    // read marker
    pos = ftello(f);
    if (fread(buf, sizeof(buf), 1, f) != 1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Couldn't read JPEG marker");
      return false;
    }
    if (buf[0] != 0xFF) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Expected marker at %"G_GINT64_FORMAT", found none", pos);
      return false;
    }
    marker_byte = buf[1];
    if (marker_byte == 0xD8) {
      // SOI; no marker segment
      continue;
    }

    // check for SOF
    if ((marker_byte >= 0xC0 && marker_byte <= 0xC3) ||
        (marker_byte >= 0xC5 && marker_byte <= 0xC7) ||
        (marker_byte >= 0xC9 && marker_byte <= 0xCB) ||
        (marker_byte >= 0xCD && marker_byte <= 0xCF)) {
      *sof_position = pos;
    }

    // read length
    if (fread(buf, sizeof(buf), 1, f) != 1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Couldn't read JPEG marker length");
      return false;
    }
    memcpy(&len, buf, sizeof(len));
    len = GUINT16_FROM_BE(len);

    // seek
    if (fseek(f, pos + sizeof(buf) + len, SEEK_SET)) {
      _openslide_io_error(err, "Couldn't seek");
      return false;
    }

    // check for SOS
    if (marker_byte == 0xDA) {
      // found it; done
      //g_debug("found bitstream start at %"G_GINT64_FORMAT, ftello(f));
      return true;
    }
  }
}

static uint8_t find_next_ff_marker(FILE *f,
				   uint8_t *buf_start,
				   uint8_t **buf,
				   int buf_size,
				   int64_t file_size,
				   int64_t *after_marker_pos,
				   int *bytes_in_buf) {
  //g_debug("bytes_in_buf: %d", *bytes_in_buf);
  int64_t file_pos = ftello(f);
  bool last_was_ff = false;
  *after_marker_pos = -1;
  while (true) {
    if (*bytes_in_buf == 0) {
      // fill buffer
      *buf = buf_start;
      int bytes_to_read = MIN(buf_size, file_size - file_pos);

      //g_debug("bytes_to_read: %d", bytes_to_read);
      size_t result = fread(*buf, bytes_to_read, 1, f);
      if (result == 0) {
	return 0;
      }

      file_pos += bytes_to_read;
      *bytes_in_buf = bytes_to_read;
    }

    // special case where the last time ended with FF
    if (last_was_ff) {
      //g_debug("last_was_ff");
      uint8_t marker = (*buf)[0];
      (*buf)++;
      (*bytes_in_buf)--;
      *after_marker_pos = file_pos - *bytes_in_buf;
      return marker;
    }

    // search for ff
    uint8_t *ff = memchr(*buf, 0xFF, *bytes_in_buf);
    if (ff == NULL) {
      // keep searching
      *bytes_in_buf = 0;
    } else {
      // ff found, advance buffer to consume everything including ff
      int offset = ff - *buf + 1;
      *bytes_in_buf -= offset;
      *buf += offset;
      g_assert(*bytes_in_buf >= 0);

      if (*bytes_in_buf == 0) {
	last_was_ff = true;
      } else {
	(*bytes_in_buf)--;
	(*buf)++;
	*after_marker_pos = file_pos - *bytes_in_buf;
	return ff[1];
      }
    }
  }
}

static bool _compute_mcu_start(struct jpeg *jpeg,
			       FILE *f,
			       int64_t target,
			       GError **err) {
  // special case for first
  if (jpeg->mcu_starts[0] == -1) {
    // walk through marker segments in header
    fseeko(f, jpeg->start_in_file, SEEK_SET);

    if (!find_bitstream_start(f, &jpeg->sof_position, err)) {
      g_prefix_error(err, "Reading JPEG header: ");
      return false;
    }

    // set the first entry
    jpeg->mcu_starts[0] = ftello(f);
  }

  // walk backwards to find the first non -1 offset
  int64_t first_good;
  for (first_good = target; jpeg->mcu_starts[first_good] == -1; first_good--) {
    // if we have an unreliable_mcu_start, validate it and use it
    int64_t offset = -1;
    if (jpeg->unreliable_mcu_starts != NULL) {
      offset = jpeg->unreliable_mcu_starts[first_good];
    }
    if (offset != -1) {
      uint8_t buf[2];
      fseeko(f, offset - 2, SEEK_SET);

      size_t result = fread(buf, 2, 1, f);
      if (result == 0 ||
          buf[0] != 0xFF || buf[1] < 0xD0 || buf[1] > 0xD7) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Restart marker not found in expected place");
        return false;
      }

      //  g_debug("accepted unreliable marker %"G_GINT64_FORMAT, first_good);
      jpeg->mcu_starts[first_good] = offset;
      break;
    }
  }

  if (first_good == target) {
    // we're done
    return true;
  }
  //  g_debug("target: %"G_GINT64_FORMAT", first_good: %"G_GINT64_FORMAT, target, first_good);

  // now search for the new restart markers
  fseeko(f, jpeg->mcu_starts[first_good], SEEK_SET);

  uint8_t buf_start[4096];
  uint8_t *buf = buf_start;
  int bytes_in_buf = 0;
  while (first_good < target) {
    int64_t after_marker_pos;
    uint8_t b = find_next_ff_marker(f, buf_start, &buf, 4096,
				    jpeg->end_in_file,
				    &after_marker_pos,
				    &bytes_in_buf);
    g_assert(after_marker_pos > 0 || after_marker_pos == -1);
    if (after_marker_pos == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "after_marker_pos == -1");
      return false;
    }
    //g_debug("after_marker_pos: %" G_GINT64_FORMAT, after_marker_pos);

    // EOI?
    if (b == JPEG_EOI) {
      // we're done
      break;
    } else if (b >= 0xD0 && b < 0xD8) {
      // marker
      jpeg->mcu_starts[1 + first_good++] = after_marker_pos;
    }
  }
  return true;
}

static bool compute_mcu_start(openslide_t *osr,
			      struct jpeg *jpeg,
			      FILE *f,
			      int64_t tileno,
			      int64_t *sof_position,
			      int64_t *header_stop_position,
			      int64_t *start_position,
			      int64_t *stop_position,
			      GError **err) {
  struct hamamatsu_jpeg_ops_data *data = osr->data;
  bool success = false;

  if (tileno < 0 || tileno >= jpeg->tile_count) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid tileno %"G_GINT64_FORMAT, tileno);
    return false;
  }

  g_mutex_lock(data->restart_marker_mutex);

  if (!_compute_mcu_start(jpeg, f, tileno, err)) {
    goto OUT;
  }

  // SOF position; always computed by _compute_mcu_start
  if (sof_position) {
    *sof_position = jpeg->sof_position;
  }

  // end of header; always computed by _compute_mcu_start
  if (header_stop_position) {
    *header_stop_position = jpeg->mcu_starts[0];
    g_assert(*header_stop_position != -1);
  }

  // start of data stream
  if (start_position) {
    *start_position = jpeg->mcu_starts[tileno];
    g_assert(*start_position != -1);
  }

  // end of data stream
  if (stop_position) {
    if (jpeg->tile_count == tileno + 1) {
      // EOF
      *stop_position = jpeg->end_in_file;
    } else {
      if (!_compute_mcu_start(jpeg, f, tileno + 1, err)) {
        goto OUT;
      }
      *stop_position = jpeg->mcu_starts[tileno + 1];
    }
    g_assert(*stop_position != -1);
  }

  success = true;

OUT:
  g_mutex_unlock(data->restart_marker_mutex);
  return success;
}

static bool read_from_jpeg(openslide_t *osr,
                           struct jpeg *jpeg,
                           int32_t tileno,
                           int32_t scale_denom,
                           uint32_t * const dest,
                           int32_t w, int32_t h,
                           GError **err) {
  bool success = false;

  // open file
  FILE *f = _openslide_fopen(jpeg->filename, "rb", err);
  if (f == NULL) {
    return false;
  }

  // begin decompress
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  jmp_buf env;

  gsize row_size = 0;

  JSAMPARRAY buffer = (JSAMPARRAY) g_slice_alloc0(sizeof(JSAMPROW) * MAX_SAMP_FACTOR);
  cinfo.rec_outbuf_height = 0;

  if (setjmp(env) == 0) {
    // figure out where to start the data stream
    int64_t sof_position;
    int64_t header_stop_position;
    int64_t start_position;
    int64_t stop_position;
    if (!compute_mcu_start(osr, jpeg, f, tileno,
                           &sof_position,
                           &header_stop_position,
                           &start_position,
                           &stop_position,
                           err)) {
      goto OUT;
    }

    // set error handler, this will longjmp if necessary
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);

    // start decompressing
    jpeg_create_decompress(&cinfo);

    if (!jpeg_random_access_src(&cinfo, f,
                                jpeg->start_in_file,
                                sof_position,
                                header_stop_position,
                                start_position,
                                stop_position,
                                err)) {
      goto OUT_JPEG;
    }

    jpeg_read_header(&cinfo, TRUE);
    cinfo.scale_num = 1;
    cinfo.scale_denom = scale_denom;
    cinfo.image_width = jpeg->tile_width;  // cunning
    cinfo.image_height = jpeg->tile_height;
    cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    //    g_debug("output_width: %d", cinfo.output_width);
    //    g_debug("output_height: %d", cinfo.output_height);

    // allocate scanline buffers
    row_size = sizeof(JSAMPLE) * cinfo.output_width * cinfo.output_components;
    for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
      buffer[i] = (JSAMPROW) g_slice_alloc(row_size);
      //g_debug("buffer[%d]: %p", i, buffer[i]);
    }

    if ((cinfo.output_width != (unsigned int) w) || (cinfo.output_height != (unsigned int) h)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Dimensional mismatch in read_from_jpeg, "
                  "expected %dx%d, got %dx%d",
                  w, h, cinfo.output_width, cinfo.output_height);
      goto OUT_JPEG;
    }

    // decompress
    uint32_t *jpeg_dest = dest;
    while (cinfo.output_scanline < cinfo.output_height) {
      JDIMENSION rows_read = jpeg_read_scanlines(&cinfo,
                                                 buffer,
                                                 cinfo.rec_outbuf_height);
      //g_debug("just read scanline %d", cinfo.output_scanline - rows_read);
      //g_debug(" rows read: %d", rows_read);
      int cur_buffer = 0;
      while (rows_read > 0) {
        // copy a row
        int32_t dest_i = 0;
        for (int32_t i = 0; i < w; i++) {
          jpeg_dest[dest_i++] = 0xFF000000 |      // A
            buffer[cur_buffer][i * 3 + 0] << 16 | // R
            buffer[cur_buffer][i * 3 + 1] << 8 |  // G
            buffer[cur_buffer][i * 3 + 2];        // B
        }

        // advance everything 1 row
        cur_buffer++;
        jpeg_dest += cinfo.output_width;
        rows_read--;
      }
    }
    success = true;
  } else {
    // setjmp returns again
    g_propagate_prefixed_error(err, jerr.err, "JPEG decompression failed: ");
  }

OUT_JPEG:
  (void) 0; // dummy statement for label

  // stop jpeg
  random_access_src_destroy(&cinfo);
  jpeg_destroy_decompress(&cinfo);

OUT:
  // free buffers
  for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
    g_slice_free1(row_size, buffer[i]);
  }
  g_slice_free1(sizeof(JSAMPROW) * MAX_SAMP_FACTOR, buffer);

  fclose(f);

  return success;
}

static bool read_jpeg_tile(openslide_t *osr,
                           cairo_t *cr,
                           struct _openslide_level *level,
                           int64_t tile_col, int64_t tile_row,
                           void *arg G_GNUC_UNUSED,
                           GError **err) {
  struct jpeg_level *l = (struct jpeg_level *) level;

  int32_t jpeg_col = tile_col / l->jpegs[0]->tiles_across;
  int32_t jpeg_row = tile_row / l->jpegs[0]->tiles_down;
  int32_t local_tile_col = tile_col % l->jpegs[0]->tiles_across;
  int32_t local_tile_row = tile_row % l->jpegs[0]->tiles_down;

  // grid should ensure tile col/row are in bounds
  g_assert(jpeg_col >= 0 && jpeg_col < l->jpegs_across);
  g_assert(jpeg_row >= 0 && jpeg_row < l->jpegs_down);

  struct jpeg *jp = l->jpegs[jpeg_row * l->jpegs_across + jpeg_col];
  int32_t tileno = local_tile_row * jp->tiles_across + local_tile_col;

  int32_t tw = l->tile_width;
  int32_t th = l->tile_height;

  //g_debug("hamamatsu read_tile: jpeg %d %d, local %d %d, tile %d, dim %d %d", jpeg_col, jpeg_row, local_tile_col, local_tile_row, tileno, tw, th);

  // get the jpeg data, possibly from cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);

  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    if (!read_from_jpeg(osr,
                        jp, tileno,
                        l->scale_denom,
                        tiledata, tw, th,
                        err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    _openslide_cache_put(osr->cache,
			 level, tile_col, tile_row,
			 tiledata,
			 tw * th * 4,
			 &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
								 CAIRO_FORMAT_RGB24,
								 tw, th,
								 tw * 4);

  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  //_openslide_grid_label_tile(l->grid, cr, tile_col, tile_row);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}


static bool jpeg_paint_region(openslide_t *osr, cairo_t *cr,
                              int64_t x, int64_t y,
                              struct _openslide_level *level,
                              int32_t w, int32_t h,
                              GError **err) {
  struct hamamatsu_jpeg_ops_data *data = osr->data;
  struct jpeg_level *l = (struct jpeg_level *) level;

  g_mutex_lock(data->restart_marker_cond_mutex);
  // check for background errors
  if (data->restart_marker_thread_error) {
    // propagate error
    g_propagate_error(err, data->restart_marker_thread_error);
    data->restart_marker_thread_error = NULL;
    g_mutex_unlock(data->restart_marker_cond_mutex);
    return false;
  }
  // tell the background thread to pause
  data->restart_marker_users++;
  //  g_debug("telling thread to pause");
  g_mutex_unlock(data->restart_marker_cond_mutex);

  // paint
  bool success = _openslide_grid_paint_region(l->grid, cr, NULL,
                                              x / level->downsample,
                                              y / level->downsample,
                                              level, w, h,
                                              err);

  // maybe tell the background thread to resume
  g_mutex_lock(data->restart_marker_cond_mutex);
  if (!--data->restart_marker_users) {
    g_timer_start(data->restart_marker_timer);
    //  g_debug("telling thread to awaken");
    g_cond_signal(data->restart_marker_cond);
  }
  g_mutex_unlock(data->restart_marker_cond_mutex);

  return success;
}

static void jpeg_do_destroy(openslide_t *osr) {
  struct hamamatsu_jpeg_ops_data *data = osr->data;

  // tell the thread to finish and wait
  g_mutex_lock(data->restart_marker_cond_mutex);
  g_warn_if_fail(data->restart_marker_users == 0);
  data->restart_marker_thread_stop = true;
  g_cond_signal(data->restart_marker_cond);
  g_mutex_unlock(data->restart_marker_cond_mutex);
  if (data->restart_marker_thread) {
    g_thread_join(data->restart_marker_thread);
  }

  // jpegs and levels
  jpeg_destroy_data(data->jpeg_count, data->all_jpegs,
                    osr->level_count, (struct jpeg_level **) osr->levels);

  // the background stuff
  g_mutex_lock(data->restart_marker_cond_mutex);
  if (data->restart_marker_thread_error) {
    g_error_free(data->restart_marker_thread_error);
  }
  g_mutex_unlock(data->restart_marker_cond_mutex);
  g_mutex_free(data->restart_marker_mutex);
  g_timer_destroy(data->restart_marker_timer);
  g_cond_free(data->restart_marker_cond);
  g_mutex_free(data->restart_marker_cond_mutex);

  // the structure
  g_slice_free(struct hamamatsu_jpeg_ops_data, data);
}

static const struct _openslide_ops hamamatsu_jpeg_ops = {
  .paint_region = jpeg_paint_region,
  .destroy = jpeg_do_destroy,
};

static gint width_compare(gconstpointer a, gconstpointer b) {
  int64_t w1 = *((const int64_t *) a);
  int64_t w2 = *((const int64_t *) b);

  g_assert(w1 >= 0 && w2 >= 0);

  return (w1 < w2) - (w1 > w2);
}

// warning: calls g_assert for trivial things, use only for debugging
static void verify_mcu_starts(struct hamamatsu_jpeg_ops_data *data) {
  g_debug("verifying mcu starts");

  int32_t current_jpeg = 0;
  int32_t current_mcu_start = 0;

  while(current_jpeg < data->jpeg_count) {
    struct jpeg *jp = data->all_jpegs[current_jpeg];

    g_assert(jp->filename);
    if (current_mcu_start > 0) {
      int64_t offset = jp->mcu_starts[current_mcu_start];
      g_assert(offset != -1);
      FILE *f = _openslide_fopen(jp->filename, "rb", NULL);
      g_assert(f);
      fseeko(f, offset - 2, SEEK_SET);
      g_assert(getc(f) == 0xFF);
      int marker = getc(f);
      g_assert(marker >= 0xD0 && marker <= 0xD7);
      fclose(f);
    }

    current_mcu_start++;
    if (current_mcu_start >= jp->tile_count) {
      current_mcu_start = 0;
      current_jpeg++;
      g_debug("done verifying jpeg %d", current_jpeg);
    }
  }
}

static gpointer restart_marker_thread_func(gpointer d) {
  openslide_t *osr = d;
  struct hamamatsu_jpeg_ops_data *data = osr->data;

  int32_t current_jpeg = 0;
  int32_t current_mcu_start = 0;

  FILE *current_file = NULL;

  GError *tmp_err = NULL;

  while(current_jpeg < data->jpeg_count) {
    g_mutex_lock(data->restart_marker_cond_mutex);

    // should we pause?
    while (data->restart_marker_users && !data->restart_marker_thread_stop) {
      //      g_debug("thread paused");
      g_cond_wait(data->restart_marker_cond,
		  data->restart_marker_cond_mutex); // zzz
      //      g_debug("thread awoken");
    }

    // should we stop?
    if (data->restart_marker_thread_stop) {
      //      g_debug("thread stopping");
      g_mutex_unlock(data->restart_marker_cond_mutex);
      break;
    }

    // should we sleep?
    double time_to_sleep = 1.0 - g_timer_elapsed(data->restart_marker_timer,
						 NULL);
    if (time_to_sleep > 0) {
      GTimeVal abstime;
      gulong sleep_time = G_USEC_PER_SEC * time_to_sleep;

      g_get_current_time(&abstime);
      g_time_val_add(&abstime, sleep_time);

      //      g_debug("zz: %lu", sleep_time);
      g_cond_timed_wait(data->restart_marker_cond,
			data->restart_marker_cond_mutex,
			&abstime);
      //      g_debug("running again");
      g_mutex_unlock(data->restart_marker_cond_mutex);
      continue;
    }

    // we are finally able to run
    g_mutex_unlock(data->restart_marker_cond_mutex);

    //g_debug("current_jpeg: %d, current_mcu_start: %d",
    //        current_jpeg, current_mcu_start);

    struct jpeg *jp = data->all_jpegs[current_jpeg];
    if (jp->tile_count > 1) {
      if (current_file == NULL) {
	current_file = _openslide_fopen(jp->filename, "rb", &tmp_err);
	if (current_file == NULL) {
	  //g_debug("restart_marker_thread_func fopen failed");
	  break;
	}
      }

      if (!compute_mcu_start(osr, jp, current_file, current_mcu_start,
                             NULL, NULL, NULL, NULL, &tmp_err)) {
        //g_debug("restart_marker_thread_func compute_mcu_start failed");
        fclose(current_file);
        break;
      }

      current_mcu_start++;
      if (current_mcu_start >= jp->tile_count) {
	current_mcu_start = 0;
	current_jpeg++;
	fclose(current_file);
	current_file = NULL;
      }
    } else {
      current_jpeg++;
    }
  }

  // store error, if any
  if (tmp_err) {
    g_mutex_lock(data->restart_marker_cond_mutex);
    data->restart_marker_thread_error = tmp_err;
    g_mutex_unlock(data->restart_marker_cond_mutex);
  }

  //  g_debug("restart_marker_thread_func done!");
  return NULL;
}

// returns w and h and tw and th and comment as a convenience
// if !use_jpeg_dimensions, use *w and *h instead of setting them
static bool verify_jpeg(FILE *f, bool use_jpeg_dimensions,
			int32_t *w, int32_t *h,
			int32_t *tw, int32_t *th,
			char **comment, GError **err) {
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  jmp_buf env;
  bool success = false;

  if (comment) {
    *comment = NULL;
  }

  // find limits of JPEG header
  int64_t header_start = ftello(f);
  int64_t sof_position;
  if (!find_bitstream_start(f, &sof_position, err)) {
    return false;
  }
  int64_t header_stop = ftello(f);

  if (setjmp(env) == 0) {
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
    jpeg_create_decompress(&cinfo);
    if (!jpeg_random_access_src(&cinfo, f,
                                header_start, sof_position, header_stop,
                                -1, -1, err)) {
      goto DONE;
    }

    int header_result;

    if (comment) {
      // extract comment
      jpeg_save_markers(&cinfo, JPEG_COM, 0xFFFF);
    }

    header_result = jpeg_read_header(&cinfo, TRUE);
    if (header_result != JPEG_HEADER_OK
        && header_result != JPEG_HEADER_TABLES_ONLY) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Couldn't read JPEG header");
      goto DONE;
    }
    if (cinfo.num_components != 3) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "JPEG color components != 3");
      goto DONE;
    }
    if (cinfo.restart_interval == 0) {
      g_set_error(err, OPENSLIDE_HAMAMATSU_ERROR,
                  OPENSLIDE_HAMAMATSU_ERROR_NO_RESTART_MARKERS,
                  "No restart markers");
      goto DONE;
    }

    jpeg_start_decompress(&cinfo);

    if (comment) {
      if (cinfo.marker_list) {
	// copy everything out
	char *com = g_strndup((const gchar *) cinfo.marker_list->data,
			      cinfo.marker_list->data_length);
	// but only really save everything up to the first '\0'
	*comment = g_strdup(com);
	g_free(com);
      }
      jpeg_save_markers(&cinfo, JPEG_COM, 0);  // stop saving
    }

    if (use_jpeg_dimensions) {
      *w = cinfo.output_width;
      *h = cinfo.output_height;
    }

    int32_t mcu_width = DCTSIZE;
    int32_t mcu_height = DCTSIZE;
    if (cinfo.comps_in_scan > 1) {
      mcu_width = cinfo.max_h_samp_factor * DCTSIZE;
      mcu_height = cinfo.max_v_samp_factor * DCTSIZE;
    }

    // don't trust cinfo.MCUs_per_row, since it's based on libjpeg's belief
    // about the image width instead of the actual value
    uint32_t mcus_per_row = (*w / mcu_width) + !!(*w % mcu_width);

    if (cinfo.restart_interval > mcus_per_row) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Restart interval greater than MCUs per row");
      goto DONE;
    }

    int leftover_mcus = mcus_per_row % cinfo.restart_interval;
    if (leftover_mcus != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Inconsistent restart marker spacing within row");
      goto DONE;
    }

    *tw = mcu_width * cinfo.restart_interval;
    *th = mcu_height;

    //g_debug("size: %d %d, tile size: %d %d, mcu size: %d %d, restart_interval: %d, mcus_per_row: %u, leftover mcus: %d", *w, *h, *tw, *th, mcu_width, mcu_height, cinfo.restart_interval, mcus_per_row, leftover_mcus);
  } else {
    // setjmp has returned again
    g_propagate_error(err, jerr.err);
    goto DONE;
  }

  success = true;

DONE:
  random_access_src_destroy(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return success;
}

static int64_t *extract_one_optimisation(FILE *opt_f,
                                         int32_t tiles_down,
                                         int32_t tiles_across) {
  int32_t tile_count = tiles_across * tiles_down;
  int64_t *mcu_starts = g_new(int64_t, tile_count);
  for (int32_t i = 0; i < tile_count; i++) {
    mcu_starts[i] = -1; // UNKNOWN value
  }

  // optimisation file is in a weird format, it is 32- (or 64- or 320- ?) bit
  // little endian values, giving the file offset into an MCU row,
  // each offset starts at a 40-byte alignment, and the last row (of the
  // entire file, not each image) seems to be missing

  // also, the offsets are all packed into 1 file, even with multiple images

  // we will read the file and verify at least that the markers
  // are valid, if anything is fishy, we will not use it

  // we represent missing data by -1, which we initialize to,
  // so if we run out of opt file, we can just stop

  for (int32_t row = 0; row < tiles_down; row++) {
    // read 40 bytes
    union {
      uint8_t buf[40];
      int64_t i64;
    } u;

    if (fread(u.buf, 40, 1, opt_f) != 1) {
      // EOF or error, we've done all we can

      if (row == 0) {
	// if we don't even get the first one, deallocate
	goto FAIL;
      }

      break;
    }

    // get the offset
    int64_t offset = GINT64_FROM_LE(u.i64);

    // record this marker
    mcu_starts[row * tiles_across] = offset;
  }

  return mcu_starts;

 FAIL:
  g_free(mcu_starts);
  return NULL;
}

static void add_properties(GHashTable *ht, GKeyFile *kf,
			   const char *group) {
  g_hash_table_insert(ht,
		      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
		      g_strdup("hamamatsu"));

  char **keys = g_key_file_get_keys(kf, group, NULL, NULL);
  if (keys == NULL) {
    return;
  }

  for (char **key = keys; *key != NULL; key++) {
    char *value = g_key_file_get_value(kf, group, *key, NULL);
    if (value) {
      g_hash_table_insert(ht,
			  g_strdup_printf("hamamatsu.%s", *key),
			  g_strdup(value));
      g_free(value);
    }
  }

  g_strfreev(keys);

  // this allows openslide.objective-power to have a fractional component
  // but it's better than rounding
  _openslide_duplicate_double_prop(ht, "hamamatsu.SourceLens",
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  // TODO: can we calculate MPP from PhysicalWidth/PhysicalHeight?
}

// create scale_denom levels
static void create_scaled_jpeg_levels(openslide_t *osr,
                                      int32_t *_level_count,
                                      struct jpeg_level ***_levels) {
  int32_t level_count = *_level_count;
  struct jpeg_level **levels = *_levels;

  GHashTable *expanded_levels = g_hash_table_new_full(_openslide_int64_hash,
                                                      _openslide_int64_equal,
                                                      _openslide_int64_free,
                                                      jpeg_level_free);

  for (int32_t i = 0; i < level_count; i++) {
    struct jpeg_level *l = levels[i];

    // add level
    int64_t *key = g_slice_new(int64_t);
    *key = l->base.w;
    g_hash_table_insert(expanded_levels, key, l);

    // try adding scale_denom levels
    for (int scale_denom = 2; scale_denom <= 8; scale_denom <<= 1) {
      // check to make sure we get an even division
      if ((l->tile_width % scale_denom) ||
          (l->tile_height % scale_denom)) {
        continue;
      }

      // create a derived level
      struct jpeg_level *sd_l = g_slice_new0(struct jpeg_level);
      sd_l->scale_denom = scale_denom;

      sd_l->base.w = l->base.w / scale_denom;
      sd_l->base.h = l->base.h / scale_denom;
      sd_l->jpegs_across = l->jpegs_across;
      sd_l->jpegs_down = l->jpegs_down;
      sd_l->tiles_across = l->tiles_across;
      sd_l->tiles_down = l->tiles_down;
      sd_l->tile_width = l->tile_width / scale_denom;
      sd_l->tile_height = l->tile_height / scale_denom;

      int32_t num_jpegs = sd_l->jpegs_across * sd_l->jpegs_down;
      sd_l->jpegs = g_new(struct jpeg *, num_jpegs);
      for (int32_t j = 0; j < num_jpegs; j++) {
        sd_l->jpegs[j] = l->jpegs[j];
      }

      // tile size hints
      sd_l->base.tile_w = sd_l->tile_width;
      sd_l->base.tile_h = sd_l->tile_height;

      // create grid
      sd_l->grid = _openslide_grid_create_simple(osr,
                                                 sd_l->tiles_across,
                                                 sd_l->tiles_down,
                                                 sd_l->tile_width,
                                                 sd_l->tile_height,
                                                 read_jpeg_tile);

      key = g_slice_new(int64_t);
      *key = sd_l->base.w;
      g_hash_table_insert(expanded_levels, key, sd_l);
    }
  }
  g_free(levels);

  // get sorted keys
  GList *level_keys = g_hash_table_get_keys(expanded_levels);
  level_keys = g_list_sort(level_keys, width_compare);
  //g_debug("number of keys: %d", g_list_length(level_keys));

  // get level count
  level_count = g_hash_table_size(expanded_levels);

  // create new level array
  levels = g_new(struct jpeg_level *, level_count);

  // load it
  int i = 0;
  while (level_keys != NULL) {
    // get a value
    struct jpeg_level *l = g_hash_table_lookup(expanded_levels,
                                               level_keys->data);

    // move
    levels[i] = l;
    g_hash_table_steal(expanded_levels, level_keys->data);
    _openslide_int64_free(level_keys->data);  // key

    // consume the head and continue
    level_keys = g_list_delete_link(level_keys, level_keys);
    i++;
  }
  g_hash_table_unref(expanded_levels);

  // return results
  *_level_count = level_count;
  *_levels = levels;
}

static void init_jpeg_ops(openslide_t *osr,
                          int32_t level_count, struct jpeg_level **levels,
                          int32_t num_jpegs, struct jpeg **jpegs,
                          bool background_thread) {
  // allocate private data
  g_assert(osr->data == NULL);
  struct hamamatsu_jpeg_ops_data *data =
    g_slice_new0(struct hamamatsu_jpeg_ops_data);
  data->jpeg_count = num_jpegs;
  data->all_jpegs = jpegs;
  osr->data = data;

  // create scale_denom levels
  create_scaled_jpeg_levels(osr, &level_count, &levels);

  // populate the level count and array
  g_assert(osr->levels == NULL);
  osr->level_count = level_count;
  osr->levels = (struct _openslide_level **) levels;

  // init background thread for finding restart markers
  data->restart_marker_timer = g_timer_new();
  data->restart_marker_mutex = g_mutex_new();
  data->restart_marker_cond = g_cond_new();
  data->restart_marker_cond_mutex = g_mutex_new();
  if (background_thread) {
    data->restart_marker_thread = g_thread_create(restart_marker_thread_func,
                                                  osr,
                                                  TRUE,
                                                  NULL);
  }

  // for debugging
  if (false) {
    if (background_thread) {
      g_thread_join(data->restart_marker_thread);
      data->restart_marker_thread = NULL;
    } else {
      restart_marker_thread_func(osr);
    }
    verify_mcu_starts(data);
  }

  // set ops
  osr->ops = &hamamatsu_jpeg_ops;
}

static struct jpeg_level *create_jpeg_level(openslide_t *osr,
                                            struct jpeg **jpegs,
                                            int32_t jpeg_cols,
                                            int32_t jpeg_rows) {
  struct jpeg_level *l = g_slice_new0(struct jpeg_level);

  // accumulate dimensions
  for (int32_t x = 0; x < jpeg_cols; x++) {
    struct jpeg *jp = jpegs[x];
    l->base.w += jp->width;
    l->tiles_across += jp->tiles_across;
  }
  for (int32_t y = 0; y < jpeg_rows; y++) {
    struct jpeg *jp = jpegs[y * jpeg_cols];
    l->base.h += jp->height;
    l->tiles_down += jp->tiles_down;
  }

  // init values
  l->jpegs_across = jpeg_cols;
  l->jpegs_down = jpeg_rows;
  l->tile_width = jpegs[0]->tile_width;
  l->tile_height = jpegs[0]->tile_height;
  l->scale_denom = 1;

  // jpeg array
  int32_t num_jpegs = l->jpegs_across * l->jpegs_down;
  l->jpegs = g_new(struct jpeg *, num_jpegs);
  for (int32_t i = 0; i < num_jpegs; i++) {
    l->jpegs[i] = jpegs[i];
  }

  // tile size hints
  l->base.tile_w = l->tile_width;
  l->base.tile_h = l->tile_height;

  // create grid
  l->grid = _openslide_grid_create_simple(osr,
                                          l->tiles_across, l->tiles_down,
                                          l->tile_width, l->tile_height,
                                          read_jpeg_tile);

  return l;
}

static bool hamamatsu_vms_part2(openslide_t *osr,
				int num_jpegs, char **image_filenames,
				int num_jpeg_cols, int num_jpeg_rows,
				FILE *optimisation_file,
				GError **err) {
  struct jpeg_level **levels = NULL;
  int32_t level_count = 0;
  bool success = false;

  // initialize individual jpeg structs
  struct jpeg **jpegs = g_new0(struct jpeg *, num_jpegs);
  for (int i = 0; i < num_jpegs; i++) {
    jpegs[i] = g_slice_new0(struct jpeg);
  }

  // process jpegs
  int32_t jpeg0_tw = 0;
  int32_t jpeg0_th = 0;
  int32_t jpeg0_ta = 0;
  int32_t jpeg0_td = 0;

  for (int i = 0; i < num_jpegs; i++) {
    struct jpeg *jp = jpegs[i];

    jp->filename = g_strdup(image_filenames[i]);

    FILE *f;
    if ((f = _openslide_fopen(jp->filename, "rb", err)) == NULL) {
      g_prefix_error(err, "Can't open JPEG %d: ", i);
      goto DONE;
    }

    // comment?
    char *comment = NULL;
    char **comment_ptr = NULL;
    if (i == 0 && osr) {
      comment_ptr = &comment;
    }

    if (!verify_jpeg(f, true,
                     &jp->width, &jp->height,
                     &jp->tile_width, &jp->tile_height,
                     comment_ptr, err)) {
      g_prefix_error(err, "Can't verify JPEG %d: ", i);
      fclose(f);
      goto DONE;
    }
    jp->tiles_across = jp->width / jp->tile_width;
    jp->tiles_down = jp->height / jp->tile_height;
    jp->tile_count = jp->tiles_across * jp->tiles_down;

    if (comment) {
      g_hash_table_insert(osr->properties,
			  g_strdup(OPENSLIDE_PROPERTY_NAME_COMMENT),
			  comment);
    }

    fseeko(f, 0, SEEK_END);
    jp->end_in_file = ftello(f);
    if (jp->end_in_file == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read file size for JPEG %d", i);
      fclose(f);
      goto DONE;
    }

    // file is done now
    fclose(f);

    // because map file is last, ensure that all tile_{width,height} are the
    // same, and that all tiles_{across,down} are the same except in the last
    // column/row, for 0 through num_jpegs-2
    if (i == 0) {
      jpeg0_tw = jp->tile_width;
      jpeg0_th = jp->tile_height;
      jpeg0_ta = jp->tiles_across;
      jpeg0_td = jp->tiles_down;
    } else if (i != (num_jpegs - 1)) {
      // not map file (still within level 0)
      g_assert(jpeg0_tw != 0 && jpeg0_th != 0 &&
               jpeg0_ta != 0 && jpeg0_td != 0);
      if (jpeg0_tw != jp->tile_width || jpeg0_th != jp->tile_height) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Tile size not consistent");
        goto DONE;
      }
      if (i % num_jpeg_cols != num_jpeg_cols - 1 &&
          jp->tiles_across != jpeg0_ta) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Tiles across not consistent");
        goto DONE;
      }
      if (i / num_jpeg_cols != num_jpeg_rows - 1 &&
          jp->tiles_down != jpeg0_td) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Tiles down not consistent");
        goto DONE;
      }
    }

    jp->mcu_starts = g_new(int64_t, jp->tile_count);
    // init all to -1
    for (int32_t i = 0; i < jp->tile_count; i++) {
      (jp->mcu_starts)[i] = -1;
    }
    // use the optimisation file, if present
    int64_t *unreliable_mcu_starts = NULL;
    if (optimisation_file) {
      unreliable_mcu_starts = extract_one_optimisation(optimisation_file,
                                                       jp->tiles_down,
                                                       jp->tiles_across);
    }
    if (unreliable_mcu_starts) {
      jp->unreliable_mcu_starts = unreliable_mcu_starts;
    } else if (optimisation_file != NULL) {
      // the optimisation file is useless, ignore it
      optimisation_file = NULL;
    }
  }

  // create levels: base image + map
  level_count = 2;
  levels = g_new0(struct jpeg_level *, level_count);
  // base
  levels[0] = create_jpeg_level(osr, jpegs, num_jpeg_cols, num_jpeg_rows);
  // map
  levels[1] = create_jpeg_level(osr, &jpegs[num_jpegs - 1], 1, 1);

  /*
  for (int32_t i = 0; i < level_count; i++) {
    struct jpeg_level *l = levels[i];
    g_debug("level %d", i);
    g_debug(" size %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, l->base.w, l->base.h);
    g_debug(" tile size %d %d", l->tile_width, l->tile_height);
  }

  g_debug("num_jpegs: %d", num_jpegs);
  */

  success = true;

 DONE:
  if (success && osr) {
    // init ops
    init_jpeg_ops(osr, level_count, levels, num_jpegs, jpegs, true);
  } else {
    // destroy
    jpeg_destroy_data(num_jpegs, jpegs, level_count, levels);
  }

  return success;
}

static void ngr_destroy_levels(struct ngr_level **levels, int count) {
  for (int i = 0; i < count; i++) {
    g_free(levels[i]->filename);
    _openslide_grid_destroy(levels[i]->grid);
    g_slice_free(struct ngr_level, levels[i]);
  }
  g_free(levels);
}

static void ngr_destroy(openslide_t *osr) {
  ngr_destroy_levels((struct ngr_level **) osr->levels, osr->level_count);
}

static bool ngr_read_tile(openslide_t *osr,
                          cairo_t *cr,
                          struct _openslide_level *level,
                          int64_t tile_x, int64_t tile_y,
                          void *arg G_GNUC_UNUSED,
                          GError **err) {
  struct ngr_level *l = (struct ngr_level *) level;

  int64_t tw = l->column_width;
  int64_t th = MIN(NGR_TILE_HEIGHT, l->base.h - tile_y * NGR_TILE_HEIGHT);
  int tilesize = tw * th * 4;
  struct _openslide_cache_entry *cache_entry;
  // look up tile in cache
  uint32_t *tiledata = _openslide_cache_get(osr->cache, level, tile_x, tile_y,
                                            &cache_entry);

  if (!tiledata) {
    // read the tile data
    FILE *f = _openslide_fopen(l->filename, "rb", err);
    if (!f) {
      return false;
    }

    // compute offset to read
    int64_t offset = l->start_in_file +
      (tile_y * NGR_TILE_HEIGHT * l->column_width * 6) +
      (tile_x * l->base.h * l->column_width * 6);
    //    g_debug("tile_x: %" G_GINT64_FORMAT ", "
    //      "tile_y: %" G_GINT64_FORMAT ", "
    //      "seeking to %" G_GINT64_FORMAT, tile_x, tile_y, offset);
    fseeko(f, offset, SEEK_SET);

    // alloc and read
    int buf_size = tw * th * 6;
    uint16_t *buf = g_slice_alloc(buf_size);

    if (fread(buf, buf_size, 1, f) != 1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot read file %s", l->filename);
      fclose(f);
      g_slice_free1(buf_size, buf);
      return false;
    }
    fclose(f);

    // got the data, now convert to 8-bit xRGB
    tiledata = g_slice_alloc(tilesize);
    for (int i = 0; i < tw * th; i++) {
      // scale down from 12 bits
      uint8_t r = GINT16_FROM_LE(buf[(i * 3)]) >> 4;
      uint8_t g = GINT16_FROM_LE(buf[(i * 3) + 1]) >> 4;
      uint8_t b = GINT16_FROM_LE(buf[(i * 3) + 2]) >> 4;

      tiledata[i] = (r << 16) | (g << 8) | b;
    }
    g_slice_free1(buf_size, buf);

    // put it in the cache
    _openslide_cache_put(osr->cache, level, tile_x, tile_y,
                         tiledata,
                         tilesize,
                         &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_RGB24,
                                                                 tw, th,
                                                                 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  //_openslide_grid_label_tile(l->grid, cr, tile_x, tile_y);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

static bool ngr_paint_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
                             int64_t x, int64_t y,
                             struct _openslide_level *level,
                             int32_t w, int32_t h,
                             GError **err) {
  struct ngr_level *l = (struct ngr_level *) level;

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / level->downsample,
                                      y / level->downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops ngr_ops = {
  .paint_region = ngr_paint_region,
  .destroy = ngr_destroy,
};

static int32_t read_le_int32_from_file(FILE *f) {
  int32_t i;

  if (fread(&i, 4, 1, f) != 1) {
    return -1;
  }

  i = GINT32_FROM_LE(i);
  //  g_debug("%d", i);

  return i;
}

static bool hamamatsu_vmu_part2(openslide_t *osr,
				int num_levels, char **image_filenames,
				GError **err) {
  bool success = false;

  // initialize individual ngr structs
  struct ngr_level **levels = g_new(struct ngr_level *, num_levels);
  for (int i = 0; i < num_levels; i++) {
    levels[i] = g_slice_new0(struct ngr_level);
  }

  // open files
  for (int i = 0; i < num_levels; i++) {
    struct ngr_level *l = levels[i];

    l->filename = g_strdup(image_filenames[i]);

    FILE *f;
    if ((f = _openslide_fopen(l->filename, "rb", err)) == NULL) {
      goto DONE;
    }

    // validate magic
    if ((fgetc(f) != 'G') || (fgetc(f) != 'N')) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Bad magic on NGR file");
      fclose(f);
      goto DONE;
    }

    // read w, h, column width, headersize
    fseeko(f, 4, SEEK_SET);
    l->base.w = read_le_int32_from_file(f);
    l->base.h = read_le_int32_from_file(f);
    l->column_width = read_le_int32_from_file(f);

    fseeko(f, 24, SEEK_SET);
    l->start_in_file = read_le_int32_from_file(f);

    // validate
    if ((l->base.w <= 0) || (l->base.h <= 0) ||
	(l->column_width <= 0) || (l->start_in_file <= 0)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Error processing header");
      fclose(f);
      goto DONE;
    }

    // ensure no remainder on columns
    if ((l->base.w % l->column_width) != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Width not multiple of column width");
      fclose(f);
      goto DONE;
    }

    l->grid = _openslide_grid_create_simple(osr,
                                            l->base.w / l->column_width,
                                            (l->base.h + NGR_TILE_HEIGHT - 1)
                                            / NGR_TILE_HEIGHT,
                                            l->column_width,
                                            NGR_TILE_HEIGHT,
                                            ngr_read_tile);

    // tile size hints
    l->base.tile_w = l->column_width;
    l->base.tile_h = NGR_TILE_HEIGHT;

    fclose(f);
  }

  if (osr == NULL) {
    ngr_destroy_levels(levels, num_levels);
    return true;
  }

  // set osr data
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = num_levels;
  osr->ops = &ngr_ops;

  success = true;

 DONE:
  if (!success) {
    // destroy
    for (int i = 0; i < num_levels; i++) {
      _openslide_grid_destroy(levels[i]->grid);
      g_free(levels[i]->filename);
      g_slice_free(struct ngr_level, levels[i]);
    }
    g_free(levels);
  }

  return success;
}


bool _openslide_try_hamamatsu(openslide_t *osr, const char *filename,
			      struct _openslide_hash *quickhash1,
			      GError **err) {
  // initialize any variables destroyed/used in DONE
  bool success = false;

  char *dirname = g_path_get_dirname(filename);

  int num_images = 0;
  char **image_filenames = NULL;

  int num_cols = -1;
  int num_rows = -1;

  char **all_keys = NULL;

  int num_layers = -1;

  // first, see if it's a VMS/VMU file
  GKeyFile *key_file = g_key_file_new();
  if (!_openslide_read_key_file(key_file, filename, G_KEY_FILE_NONE, NULL)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Can't load key file");
    goto DONE;
  }

  // select group or fail, then read dimensions
  const char *groupname;
  if (g_key_file_has_group(key_file, GROUP_VMS)) {
    groupname = GROUP_VMS;

    num_cols = g_key_file_get_integer(key_file, groupname,
				      KEY_NUM_JPEG_COLS,
				      NULL);
    num_rows = g_key_file_get_integer(key_file,
				      groupname,
				      KEY_NUM_JPEG_ROWS,
				      NULL);
  } else if (g_key_file_has_group(key_file, GROUP_VMU)) {
    groupname = GROUP_VMU;

    num_cols = 1;  // not specified in file for VMU
    num_rows = 1;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not VMS or VMU file");
    goto DONE;
  }

  // validate cols/rows
  if (num_cols < 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "File has no columns");
    goto DONE;
  }
  if (num_rows < 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "File has no rows");
    goto DONE;
  }

  // init the image filenames
  // this format has cols*rows image files, plus the map
  num_images = (num_cols * num_rows) + 1;
  image_filenames = g_new0(char *, num_images);

  // hash in the key file
  if (!_openslide_hash_file(quickhash1, filename, err)) {
    goto DONE;
  }

  // make sure values are within known bounds
  num_layers = g_key_file_get_integer(key_file, groupname, KEY_NUM_LAYERS,
				      NULL);
  if (num_layers < 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot handle Hamamatsu files with NoLayers < 1");
    goto DONE;
  }

  // add properties
  if (osr) {
    add_properties(osr->properties, key_file, groupname);
  }

  // extract MapFile
  char *tmp;
  tmp = g_key_file_get_string(key_file,
			      groupname,
			      KEY_MAP_FILE,
			      NULL);
  if (tmp && *tmp) {
    char *map_filename = g_build_filename(dirname, tmp, NULL);
    g_free(tmp);

    image_filenames[num_images - 1] = map_filename;

    // hash in the map file
    if (!_openslide_hash_file(quickhash1, map_filename, err)) {
      goto DONE;
    }
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read map file");
    g_free(tmp);
    goto DONE;
  }

  // now each ImageFile
  all_keys = g_key_file_get_keys(key_file, groupname, NULL, NULL);
  for (char **tmp = all_keys; *tmp != NULL; tmp++) {
    char *key = *tmp;
    char *value = g_key_file_get_string(key_file, groupname, key, NULL);

    //    g_debug("%s", key);

    if (strncmp(KEY_IMAGE_FILE, key, strlen(KEY_IMAGE_FILE)) == 0) {
      // starts with ImageFile
      char *suffix = key + strlen(KEY_IMAGE_FILE);

      int layer;
      int col;
      int row;

      char **split = g_strsplit(suffix, ",", 0);
      switch (g_strv_length(split)) {
      case 0:
	// all zero
	layer = 0;
	col = 0;
	row = 0;
	break;

      case 1:
	// (z)
	// first item, skip '('
	layer = g_ascii_strtoll(split[0] + 1, NULL, 10);
	col = 0;
	row = 0;
	break;

      case 2:
	// (x,y)
	layer = 0;
	// first item, skip '('
	col = g_ascii_strtoll(split[0] + 1, NULL, 10);
	row = g_ascii_strtoll(split[1], NULL, 10);
	break;

      case 3:
        // (z,x,y)
        // first item, skip '('
        layer = g_ascii_strtoll(split[0] + 1, NULL, 10);
        col = g_ascii_strtoll(split[1], NULL, 10);
        row = g_ascii_strtoll(split[2], NULL, 10);
        break;

      default:
        // we just don't know
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unknown number of image dimensions: %d",
                    g_strv_length(split));
        g_free(value);
        g_strfreev(split);
        g_strfreev(all_keys);
        goto DONE;
      }
      g_strfreev(split);

      //g_debug("layer: %d, col: %d, row: %d", layer, col, row);

      if (layer != 0) {
        // skip non-zero layers for now
        g_free(value);
        continue;
      }

      if (col >= num_cols || row >= num_rows || col < 0 || row < 0) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Invalid row or column in Hamamatsu file (%d,%d)",
                    col, row);
        g_free(value);
	g_strfreev(all_keys);
        goto DONE;
      }

      // compute index from x,y
      int i = row * num_cols + col;

      // init the file
      if (image_filenames[i]) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Duplicate image for (%d,%d)", col, row);
        g_free(value);
        g_strfreev(all_keys);
        goto DONE;
      }
      image_filenames[i] = g_build_filename(dirname, value, NULL);
    }
    g_free(value);
  }
  g_strfreev(all_keys);

  // ensure all image filenames are filled
  for (int i = 0; i < num_images; i++) {
    if (!image_filenames[i]) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read image filename %d", i);
      goto DONE;
    }
  }

  // add macro image
  tmp = g_key_file_get_string(key_file,
			      groupname,
			      KEY_MACRO_IMAGE,
			      NULL);
  if (tmp && *tmp) {
    char *macro_filename = g_build_filename(dirname, tmp, NULL);
    bool result = _openslide_jpeg_add_associated_image(osr,
                                                       "macro",
                                                       macro_filename, 0, err);
    g_free(macro_filename);

    if (!result) {
      g_prefix_error(err, "Could not read macro image: ");
      g_free(tmp);
      goto DONE;
    }
  }
  g_free(tmp);

  // finalize depending on what format
  if (groupname == GROUP_VMS) {
    // open OptimisationFile
    FILE *optimisation_file = NULL;
    char *tmp = g_key_file_get_string(key_file,
				      GROUP_VMS,
				      KEY_OPTIMISATION_FILE,
				      NULL);
    if (tmp) {
      char *optimisation_filename = g_build_filename(dirname, tmp, NULL);
      g_free(tmp);

      optimisation_file = _openslide_fopen(optimisation_filename, "rb", NULL);

      if (optimisation_file == NULL) {
	// g_debug("Can't open optimisation file");
      }
      g_free(optimisation_filename);
    } else {
      // g_debug("Optimisation file key not present");
    }

    // do all the jpeg stuff
    success = hamamatsu_vms_part2(osr,
				  num_images, image_filenames,
				  num_cols, num_rows,
				  optimisation_file,
				  err);

    // clean up
    if (optimisation_file) {
      fclose(optimisation_file);
    }
  } else if (groupname == GROUP_VMU) {
    // verify a few assumptions for VMU
    int bits_per_pixel = g_key_file_get_integer(key_file,
						GROUP_VMU,
						KEY_BITS_PER_PIXEL,
						NULL);
    char *pixel_order = g_key_file_get_string(key_file,
					      GROUP_VMU,
					      KEY_PIXEL_ORDER,
					      NULL);

    if (bits_per_pixel != 36) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "%s must be 36", KEY_BITS_PER_PIXEL);
    } else if (!pixel_order || (strcmp(pixel_order, "RGB") != 0)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "%s must be RGB", KEY_PIXEL_ORDER);
    } else {
      // assumptions verified
      success = hamamatsu_vmu_part2(osr,
				    num_images, image_filenames,
				    err);
    }
    g_free(pixel_order);
  } else {
    g_assert_not_reached();
  }

 DONE:
  g_free(dirname);

  if (image_filenames) {
    for (int i = 0; i < num_images; i++) {
      g_free(image_filenames[i]);
    }
    g_free(image_filenames);
  }
  g_key_file_free(key_file);

  return success;
}

static void ndpi_set_sint_prop(GHashTable *ht,
                               struct _openslide_tifflike *tl,
                               int64_t dir, int32_t tag,
                               const char *property_name) {
  bool ok = true;
  int64_t value = _openslide_tifflike_get_sint(tl, dir, tag, 0, &ok);
  if (ok) {
    g_hash_table_insert(ht, g_strdup(property_name),
                        g_strdup_printf("%"G_GINT64_FORMAT, value));
  }
}

static void ndpi_set_float_prop(GHashTable *ht,
                                struct _openslide_tifflike *tl,
                                int64_t dir, int32_t tag,
                                const char *property_name) {
  bool ok = true;
  double value = _openslide_tifflike_get_float(tl, dir, tag, 0, &ok);
  if (ok) {
    g_hash_table_insert(ht, g_strdup(property_name),
                        _openslide_format_double(value));
  }
}

static void ndpi_set_resolution_prop(GHashTable *ht,
                                     struct _openslide_tifflike *tl,
                                     int64_t dir, int32_t tag,
                                     const char *property_name) {
  bool ok = true;
  uint64_t unit = _openslide_tifflike_get_uint(tl, dir,
                                               TIFFTAG_RESOLUTIONUNIT, 0, &ok);
  if (!ok) {
    unit = RESUNIT_INCH;  // default
    ok = true;
  }
  double res = _openslide_tifflike_get_float(tl, dir, tag, 0, &ok);

  if (ok && unit == RESUNIT_CENTIMETER) {
    g_hash_table_insert(ht, g_strdup(property_name),
                        _openslide_format_double(10000.0 / res));
  }
}

static void ndpi_set_string_prop(GHashTable *ht,
                                 struct _openslide_tifflike *tl,
                                 int64_t dir, int32_t tag,
                                 const char *property_name) {
  const char *value = _openslide_tifflike_get_buffer(tl, dir, tag);
  if (value) {
    g_hash_table_insert(ht, g_strdup(property_name), g_strdup(value));
  }
}

static void ndpi_set_props(openslide_t *osr,
                           struct _openslide_tifflike *tl, int64_t dir) {
  if (!osr) {
    return;
  }
  GHashTable *ht = osr->properties;

  // vendor
  g_hash_table_insert(ht, g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
                      g_strdup("hamamatsu"));

  // MPP
  ndpi_set_resolution_prop(ht, tl, dir, TIFFTAG_XRESOLUTION,
                           OPENSLIDE_PROPERTY_NAME_MPP_X);
  ndpi_set_resolution_prop(ht, tl, dir, TIFFTAG_YRESOLUTION,
                           OPENSLIDE_PROPERTY_NAME_MPP_Y);

  // objective power
  ndpi_set_float_prop(ht, tl, dir, NDPI_SOURCELENS,
                      "hamamatsu.SourceLens");
  ndpi_set_float_prop(ht, tl, dir, NDPI_SOURCELENS,
                      OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);

  // misc properties
  ndpi_set_sint_prop(ht, tl, dir, NDPI_XOFFSET,
                     "hamamatsu.XOffsetFromSlideCentre");
  ndpi_set_sint_prop(ht, tl, dir, NDPI_YOFFSET,
                     "hamamatsu.YOffsetFromSlideCentre");
  ndpi_set_string_prop(ht, tl, dir, NDPI_REFERENCE,
                       "hamamatsu.Reference");

  // ASCII property map
  const char *props = _openslide_tifflike_get_buffer(tl, dir,
                                                     NDPI_PROPERTY_MAP);
  if (props) {
    char **records = g_strsplit(props, "\r\n", 0);
    for (char **cur_record = records; *cur_record; cur_record++) {
      char **pair = g_strsplit(*cur_record, "=", 2);
      if (pair[0] && pair[0][0] && pair[1] && pair[1][0]) {
        g_hash_table_insert(ht,
                            g_strdup_printf("hamamatsu.%s", pair[0]),
                            g_strdup(pair[1]));
      }
      g_strfreev(pair);
    }
    g_strfreev(records);
  }
}

bool _openslide_try_hamamatsu_ndpi(openslide_t *osr, const char *filename,
				   struct _openslide_hash *quickhash1,
				   GError **err) {
  FILE *f = NULL;
  struct _openslide_tifflike *tl = NULL;
  GPtrArray *jpeg_array = g_ptr_array_new();
  GPtrArray *level_array = g_ptr_array_new();
  GError *tmp_err = NULL;
  bool success = false;
  bool restart_marker_scan = false;

  // open file
  f = _openslide_fopen(filename, "rb", &tmp_err);
  if (!f) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "%s", tmp_err->message);
    g_clear_error(&tmp_err);
    goto DONE;
  }

  // parse TIFF
  tl = _openslide_tifflike_create(f, err);
  if (!tl) {
    goto DONE;
  }

  // check for NDPI
  const char *software = _openslide_tifflike_get_buffer(tl, 0,
                                                        TIFFTAG_SOFTWARE);
  if (software == NULL ||
      strncmp(software, NDPI_SOFTWARE, strlen(NDPI_SOFTWARE))) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Unexpected or missing Software tag");
    goto DONE;
  }

  // walk directories
  int64_t directories = _openslide_tifflike_get_directory_count(tl);
  int64_t min_width = INT64_MAX;
  int64_t min_width_dir = 0;
  for (int64_t dir = 0; dir < directories; dir++) {
    // read tags
    bool ok = true;
    int64_t width =
      _openslide_tifflike_get_uint(tl, dir, TIFFTAG_IMAGEWIDTH, 0, &ok);
    int64_t height =
      _openslide_tifflike_get_uint(tl, dir, TIFFTAG_IMAGELENGTH, 0, &ok);
    int64_t rows_per_strip =
      _openslide_tifflike_get_uint(tl, dir, TIFFTAG_ROWSPERSTRIP, 0, &ok);
    int64_t start_in_file =
      _openslide_tifflike_get_uint(tl, dir, TIFFTAG_STRIPOFFSETS, 0, &ok);
    int64_t num_bytes =
      _openslide_tifflike_get_uint(tl, dir, TIFFTAG_STRIPBYTECOUNTS, 0, &ok);
    double lens =
      _openslide_tifflike_get_float(tl, dir, NDPI_SOURCELENS, 0, &ok);

    // check results
    if (!ok) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Missing TIFF tag in directory %"G_GINT64_FORMAT, dir);
      goto DONE;
    }
    if (height != rows_per_strip) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Unexpected rows per strip %"G_GINT64_FORMAT" "
                  "(height %"G_GINT64_FORMAT")", rows_per_strip, height);
      goto DONE;
    }

    if (lens > 0) {
      // is a pyramid level

      // is smallest level?
      if (width < min_width) {
        min_width = width;
        min_width_dir = dir;
      } else {
        // Slide may have multiple focal planes.  We should ignore
        // planes != 0, but we don't know which TIFF tag specifies the
        // plane.  This slide's levels seem to be in a strange order, and
        // we don't want to accidentally merge levels from different planes,
        // so reject the slide for safety.
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unexpected slide layout; contact "
                    "<openslide-users@lists.andrew.cmu.edu> for assistance");
        goto DONE;
      }

      // will the JPEG image dimensions be valid?
      bool dimensions_valid = (width <= JPEG_MAX_DIMENSION &&
                               height <= JPEG_MAX_DIMENSION);

      // verify JPEG
      int32_t jp_w = width;  // overwritten if dimensions_valid
      int32_t jp_h = height; // overwritten if dimensions_valid
      int32_t jp_tw, jp_th;
      if (fseeko(f, start_in_file, SEEK_SET)) {
        _openslide_io_error(err, "Couldn't fseek %s", filename);
        goto DONE;
      }
      if (!verify_jpeg(f, dimensions_valid,
                       &jp_w, &jp_h,
                       &jp_tw, &jp_th,
                       NULL, &tmp_err)) {
        if (g_error_matches(tmp_err, OPENSLIDE_HAMAMATSU_ERROR,
                            OPENSLIDE_HAMAMATSU_ERROR_NO_RESTART_MARKERS)) {
          // non-tiled image
          //g_debug("non-tiled image %"G_GINT64_FORMAT, dir);
          g_clear_error(&tmp_err);
          jp_w = jp_tw = width;
          jp_h = jp_th = height;
        } else {
          g_propagate_prefixed_error(err, tmp_err,
                                     "Can't verify JPEG for directory "
                                     "%"G_GINT64_FORMAT": ", dir);
          goto DONE;
        }
      }
      if (width != jp_w || height != jp_h) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "JPEG dimension mismatch for directory "
                    "%"G_GINT64_FORMAT": "
                    "expected %"G_GINT64_FORMAT"x%"G_GINT64_FORMAT", "
                    "found %dx%d",
                    dir, width, height, jp_w, jp_h);
        goto DONE;
      }

      // init jpeg
      struct jpeg *jp = g_slice_new0(struct jpeg);
      jp->filename = g_strdup(filename);
      jp->start_in_file = start_in_file;
      jp->end_in_file = start_in_file + num_bytes;
      jp->width = width;
      jp->height = height;
      jp->tiles_across = width / jp_tw;
      jp->tiles_down = height / jp_th;
      jp->tile_width = jp_tw;
      jp->tile_height = jp_th;
      jp->tile_count = jp->tiles_across * jp->tiles_down;
      jp->mcu_starts = g_new(int64_t, jp->tile_count);
      // init all to -1
      for (int32_t i = 0; i < jp->tile_count; i++) {
        jp->mcu_starts[i] = -1;
      }

      // read MCU starts, if this directory is tiled
      if (jp->tile_count > 1) {
        int64_t mcu_start_count =
          _openslide_tifflike_get_value_count(tl, dir, NDPI_MCU_STARTS);

        if (mcu_start_count == jp->tile_count) {
          //g_debug("loading MCU starts for directory %"G_GINT64_FORMAT, dir);
          jp->unreliable_mcu_starts = g_new(int64_t, mcu_start_count);
          ok = true;
          for (int64_t tile = 0; tile < mcu_start_count; tile++) {
            jp->unreliable_mcu_starts[tile] =
              _openslide_tifflike_get_uint(tl, dir, NDPI_MCU_STARTS, tile,
                                           &ok) + jp->start_in_file;
            //g_debug("mcu start at %"G_GINT64_FORMAT, jp->unreliable_mcu_starts[tile]);
          }
          if (!ok) {
            //g_debug("failed to load MCU starts for directory %"G_GINT64_FORMAT, dir);
            g_free(jp->unreliable_mcu_starts);
            jp->unreliable_mcu_starts = NULL;
          }
        }

        if (jp->unreliable_mcu_starts == NULL) {
          // no marker positions; scan for them in the background
          //g_debug("enabling restart marker thread for directory %"G_GINT64_FORMAT, dir);
          restart_marker_scan = true;
        }
      }

      g_ptr_array_add(jpeg_array, jp);

      // create level
      struct jpeg_level *l = create_jpeg_level(osr, &jp, 1, 1);
      g_ptr_array_add(level_array, l);

    } else if (lens == -1) {
      // macro image
      if (!_openslide_jpeg_add_associated_image(osr, "macro",
                                                filename, start_in_file,
                                                err)) {
        goto DONE;
      }
    }
  }

  // init properties and set hash
  if (!_openslide_tifflike_init_properties_and_hash(osr, filename, tl,
                                                    quickhash1,
                                                    min_width_dir, 0,
                                                    err)) {
    goto DONE;
  }
  ndpi_set_props(osr, tl, 0);

  success = true;

DONE:
  // free
  _openslide_tifflike_destroy(tl);
  if (f) {
    fclose(f);
  }

  // unwrap jpegs
  int32_t num_jpegs = jpeg_array->len;
  struct jpeg **jpegs = (struct jpeg **) g_ptr_array_free(jpeg_array, false);

  // unwrap levels
  int32_t level_count = level_array->len;
  struct jpeg_level **levels =
    (struct jpeg_level **) g_ptr_array_free(level_array, false);

  if (success && osr) {
    // init ops
    init_jpeg_ops(osr, level_count, levels, num_jpegs, jpegs,
                  restart_marker_scan);
  } else {
    // destroy
    jpeg_destroy_data(num_jpegs, jpegs, level_count, levels);
  }

  return success;
}
