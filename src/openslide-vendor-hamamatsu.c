/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
 *  Copyright (c) 2011, 2016 Google, Inc.
 *  Copyright (c) 2016-2022 Benjamin Gilbert
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
static const char KEY_NUM_JPEG_COLS[] = "NoJpegColumns";
static const char KEY_NUM_JPEG_ROWS[] = "NoJpegRows";
static const char KEY_OPTIMISATION_FILE[] = "OptimisationFile";
static const char KEY_MACRO_IMAGE[] = "MacroImage";
static const char KEY_PHYSICAL_WIDTH[] = "PhysicalWidth";
static const char KEY_PHYSICAL_HEIGHT[] = "PhysicalHeight";
static const char KEY_BITS_PER_PIXEL[] = "BitsPerPixel";
static const char KEY_PIXEL_ORDER[] = "PixelOrder";
// probing any file under this limit will load the entire file into RAM,
// so set the limit conservatively
static const int KEY_FILE_MAX_SIZE = 64 << 10;

// NDPI
#define NDPI_FORMAT_FLAG 65420
#define NDPI_SOURCELENS 65421
#define NDPI_XOFFSET 65422
#define NDPI_YOFFSET 65423
#define NDPI_FOCAL_PLANE 65424
#define NDPI_MCU_STARTS 65426
#define NDPI_REFERENCE 65427
#define NDPI_PROPERTY_MAP 65449
#define JPEG_MAX_DIMENSION_HIGH ((JPEG_MAX_DIMENSION >> 8) & 0xff)
#define JPEG_MAX_DIMENSION_LOW (JPEG_MAX_DIMENSION & 0xff)

#define TIFF_GET_UINT_OR_RETURN(TL, DIR, TAG, OUT, RET) do {		\
    GError *tmp_err = NULL;						\
    OUT = _openslide_tifflike_get_uint(TL, DIR, TAG, &tmp_err);		\
    if (tmp_err) {							\
      g_propagate_error(err, tmp_err);					\
      return RET;							\
    }									\
  } while (0)

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
  int64_t header_stop_position;
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

// temporary data structure during open
struct jpeg_setup {
  GPtrArray *levels;
  GPtrArray *jpegs;
};

struct hamamatsu_jpeg_ops_data {
  int32_t jpeg_count;
  struct jpeg **all_jpegs;

  // thread stuff, for background search of restart markers
  int64_t restart_marker_last_used_time;
  GMutex restart_marker_mutex;
  GThread *restart_marker_thread;

  GCond restart_marker_cond;
  GMutex restart_marker_cond_mutex;
  uint32_t restart_marker_users;
  bool restart_marker_thread_throttle;
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
 * Source manager for reading a run of MCUs between two restart markers
 * as a complete JPEG.  Originally based on jdatasrc.c from IJG libjpeg.
 */
static bool jpeg_random_access_src(j_decompress_ptr cinfo,
                                   struct _openslide_file *infile,
                                   int64_t header_start_position,
                                   int64_t sof_position,
                                   int64_t header_stop_position,
                                   int64_t start_position,
                                   int64_t stop_position,
                                   GError **err) {
  // check for problems
  if ((0 > header_start_position) ||
      (header_start_position >= sof_position) ||
      (sof_position + 9 >= header_stop_position) ||
      (start_position != -1 &&
       ((header_stop_position > start_position) ||
        (start_position >= stop_position)))) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
               "Can't do random access JPEG read: "
	       "header_start_position: %"PRId64", "
	       "sof_position: %"PRId64", "
	       "header_stop_position: %"PRId64", "
	       "start_position: %"PRId64", "
	       "stop_position: %"PRId64,
	       header_start_position, sof_position, header_stop_position,
	       start_position, stop_position);
    return false;
  }

  // compute size of buffer and allocate
  int header_length = header_stop_position - header_start_position;
  int data_length = 0;
  if (start_position != -1) {
    data_length = stop_position - start_position;
  }

  int buffer_size = header_length + data_length;
  // automatically freed when decompression is terminated
  JOCTET *buffer = (*cinfo->mem->alloc_large)((j_common_ptr) cinfo,
                                              JPOOL_IMAGE, buffer_size);

  // read in the 2 parts
  //  g_debug("reading header from %"PRId64, header_start_position);
  if (!_openslide_fseek(infile, header_start_position, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to header start: ");
    return false;
  }
  if (_openslide_fread(infile, buffer, header_length) !=
      (size_t) header_length) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read header in JPEG at %"PRId64,
                header_start_position);
    return false;
  }

  if (data_length) {
    //  g_debug("reading from %"PRId64, start_position);
    if (!_openslide_fseek(infile, start_position, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek to data start: ");
      return false;
    }
    if (_openslide_fread(infile, buffer + header_length, data_length) !=
        (size_t) data_length) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot read data in JPEG at %"PRId64, start_position);
      return false;
    }

    // change the final byte to EOI
    if (buffer[buffer_size - 2] != 0xFF) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Expected 0xFF byte at end of JPEG data");
      return false;
    }
    buffer[buffer_size - 1] = JPEG_EOI;
  }

  // check for overlarge or 0 X/Y in SOF (some NDPI JPEGs have this)
  // change them to a value libjpeg will accept
  int64_t size_offset = sof_position - header_start_position + 5;
  uint16_t y = (buffer[size_offset + 0] << 8) +
                buffer[size_offset + 1];
  if (y > JPEG_MAX_DIMENSION || y == 0) {
    //g_debug("fixing up SOF Y");
    buffer[size_offset + 0] = JPEG_MAX_DIMENSION_HIGH;
    buffer[size_offset + 1] = JPEG_MAX_DIMENSION_LOW;
  }
  uint16_t x = (buffer[size_offset + 2] << 8) +
                buffer[size_offset + 3];
  if (x > JPEG_MAX_DIMENSION || x == 0) {
    //g_debug("fixing up SOF X");
    buffer[size_offset + 2] = JPEG_MAX_DIMENSION_HIGH;
    buffer[size_offset + 3] = JPEG_MAX_DIMENSION_LOW;
  }

  // pass the buffer off to mem_src
  _openslide_jpeg_mem_src(cinfo, buffer, buffer_size);

  return true;
}

static void jpeg_level_free(struct jpeg_level *l) {
  //g_debug("level_free: %p", data);
  if (l == NULL) {
    return;
  }
  g_free(l->jpegs);
  _openslide_grid_destroy(l->grid);
  g_slice_free(struct jpeg_level, l);
}

static void jpeg_free(struct jpeg *jpeg) {
  g_free(jpeg->filename);
  g_free(jpeg->mcu_starts);
  g_free(jpeg->unreliable_mcu_starts);
  g_slice_free(struct jpeg, jpeg);
}

static struct jpeg_setup *jpeg_setup_new(void) {
  struct jpeg_setup *setup = g_slice_new0(struct jpeg_setup);
  setup->levels =
    g_ptr_array_new_with_free_func((GDestroyNotify) jpeg_level_free);
  setup->jpegs = g_ptr_array_new_with_free_func((GDestroyNotify) jpeg_free);
  return setup;
}

static void jpeg_setup_free(struct jpeg_setup *setup) {
  if (setup->levels) {
    g_ptr_array_free(setup->levels, true);
  }
  if (setup->jpegs) {
    g_ptr_array_free(setup->jpegs, true);
  }
  g_slice_free(struct jpeg_setup, setup);
}

typedef struct jpeg_setup jpeg_setup;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(jpeg_setup, jpeg_setup_free)

static bool find_bitstream_start(struct _openslide_file *f,
                                 int64_t *sof_position,
                                 int64_t *header_stop_position,
                                 GError **err) {
  uint8_t buf[2];
  uint8_t marker_byte;
  uint16_t len;
  int64_t pos;
  bool have_sof = false;

  while (true) {
    // read marker
    pos = _openslide_ftell(f, err);
    if (pos == -1) {
      g_prefix_error(err, "Couldn't seek to JPEG marker: ");
      return false;
    }
    if (_openslide_fread(f, buf, sizeof(buf)) != sizeof(buf)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG marker at %"PRId64, pos);
      return false;
    }
    if (buf[0] != 0xFF) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Expected marker at %"PRId64", found none", pos);
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
      have_sof = true;
    }

    // read length
    if (_openslide_fread(f, buf, sizeof(buf)) != sizeof(buf)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG marker length at %"PRId64, pos);
      return false;
    }
    memcpy(&len, buf, sizeof(len));
    len = GUINT16_FROM_BE(len);

    // seek
    if (!_openslide_fseek(f, pos + sizeof(buf) + len, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek to next marker: ");
      return false;
    }

    // check for SOS
    if (marker_byte == 0xDA) {
      // found it; done
      *header_stop_position = _openslide_ftell(f, err);
      if (*header_stop_position == -1) {
        g_prefix_error(err, "Couldn't get header stop position: ");
        return false;
      }
      //g_debug("found bitstream start at %"PRId64, *header_stop_position);
      break;
    }
  }

  if (!have_sof) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Reached SOS marker without finding SOF");
    return false;
  }

  return true;
}

static bool find_next_ff_marker(struct _openslide_file *f,
                                uint8_t *buf_start,
                                uint8_t **buf,
                                int buf_size,
                                int64_t file_size,
                                uint8_t *marker_byte,
                                int64_t *after_marker_pos,
                                int *bytes_in_buf,
                                GError **err) {
  //g_debug("bytes_in_buf: %d", *bytes_in_buf);
  int64_t file_pos = _openslide_ftell(f, err);
  if (file_pos == -1) {
    g_prefix_error(err, "Couldn't get file position: ");
    return false;
  }
  bool last_was_ff = false;
  while (true) {
    if (*bytes_in_buf == 0) {
      // fill buffer
      *buf = buf_start;
      int bytes_to_read = MIN(buf_size, file_size - file_pos);

      //g_debug("bytes_to_read: %d", bytes_to_read);
      if (bytes_to_read == 0 ||
          _openslide_fread(f, *buf, bytes_to_read) != (size_t) bytes_to_read) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Short read searching for JPEG marker at %"PRId64,
                    file_pos);
        return false;
      }

      file_pos += bytes_to_read;
      *bytes_in_buf = bytes_to_read;
    }

    // special case where the last time ended with FF
    if (last_was_ff) {
      //g_debug("last_was_ff");
      *marker_byte = (*buf)[0];
      (*buf)++;
      (*bytes_in_buf)--;
      *after_marker_pos = file_pos - *bytes_in_buf;
      return true;
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
	*marker_byte = ff[1];
	*after_marker_pos = file_pos - *bytes_in_buf;
	return true;
      }
    }
  }
}

static bool _compute_mcu_start(struct jpeg *jpeg,
			       struct _openslide_file *f,
			       int64_t target,
			       GError **err) {
  // special case for first
  if (jpeg->mcu_starts[0] == -1) {
    jpeg->mcu_starts[0] = jpeg->header_stop_position;
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
      if (!_openslide_fseek(f, offset - 2, SEEK_SET, err)) {
        g_prefix_error(err, "Couldn't seek to recorded restart marker at "
                       "%"PRId64": ", offset - 2);
        return false;
      }

      if (_openslide_fread(f, buf, 2) != 2 ||
          buf[0] != 0xFF || buf[1] < 0xD0 || buf[1] > 0xD7) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Restart marker not found at recorded position %"PRId64,
                    offset - 2);
        return false;
      }

      //  g_debug("accepted unreliable marker %"PRId64, first_good);
      jpeg->mcu_starts[first_good] = offset;
      break;
    }
  }

  if (first_good == target) {
    // we're done
    return true;
  }
  //  g_debug("target: %"PRId64", first_good: %"PRId64, target, first_good);

  // now search for the new restart markers
  if (!_openslide_fseek(f, jpeg->mcu_starts[first_good], SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't seek to first good restart marker: ");
    return false;
  }

  uint8_t buf_start[4096];
  uint8_t *buf = buf_start;
  int bytes_in_buf = 0;
  while (first_good < target) {
    uint8_t marker_byte;
    int64_t after_marker_pos;
    if (!find_next_ff_marker(f, buf_start, &buf, sizeof(buf_start),
                             jpeg->end_in_file,
                             &marker_byte,
                             &after_marker_pos,
                             &bytes_in_buf,
                             err)) {
      return false;
    }
    g_assert(after_marker_pos > 0);
    //g_debug("after_marker_pos: %"PRId64, after_marker_pos);

    if (marker_byte >= 0xD0 && marker_byte < 0xD8) {
      // restart marker
      jpeg->mcu_starts[1 + first_good++] = after_marker_pos;
    }
  }
  return true;
}

static bool compute_mcu_start(openslide_t *osr,
			      struct jpeg *jpeg,
			      struct _openslide_file *f,
			      int64_t tileno,
			      int64_t *start_position,
			      int64_t *stop_position,
			      GError **err) {
  struct hamamatsu_jpeg_ops_data *data = osr->data;

  if (tileno < 0 || tileno >= jpeg->tile_count) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid tileno %"PRId64, tileno);
    return false;
  }

  g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
    g_mutex_locker_new(&data->restart_marker_mutex);

  if (!_compute_mcu_start(jpeg, f, tileno, err)) {
    return false;
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
        return false;
      }
      *stop_position = jpeg->mcu_starts[tileno + 1];
    }
    g_assert(*stop_position != -1);
  }

  return true;
}

static bool read_from_jpeg(openslide_t *osr,
                           struct jpeg *jpeg,
                           int32_t tileno,
                           int32_t scale_denom,
                           uint32_t *dest,
                           int32_t w, int32_t h,
                           GError **err) {
  // open file
  g_autoptr(_openslide_file) f = _openslide_fopen(jpeg->filename, err);
  if (f == NULL) {
    return false;
  }

  // begin decompress
  struct jpeg_decompress_struct *cinfo;
  g_auto(_openslide_jpeg_decompress) dc =
    _openslide_jpeg_decompress_create(&cinfo);
  jmp_buf env;

  if (setjmp(env) == 0) {
    // figure out where to start the data stream
    int64_t start_position;
    int64_t stop_position;
    if (!compute_mcu_start(osr, jpeg, f, tileno,
                           &start_position, &stop_position,
                           err)) {
      return false;
    }

    // start decompressing
    _openslide_jpeg_decompress_init(dc, &env);

    if (!jpeg_random_access_src(cinfo, f,
                                jpeg->start_in_file,
                                jpeg->sof_position,
                                jpeg->header_stop_position,
                                start_position,
                                stop_position,
                                err)) {
      return false;
    }

    if (jpeg_read_header(cinfo, true) != JPEG_HEADER_OK) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG header");
      return false;
    }
    cinfo->scale_num = 1;
    cinfo->scale_denom = scale_denom;
    cinfo->image_width = jpeg->tile_width;  // cunning
    cinfo->image_height = jpeg->tile_height;

    //    g_debug("output_width: %d", cinfo->output_width);
    //    g_debug("output_height: %d", cinfo->output_height);

    return _openslide_jpeg_decompress_run(dc, dest, false, w, h, err);
  } else {
    // setjmp returns again
    _openslide_jpeg_propagate_error(err, dc);
    return false;
  }
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
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);

  if (!tiledata) {
    g_auto(_openslide_slice) box = _openslide_slice_alloc(tw * th * 4);
    if (!read_from_jpeg(osr,
                        jp, tileno,
                        l->scale_denom,
                        box.p, tw, th,
                        err)) {
      return false;
    }

    tiledata = _openslide_slice_steal(&box);
    _openslide_cache_put(osr->cache,
			 level, tile_col, tile_row,
			 tiledata,
			 tw * th * 4,
			 &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_RGB24,
                                        tw, th, tw * 4);

  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return true;
}


static bool jpeg_paint_region(openslide_t *osr, cairo_t *cr,
                              int64_t x, int64_t y,
                              struct _openslide_level *level,
                              int32_t w, int32_t h,
                              GError **err) {
  struct hamamatsu_jpeg_ops_data *data = osr->data;
  struct jpeg_level *l = (struct jpeg_level *) level;

  {
    g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
      g_mutex_locker_new(&data->restart_marker_cond_mutex);
    // check for background errors
    if (data->restart_marker_thread_error) {
      // propagate error
      g_propagate_error(err, g_steal_pointer(&data->restart_marker_thread_error));
      return false;
    }
    // tell the background thread to pause
    data->restart_marker_users++;
    //  g_debug("telling thread to pause");
  }

  // paint
  bool success = _openslide_grid_paint_region(l->grid, cr, NULL,
                                              x / level->downsample,
                                              y / level->downsample,
                                              level, w, h,
                                              err);

  // maybe tell the background thread to resume
  {
    g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
      g_mutex_locker_new(&data->restart_marker_cond_mutex);
    if (!--data->restart_marker_users) {
      data->restart_marker_last_used_time = g_get_monotonic_time();
      //  g_debug("telling thread to awaken");
      g_cond_signal(&data->restart_marker_cond);
    }
  }

  return success;
}

static void jpeg_do_destroy(openslide_t *osr) {
  struct hamamatsu_jpeg_ops_data *data = osr->data;

  // tell the thread to finish and wait
  {
    g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
      g_mutex_locker_new(&data->restart_marker_cond_mutex);
    g_warn_if_fail(data->restart_marker_users == 0);
    data->restart_marker_thread_stop = true;
    g_cond_signal(&data->restart_marker_cond);
  }
  if (data->restart_marker_thread) {
    g_thread_join(data->restart_marker_thread);
  }

  // jpegs
  for (int32_t i = 0; i < data->jpeg_count; i++) {
    jpeg_free(data->all_jpegs[i]);
  }
  g_free(data->all_jpegs);

  // levels
  for (int32_t i = 0; i < osr->level_count; i++) {
    jpeg_level_free((struct jpeg_level *) osr->levels[i]);
  }
  g_free(osr->levels);

  // the background stuff
  {
    g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
      g_mutex_locker_new(&data->restart_marker_cond_mutex);
    if (data->restart_marker_thread_error) {
      g_error_free(data->restart_marker_thread_error);
    }
  }
  g_mutex_clear(&data->restart_marker_mutex);
  g_cond_clear(&data->restart_marker_cond);
  g_mutex_clear(&data->restart_marker_cond_mutex);

  // the structure
  g_slice_free(struct hamamatsu_jpeg_ops_data, data);
}

static const struct _openslide_ops hamamatsu_jpeg_ops = {
  .paint_region = jpeg_paint_region,
  .destroy = jpeg_do_destroy,
};

static bool hamamatsu_vms_vmu_detect(const char *filename,
                                     struct _openslide_tifflike *tl,
                                     GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // try to parse key file
  g_autoptr(GKeyFile) key_file =
    _openslide_read_key_file(filename, KEY_FILE_MAX_SIZE, G_KEY_FILE_NONE, err);
  if (!key_file) {
    g_prefix_error(err, "Can't read key file: ");
    return false;
  }

  // check format
  if (g_key_file_has_group(key_file, GROUP_VMS)) {
    // validate cols/rows
    if (g_key_file_get_integer(key_file, GROUP_VMS,
                               KEY_NUM_JPEG_COLS, NULL) < 1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "VMS file has no columns");
      return false;
    }
    if (g_key_file_get_integer(key_file, GROUP_VMS,
                               KEY_NUM_JPEG_ROWS, NULL) < 1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "VMS file has no rows");
      return false;
    }

  } else if (!g_key_file_has_group(key_file, GROUP_VMU)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not VMS or VMU file");
    return false;
  }

  return true;
}

static gint width_compare(gconstpointer a, gconstpointer b) {
  int64_t w1 = *((const int64_t *) a);
  int64_t w2 = *((const int64_t *) b);

  g_assert(w1 >= 0 && w2 >= 0);

  return (w1 < w2) - (w1 > w2);
}

#define CHK(ASSERTION)							\
  do {									\
    if (!(ASSERTION)) {							\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,		\
                  "Invalid MCU starts: JPEG %d, tile %d, "		\
                  "assertion: " # ASSERTION,				\
                  current_jpeg, current_mcu_start);			\
      return false;							\
    }									\
  } while (0)

// for debugging
static bool verify_mcu_starts(int32_t num_jpegs, struct jpeg **jpegs,
                              GError **err) {
  for (int32_t current_jpeg = 0; current_jpeg < num_jpegs; current_jpeg++) {
    struct jpeg *jp = jpegs[current_jpeg];
    int32_t current_mcu_start = 0;
    CHK(jp->filename);
    g_autoptr(_openslide_file) f = _openslide_fopen(jp->filename, NULL);
    CHK(f);
    for (current_mcu_start = 1; current_mcu_start < jp->tile_count;
         current_mcu_start++) {
      int64_t offset = jp->mcu_starts[current_mcu_start];
      CHK(offset != -1);
      bool seek_ok = _openslide_fseek(f, offset - 2, SEEK_SET, NULL);
      CHK(seek_ok);
      uint8_t buf[2];
      size_t count = _openslide_fread(f, buf, sizeof(buf));
      CHK(count == sizeof(buf));
      CHK(buf[0] == 0xFF);  // prefix
      CHK(buf[1] >= 0xD0 && buf[1] <= 0xD7);  // marker
    }
  }
  return true;
}

static gpointer restart_marker_thread_func(gpointer d) {
  openslide_t *osr = d;
  struct hamamatsu_jpeg_ops_data *data = osr->data;

  int32_t current_jpeg = 0;
  int32_t current_mcu_start = 0;

  g_autoptr(_openslide_file) current_file = NULL;

  GError *tmp_err = NULL;

  while(current_jpeg < data->jpeg_count) {
    {
      g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
        g_mutex_locker_new(&data->restart_marker_cond_mutex);

      // should we pause?
      while (data->restart_marker_users && !data->restart_marker_thread_stop) {
        //      g_debug("thread paused");
        g_cond_wait(&data->restart_marker_cond,
                    &data->restart_marker_cond_mutex); // zzz
        //      g_debug("thread awoken");
      }

      // should we stop?
      if (data->restart_marker_thread_stop) {
        //      g_debug("thread stopping");
        break;
      }

      // should we sleep?
      int64_t end_time = data->restart_marker_last_used_time + G_TIME_SPAN_SECOND;
      if (data->restart_marker_thread_throttle &&
          end_time > g_get_monotonic_time()) {
        //g_debug("zz: %lu", end_time - g_get_monotonic_time());
        g_cond_wait_until(&data->restart_marker_cond,
                          &data->restart_marker_cond_mutex,
                          end_time);
        //      g_debug("running again");
        continue;
      }
    }

    // we are finally able to run

    //g_debug("current_jpeg: %d, current_mcu_start: %d",
    //        current_jpeg, current_mcu_start);

    struct jpeg *jp = data->all_jpegs[current_jpeg];
    if (jp->tile_count > 1) {
      if (current_file == NULL) {
	current_file = _openslide_fopen(jp->filename, &tmp_err);
	if (current_file == NULL) {
	  //g_debug("restart_marker_thread_func fopen failed");
	  break;
	}
      }

      if (!compute_mcu_start(osr, jp, current_file, current_mcu_start,
                             NULL, NULL, &tmp_err)) {
        //g_debug("restart_marker_thread_func compute_mcu_start failed");
        break;
      }

      current_mcu_start++;
      if (current_mcu_start >= jp->tile_count) {
	current_mcu_start = 0;
	current_jpeg++;
	_openslide_fclose(g_steal_pointer(&current_file));
      }
    } else {
      current_jpeg++;
    }
  }

  // store error, if any
  if (tmp_err) {
    //g_debug("restart_marker_thread_func failed: %s", tmp_err->message);
    g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
      g_mutex_locker_new(&data->restart_marker_cond_mutex);
    data->restart_marker_thread_error = tmp_err;
  }

  //  g_debug("restart_marker_thread_func done!");
  return NULL;
}

// if !use_jpeg_dimensions, use *w and *h instead of setting them
static bool validate_jpeg_header(struct _openslide_file *f,
                                 bool use_jpeg_dimensions,
                                 int32_t *w, int32_t *h,
                                 int32_t *tw, int32_t *th,
                                 int64_t *sof_position,
                                 int64_t *header_stop_position,
                                 char **comment, GError **err) {
  jmp_buf env;

  if (comment) {
    *comment = NULL;
  }

  // find limits of JPEG header
  int64_t header_start = _openslide_ftell(f, err);
  if (header_start == -1) {
    g_prefix_error(err, "Couldn't get header start position: ");
    return false;
  }
  if (!find_bitstream_start(f, sof_position, header_stop_position, err)) {
    return false;
  }

  struct jpeg_decompress_struct *cinfo;
  g_auto(_openslide_jpeg_decompress) dc =
    _openslide_jpeg_decompress_create(&cinfo);

  if (setjmp(env) == 0) {
    _openslide_jpeg_decompress_init(dc, &env);
    if (!jpeg_random_access_src(cinfo, f,
                                header_start, *sof_position,
                                *header_stop_position, -1, -1, err)) {
      return false;
    }

    if (comment) {
      // extract comment
      jpeg_save_markers(cinfo, JPEG_COM, 0xFFFF);
    }

    if (jpeg_read_header(cinfo, true) != JPEG_HEADER_OK) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG header");
      return false;
    }
    if (cinfo->num_components != 3) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "JPEG color components != 3");
      return false;
    }
    if (cinfo->restart_interval == 0) {
      g_set_error(err, OPENSLIDE_HAMAMATSU_ERROR,
                  OPENSLIDE_HAMAMATSU_ERROR_NO_RESTART_MARKERS,
                  "No restart markers");
      return false;
    }

    jpeg_start_decompress(cinfo);

    if (comment) {
      if (cinfo->marker_list) {
	// copy everything out
	g_autofree char *com =
	  g_strndup((const gchar *) cinfo->marker_list->data,
                    cinfo->marker_list->data_length);
	// but only really save everything up to the first '\0'
	*comment = g_strdup(com);
      }
      jpeg_save_markers(cinfo, JPEG_COM, 0);  // stop saving
    }

    if (use_jpeg_dimensions) {
      *w = cinfo->output_width;
      *h = cinfo->output_height;
    }

    int32_t mcu_width = DCTSIZE;
    int32_t mcu_height = DCTSIZE;
    if (cinfo->comps_in_scan > 1) {
      mcu_width = cinfo->max_h_samp_factor * DCTSIZE;
      mcu_height = cinfo->max_v_samp_factor * DCTSIZE;
    }

    // don't trust cinfo->MCUs_per_row, since it's based on libjpeg's belief
    // about the image width instead of the actual value
    uint32_t mcus_per_row = (*w / mcu_width) + !!(*w % mcu_width);

    if (cinfo->restart_interval > mcus_per_row) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Restart interval greater than MCUs per row");
      return false;
    }

    int leftover_mcus = mcus_per_row % cinfo->restart_interval;
    if (leftover_mcus != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Inconsistent restart marker spacing within row");
      return false;
    }

    *tw = mcu_width * cinfo->restart_interval;
    *th = mcu_height;

    //g_debug("size: %d %d, tile size: %d %d, mcu size: %d %d, restart_interval: %d, mcus_per_row: %u, leftover mcus: %d", *w, *h, *tw, *th, mcu_width, mcu_height, cinfo->restart_interval, mcus_per_row, leftover_mcus);
    return true;
  } else {
    // setjmp has returned again
    _openslide_jpeg_propagate_error(err, dc);
    return false;
  }
}

static int64_t *extract_optimisations_for_one_jpeg(struct _openslide_file *opt_f,
                                                   int32_t tiles_down,
                                                   int32_t tiles_across) {
  int32_t tile_count = tiles_across * tiles_down;
  g_autofree int64_t *mcu_starts = g_new(int64_t, tile_count);
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

    if (_openslide_fread(opt_f, u.buf, sizeof(u.buf)) != sizeof(u.buf)) {
      // EOF or error, we've done all we can

      if (row == 0) {
	// if we don't even get the first one, deallocate
	return NULL;
      }

      break;
    }

    // get the offset
    int64_t offset = GINT64_FROM_LE(u.i64);

    // record this marker
    mcu_starts[row * tiles_across] = offset;
  }

  return g_steal_pointer(&mcu_starts);
}

static void add_mpp_property(openslide_t *osr, GKeyFile *kf,
                             const char *group, const char *key,
                             int64_t pixels, const char *property) {
  int nm = g_key_file_get_integer(kf, group, key, NULL);
  if (nm > 0) {
    g_hash_table_insert(osr->properties,
                        g_strdup(property),
                        _openslide_format_double(nm / (1000.0 * pixels)));
  }
}

static void add_properties(openslide_t *osr,
                           GKeyFile *kf, const char *group,
                           struct _openslide_level *level0) {
  g_auto(GStrv) keys = g_key_file_get_keys(kf, group, NULL, NULL);
  if (keys == NULL) {
    return;
  }

  for (char **key = keys; *key != NULL; key++) {
    char *value = g_key_file_get_value(kf, group, *key, NULL);
    if (value) {
      g_hash_table_insert(osr->properties,
			  g_strdup_printf("hamamatsu.%s", *key),
                          value);
    }
  }

  // this allows openslide.objective-power to have a fractional component
  // but it's better than rounding
  _openslide_duplicate_double_prop(osr, "hamamatsu.SourceLens",
                                   OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  add_mpp_property(osr, kf, group, KEY_PHYSICAL_WIDTH, level0->w,
                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  add_mpp_property(osr, kf, group, KEY_PHYSICAL_HEIGHT, level0->h,
                   OPENSLIDE_PROPERTY_NAME_MPP_Y);
}

// create scale_denom levels
static void create_scaled_jpeg_levels(openslide_t *osr,
                                      GPtrArray *levels) {
  g_autoptr(GHashTable) expanded_levels =
    g_hash_table_new_full(g_int64_hash, g_int64_equal,
                          _openslide_int64_free,
                          (GDestroyNotify) jpeg_level_free);

  for (guint i = 0; i < levels->len; i++) {
    struct jpeg_level *l = g_steal_pointer(&levels->pdata[i]);

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
      sd_l->jpegs = g_memdup(l->jpegs, sizeof(struct jpeg *) * num_jpegs);

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
  g_ptr_array_set_size(levels, 0);

  // get sorted keys
  GList *level_keys = g_hash_table_get_keys(expanded_levels);
  level_keys = g_list_sort(level_keys, width_compare);
  //g_debug("number of keys: %d", g_list_length(level_keys));

  // load it
  while (level_keys != NULL) {
    // get a value
    struct jpeg_level *l = g_hash_table_lookup(expanded_levels,
                                               level_keys->data);

    // move
    g_ptr_array_add(levels, l);
    g_hash_table_steal(expanded_levels, level_keys->data);
    _openslide_int64_free(level_keys->data);  // key

    // consume the head and continue
    level_keys = g_list_delete_link(level_keys, level_keys);
  }
}

typedef openslide_t jpeg_osr;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(jpeg_osr, jpeg_do_destroy)

// consumes setup, even on failure
static bool init_jpeg_ops(openslide_t *_osr,
                          struct jpeg_setup *_setup,
                          bool background_thread,
                          GError **err) {
  g_autoptr(jpeg_setup) setup = _setup;

  // allocate private data
  g_autoptr(jpeg_osr) osr = _osr;
  g_assert(osr->data == NULL);
  struct hamamatsu_jpeg_ops_data *data =
    g_slice_new0(struct hamamatsu_jpeg_ops_data);
  data->jpeg_count = setup->jpegs->len;
  data->all_jpegs = (struct jpeg **)
    g_ptr_array_free(g_steal_pointer(&setup->jpegs), false);
  osr->data = data;

  // create scale_denom levels
  create_scaled_jpeg_levels(osr, setup->levels);

  // populate the level count and array
  g_assert(osr->levels == NULL);
  osr->level_count = setup->levels->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&setup->levels), false);

  // init background thread for finding restart markers
  data->restart_marker_last_used_time = g_get_monotonic_time();
  g_mutex_init(&data->restart_marker_mutex);
  g_cond_init(&data->restart_marker_cond);
  g_mutex_init(&data->restart_marker_cond_mutex);
  data->restart_marker_thread_throttle =
    !_openslide_debug(OPENSLIDE_DEBUG_JPEG_MARKERS);
  if (background_thread) {
    data->restart_marker_thread = g_thread_new("hamamatsu-marker",
                                               restart_marker_thread_func,
                                               osr);
  }

  // for debugging
  if (_openslide_debug(OPENSLIDE_DEBUG_JPEG_MARKERS)) {
    // run background thread to completion
    if (background_thread) {
      g_thread_join(g_steal_pointer(&data->restart_marker_thread));
    } else {
      restart_marker_thread_func(osr);
    }

    // check for errors
    {
      g_autoptr(GMutexLocker) locker G_GNUC_UNUSED =
        g_mutex_locker_new(&data->restart_marker_cond_mutex);
      if (data->restart_marker_thread_error) {
        g_propagate_error(err,
                          g_steal_pointer(&data->restart_marker_thread_error));
        return false;
      }
    }

    // verify results
    if (!verify_mcu_starts(data->jpeg_count, data->all_jpegs, err)) {
      return false;
    }
  }

  // set ops
  osr->ops = &hamamatsu_jpeg_ops;

  g_steal_pointer(&osr);
  return true;
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
				struct _openslide_file *optimisation_file,
				GError **err) {
  g_autoptr(jpeg_setup) setup = jpeg_setup_new();

  // process jpegs
  for (int i = 0; i < num_jpegs; i++) {
    struct jpeg *jp = g_slice_new0(struct jpeg);
    g_ptr_array_add(setup->jpegs, jp);

    jp->filename = g_strdup(image_filenames[i]);

    g_autoptr(_openslide_file) f = _openslide_fopen(jp->filename, err);
    if (f == NULL) {
      g_prefix_error(err, "Can't open JPEG %d: ", i);
      return false;
    }

    // comment?
    char *comment = NULL;
    char **comment_ptr = NULL;
    if (i == 0) {
      comment_ptr = &comment;
    }

    if (!validate_jpeg_header(f, true,
                              &jp->width, &jp->height,
                              &jp->tile_width, &jp->tile_height,
                              &jp->sof_position,
                              &jp->header_stop_position,
                              comment_ptr, err)) {
      g_prefix_error(err, "Can't validate JPEG %d: ", i);
      return false;
    }
    jp->tiles_across = jp->width / jp->tile_width;
    jp->tiles_down = jp->height / jp->tile_height;
    jp->tile_count = jp->tiles_across * jp->tiles_down;

    if (comment) {
      g_hash_table_insert(osr->properties,
			  g_strdup(OPENSLIDE_PROPERTY_NAME_COMMENT),
			  comment);
    }

    jp->end_in_file = _openslide_fsize(f, err);
    if (jp->end_in_file == -1) {
      g_prefix_error(err, "Can't read file size for JPEG %d: ", i);
      return false;
    }

    // init MCU starts
    jp->mcu_starts = g_new(int64_t, jp->tile_count);
    // init all to -1
    for (int32_t j = 0; j < jp->tile_count; j++) {
      (jp->mcu_starts)[j] = -1;
    }
  }

  // walk image files, ignoring the map file (which is last)
  const struct jpeg *jp0 = setup->jpegs->pdata[0];
  for (int i = 0; i < num_jpegs - 1; i++) {
    struct jpeg *jp = setup->jpegs->pdata[i];

    // ensure that all tile_{width,height} match image 0, and that all
    // tiles_{across,down} match image 0 except in the last column/row
    if (i > 0) {
      g_assert(jp0->tile_width != 0 && jp0->tile_height != 0 &&
               jp0->tiles_across != 0 && jp0->tiles_down != 0);
      if (jp0->tile_width != jp->tile_width ||
          jp0->tile_height != jp->tile_height) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Tile size not consistent for JPEG %d: "
                    "expected %dx%d, found %dx%d",
                    i, jp0->tile_width, jp0->tile_height,
                    jp->tile_width, jp->tile_height);
        return false;
      }
      if (i % num_jpeg_cols != num_jpeg_cols - 1 &&
          jp->tiles_across != jp0->tiles_across) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Tiles across not consistent for JPEG %d: "
                    "expected %d, found %d",
                    i, jp0->tiles_across, jp->tiles_across);
        return false;
      }
      if (i / num_jpeg_cols != num_jpeg_rows - 1 &&
          jp->tiles_down != jp0->tiles_down) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Tiles down not consistent for JPEG %d: "
                    "expected %d, found %d",
                    i, jp0->tiles_down, jp->tiles_down);
        return false;
      }
    }

    // use the optimisation file, if present
    // there appear to be no optimisations for the map file
    if (optimisation_file) {
      jp->unreliable_mcu_starts =
          extract_optimisations_for_one_jpeg(optimisation_file,
                                             jp->tiles_down,
                                             jp->tiles_across);
    }
    if (jp->unreliable_mcu_starts == NULL && optimisation_file != NULL) {
      // the optimisation file is useless, ignore it
      optimisation_file = NULL;
      _openslide_performance_warn("Bad optimisation file");
    }
  }

  // create levels: base image + map
  // base
  g_ptr_array_add(setup->levels,
                  create_jpeg_level(osr,
                                    (struct jpeg **) setup->jpegs->pdata,
                                    num_jpeg_cols, num_jpeg_rows));
  // map
  g_ptr_array_add(setup->levels,
                  create_jpeg_level(osr,
                                    (struct jpeg **) &setup->jpegs->pdata[num_jpegs - 1],
                                    1, 1));

  /*
  for (int32_t i = 0; i < level_count; i++) {
    struct jpeg_level *l = levels[i];
    g_debug("level %d", i);
    g_debug(" size %"PRId64" %"PRId64, l->base.w, l->base.h);
    g_debug(" tile size %d %d", l->tile_width, l->tile_height);
  }

  g_debug("num_jpegs: %d", num_jpegs);
  */

  // init ops
  return init_jpeg_ops(osr, g_steal_pointer(&setup), true, err);
}

static void ngr_level_free(struct ngr_level *l) {
  g_free(l->filename);
  _openslide_grid_destroy(l->grid);
  g_slice_free(struct ngr_level, l);
}

static void ngr_destroy(openslide_t *osr) {
  for (int i = 0; i < osr->level_count; i++) {
    ngr_level_free((struct ngr_level *) osr->levels[i]);
  }
  g_free(osr->levels);
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
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  // look up tile in cache
  uint32_t *tiledata = _openslide_cache_get(osr->cache, level, tile_x, tile_y,
                                            &cache_entry);

  if (!tiledata) {
    // read the tile data
    g_autoptr(_openslide_file) f = _openslide_fopen(l->filename, err);
    if (!f) {
      return false;
    }

    // compute offset to read
    int64_t offset = l->start_in_file +
      (tile_y * NGR_TILE_HEIGHT * l->column_width * 6) +
      (tile_x * l->base.h * l->column_width * 6);
    //g_debug("tile_x: %"PRId64", tile_y: %"PRId64", seeking to %"PRId64, tile_x, tile_y, offset);
    if (!_openslide_fseek(f, offset, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek to tile offset: ");
      return false;
    }

    // alloc and read
    g_auto(_openslide_slice) buf_box = _openslide_slice_alloc(tw * th * 6);
    if (_openslide_fread(f, buf_box.p, buf_box.len) != buf_box.len) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot read file %s", l->filename);
      return false;
    }

    // got the data, now convert to 8-bit xRGB
    tiledata = g_slice_alloc(tilesize);
    uint16_t *buf = buf_box.p;
    for (int i = 0; i < tw * th; i++) {
      // scale down from 12 bits
      uint8_t r = GINT16_FROM_LE(buf[(i * 3)]) >> 4;
      uint8_t g = GINT16_FROM_LE(buf[(i * 3) + 1]) >> 4;
      uint8_t b = GINT16_FROM_LE(buf[(i * 3) + 2]) >> 4;

      tiledata[i] = (r << 16) | (g << 8) | b;
    }

    // put it in the cache
    _openslide_cache_put(osr->cache, level, tile_x, tile_y,
                         tiledata,
                         tilesize,
                         &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                         CAIRO_FORMAT_RGB24,
                                         tw, th, tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

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

static int32_t read_le_int32_from_file(struct _openslide_file *f) {
  int32_t i;

  if (_openslide_fread(f, &i, 4) != 4) {
    return -1;
  }

  i = GINT32_FROM_LE(i);
  //  g_debug("%d", i);

  return i;
}

static bool hamamatsu_vmu_part2(openslide_t *osr,
				int num_levels, char **image_filenames,
				GError **err) {
  // initialize individual ngr structs
  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_with_free_func((GDestroyNotify) ngr_level_free);

  // open files
  for (int i = 0; i < num_levels; i++) {
    struct ngr_level *l = g_slice_new0(struct ngr_level);
    g_ptr_array_add(level_array, l);

    l->filename = g_strdup(image_filenames[i]);

    g_autoptr(_openslide_file) f = _openslide_fopen(l->filename, err);
    if (f == NULL) {
      return false;
    }

    // validate magic
    char buf[2];
    if (_openslide_fread(f, buf, sizeof(buf)) != sizeof(buf)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read magic on NGR file, level %d", i);
      return false;
    }
    if ((buf[0] != 'G') || (buf[1] != 'N')) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Bad magic on NGR file, level %d", i);
      return false;
    }

    // read w, h, column width, headersize
    if (!_openslide_fseek(f, 4, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek to NGR header: ");
      return false;
    }
    l->base.w = read_le_int32_from_file(f);
    l->base.h = read_le_int32_from_file(f);
    l->column_width = read_le_int32_from_file(f);

    if (!_openslide_fseek(f, 24, SEEK_SET, err)) {
      g_prefix_error(err, "Couldn't seek within NGR header: ");
      return false;
    }
    l->start_in_file = read_le_int32_from_file(f);

    // validate
    if ((l->base.w <= 0) || (l->base.h <= 0) ||
	(l->column_width <= 0) || (l->start_in_file <= 0)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read header, level %d", i);
      return false;
    }

    // ensure no remainder on columns
    if ((l->base.w % l->column_width) != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Width %"PRId64" not multiple of column width %d",
                  l->base.w, l->column_width);
      return false;
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
  }

  // set osr data
  g_assert(osr->levels == NULL);
  osr->level_count = level_array->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->ops = &ngr_ops;

  return true;
}


static bool hamamatsu_vms_vmu_open(openslide_t *osr, const char *filename,
                                   struct _openslide_tifflike *tl G_GNUC_UNUSED,
                                   struct _openslide_hash *quickhash1,
                                   GError **err) {
  // first, see if it's a VMS/VMU file
  g_autoptr(GKeyFile) key_file =
    _openslide_read_key_file(filename, KEY_FILE_MAX_SIZE,
                             G_KEY_FILE_NONE, err);
  if (!key_file) {
    g_prefix_error(err, "Can't load key file: ");
    return false;
  }

  // select group or fail, then read dimensions
  const char *groupname;
  int num_cols, num_rows;
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
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not VMS or VMU file");
    return false;
  }

  // revalidate cols/rows
  if (num_cols < 1 || num_rows < 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File missing columns or rows");
    return false;
  }

  // init the image filenames
  // this format has cols*rows image files, plus the map
  uint64_t num_images_tmp = ((uint64_t) num_cols * (uint64_t) num_rows) + 1;
  if (num_images_tmp > INT32_MAX) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Too many columns or rows");
    return false;
  }
  g_autoptr(GPtrArray) image_filenames =
    g_ptr_array_new_full(num_images_tmp, g_free);
  g_ptr_array_set_size(image_filenames, num_images_tmp);

  // hash in the key file
  if (!_openslide_hash_file(quickhash1, filename, err)) {
    return false;
  }

  // extract MapFile
  g_autofree char *dirname = g_path_get_dirname(filename);
  g_autofree char *map = g_key_file_get_string(key_file, groupname,
                                               KEY_MAP_FILE, NULL);
  if (map && *map) {
    char *map_filename = g_build_filename(dirname, map, NULL);

    image_filenames->pdata[image_filenames->len - 1] = map_filename;

    // hash in the map file
    if (!_openslide_hash_file(quickhash1, map_filename, err)) {
      return false;
    }
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read map file");
    return false;
  }

  // now each ImageFile
  g_auto(GStrv) all_keys = g_key_file_get_keys(key_file, groupname, NULL, NULL);
  for (char **tmp = all_keys; *tmp != NULL; tmp++) {
    char *key = *tmp;
    g_autofree char *value =
      g_key_file_get_string(key_file, groupname, key, NULL);

    //    g_debug("%s", key);

    if (g_str_has_prefix(key, KEY_IMAGE_FILE)) {
      char *suffix = key + strlen(KEY_IMAGE_FILE);

      int layer;
      int col;
      int row;

      g_auto(GStrv) split = g_strsplit(suffix, ",", 0);
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
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unknown number of image dimensions: %d",
                    g_strv_length(split));
        return false;
      }

      //g_debug("layer: %d, col: %d, row: %d", layer, col, row);

      if (layer != 0) {
        // skip non-zero layers for now
        continue;
      }

      if (col >= num_cols || row >= num_rows || col < 0 || row < 0) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Invalid row or column in Hamamatsu file (%d,%d)",
                    col, row);
        return false;
      }

      // compute index from x,y
      int i = row * num_cols + col;

      // init the file
      if (image_filenames->pdata[i]) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Duplicate image for (%d,%d)", col, row);
        return false;
      }
      image_filenames->pdata[i] = g_build_filename(dirname, value, NULL);
    }
  }

  // ensure all image filenames are filled
  for (guint i = 0; i < image_filenames->len; i++) {
    if (!image_filenames->pdata[i]) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read image filename %d", i);
      return false;
    }
  }

  // add macro image
  g_autofree char *macro = g_key_file_get_string(key_file, groupname,
                                                 KEY_MACRO_IMAGE, NULL);
  if (macro && *macro) {
    g_autofree char *macro_filename = g_build_filename(dirname, macro, NULL);
    if (!_openslide_jpeg_add_associated_image(osr, "macro", macro_filename,
                                              0, err)) {
      return false;
    }
  }

  // finalize depending on what format
  if (groupname == GROUP_VMS) {
    // open OptimisationFile
    g_autoptr(_openslide_file) optimisation_file = NULL;
    g_autofree char *opt = g_key_file_get_string(key_file, GROUP_VMS,
				                 KEY_OPTIMISATION_FILE, NULL);
    if (opt) {
      g_autofree char *optimisation_filename =
        g_build_filename(dirname, opt, NULL);

      optimisation_file = _openslide_fopen(optimisation_filename, NULL);

      if (optimisation_file == NULL) {
	// g_debug("Can't open optimisation file");
      }
    } else {
      // g_debug("Optimisation file key not present");
    }
    if (!optimisation_file) {
      _openslide_performance_warn("Missing optimisation file");
    }

    // do all the jpeg stuff
    if (!hamamatsu_vms_part2(osr,
                             image_filenames->len,
                             (char **) image_filenames->pdata,
                             num_cols, num_rows,
                             optimisation_file,
                             err)) {
      return false;
    }
  } else if (groupname == GROUP_VMU) {
    // verify a few assumptions for VMU
    int bits_per_pixel = g_key_file_get_integer(key_file,
						GROUP_VMU,
						KEY_BITS_PER_PIXEL,
						NULL);
    g_autofree char *pixel_order = g_key_file_get_string(key_file, GROUP_VMU,
					                 KEY_PIXEL_ORDER, NULL);

    if (bits_per_pixel != 36) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "%s must be 36", KEY_BITS_PER_PIXEL);
    } else if (!pixel_order || (strcmp(pixel_order, "RGB") != 0)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "%s must be RGB", KEY_PIXEL_ORDER);
    } else {
      // assumptions verified
      if (!hamamatsu_vmu_part2(osr,
                               image_filenames->len,
                               (char **) image_filenames->pdata,
                               err)) {
        return false;
      }
    }
  } else {
    g_assert_not_reached();
  }

  // now that we have the level 0 dimensions, add properties
  add_properties(osr, key_file, groupname, osr->levels[0]);

  return true;
}

const struct _openslide_format _openslide_format_hamamatsu_vms_vmu = {
  .name = "hamamatsu-vms-vmu",
  .vendor = "hamamatsu",
  .detect = hamamatsu_vms_vmu_detect,
  .open = hamamatsu_vms_vmu_open,
};

static bool hamamatsu_ndpi_detect(const char *filename G_GNUC_UNUSED,
                                  struct _openslide_tifflike *tl,
                                  GError **err) {
  // ensure we have a tifflike
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // check for a TIFF tag unique to NDPI and always present in it
  if (!_openslide_tifflike_get_value_count(tl, 0, NDPI_FORMAT_FLAG)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No TIFF tag %d", NDPI_FORMAT_FLAG);
    return false;
  }

  return true;
}

static void ndpi_set_sint_prop(openslide_t *osr,
                               struct _openslide_tifflike *tl,
                               int64_t dir, int32_t tag,
                               const char *property_name) {
  GError *tmp_err = NULL;
  int64_t value = _openslide_tifflike_get_sint(tl, dir, tag, &tmp_err);
  if (!tmp_err) {
    g_hash_table_insert(osr->properties,
                        g_strdup(property_name),
                        g_strdup_printf("%"PRId64, value));
  }
  g_clear_error(&tmp_err);
}

static void ndpi_set_float_prop(openslide_t *osr,
                                struct _openslide_tifflike *tl,
                                int64_t dir, int32_t tag,
                                const char *property_name) {
  GError *tmp_err = NULL;
  double value = _openslide_tifflike_get_float(tl, dir, tag, &tmp_err);
  if (!tmp_err) {
    g_hash_table_insert(osr->properties,
                        g_strdup(property_name),
                        _openslide_format_double(value));
  }
  g_clear_error(&tmp_err);
}

static void ndpi_set_resolution_prop(openslide_t *osr,
                                     struct _openslide_tifflike *tl,
                                     int64_t dir, int32_t tag,
                                     const char *property_name) {
  GError *tmp_err = NULL;
  uint64_t unit = _openslide_tifflike_get_uint(tl, dir,
                                               TIFFTAG_RESOLUTIONUNIT,
                                               &tmp_err);
  if (g_error_matches(tmp_err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_NO_VALUE)) {
    unit = RESUNIT_INCH;  // default
    g_clear_error(&tmp_err);
  } else if (tmp_err) {
    g_clear_error(&tmp_err);
    return;
  }

  double res = _openslide_tifflike_get_float(tl, dir, tag, &tmp_err);
  if (!tmp_err && unit == RESUNIT_CENTIMETER) {
    g_hash_table_insert(osr->properties,
                        g_strdup(property_name),
                        _openslide_format_double(10000.0 / res));
  }
  g_clear_error(&tmp_err);
}

static void ndpi_set_string_prop(openslide_t *osr,
                                 struct _openslide_tifflike *tl,
                                 int64_t dir, int32_t tag,
                                 const char *property_name) {
  const char *value = _openslide_tifflike_get_buffer(tl, dir, tag, NULL);
  if (value) {
    g_hash_table_insert(osr->properties,
                        g_strdup(property_name),
                        g_strdup(value));
  }
}

static void ndpi_set_props(openslide_t *osr,
                           struct _openslide_tifflike *tl, int64_t dir) {
  // MPP
  ndpi_set_resolution_prop(osr, tl, dir, TIFFTAG_XRESOLUTION,
                           OPENSLIDE_PROPERTY_NAME_MPP_X);
  ndpi_set_resolution_prop(osr, tl, dir, TIFFTAG_YRESOLUTION,
                           OPENSLIDE_PROPERTY_NAME_MPP_Y);

  // objective power
  ndpi_set_float_prop(osr, tl, dir, NDPI_SOURCELENS,
                      "hamamatsu.SourceLens");
  ndpi_set_float_prop(osr, tl, dir, NDPI_SOURCELENS,
                      OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);

  // misc properties
  ndpi_set_sint_prop(osr, tl, dir, NDPI_XOFFSET,
                     "hamamatsu.XOffsetFromSlideCentre");
  ndpi_set_sint_prop(osr, tl, dir, NDPI_YOFFSET,
                     "hamamatsu.YOffsetFromSlideCentre");
  ndpi_set_string_prop(osr, tl, dir, NDPI_REFERENCE,
                       "hamamatsu.Reference");

  // ASCII property map
  const char *props = _openslide_tifflike_get_buffer(tl, dir,
                                                     NDPI_PROPERTY_MAP, NULL);
  if (props) {
    g_auto(GStrv) records = g_strsplit(props, "\r\n", 0);
    for (char **cur_record = records; *cur_record; cur_record++) {
      g_auto(GStrv) pair = g_strsplit(*cur_record, "=", 2);
      if (pair[0] && pair[0][0] && pair[1] && pair[1][0]) {
        g_hash_table_insert(osr->properties,
                            g_strdup_printf("hamamatsu.%s", pair[0]),
                            g_strdup(pair[1]));
      }
    }
  }
}

static bool hamamatsu_ndpi_open(openslide_t *osr, const char *filename,
                                struct _openslide_tifflike *tl,
                                struct _openslide_hash *quickhash1,
                                GError **err) {
  g_autoptr(jpeg_setup) setup = jpeg_setup_new();
  GError *tmp_err = NULL;
  bool restart_marker_scan = false;

  // open file
  g_autoptr(_openslide_file) f = _openslide_fopen(filename, err);
  if (!f) {
    return false;
  }

  // walk directories
  int64_t directories = _openslide_tifflike_get_directory_count(tl);
  int64_t min_width = INT64_MAX;
  int64_t min_width_dir = 0;
  for (int64_t dir = 0; dir < directories; dir++) {
    // read tags
    int64_t width, height, rows_per_strip, start_in_file, num_bytes;
    TIFF_GET_UINT_OR_RETURN(tl, dir, TIFFTAG_IMAGEWIDTH, width, false);
    TIFF_GET_UINT_OR_RETURN(tl, dir, TIFFTAG_IMAGELENGTH, height, false);
    TIFF_GET_UINT_OR_RETURN(tl, dir, TIFFTAG_ROWSPERSTRIP, rows_per_strip, false);
    TIFF_GET_UINT_OR_RETURN(tl, dir, TIFFTAG_STRIPOFFSETS, start_in_file, false);
    TIFF_GET_UINT_OR_RETURN(tl, dir, TIFFTAG_STRIPBYTECOUNTS, num_bytes, false);
    start_in_file = _openslide_tifflike_uint_fix_offset_ndpi(tl, dir,
                                                             start_in_file);

    double lens =
      _openslide_tifflike_get_float(tl, dir, NDPI_SOURCELENS, &tmp_err);
    if (tmp_err) {
      g_propagate_error(err, tmp_err);
      return false;
    }

    // check results
    if (height != rows_per_strip) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unexpected rows per strip %"PRId64" (height %"PRId64")",
                  rows_per_strip, height);
      return false;
    }

    if (lens > 0) {
      // is a pyramid level

      // ignore focal planes != 0
      int64_t focal_plane =
        _openslide_tifflike_get_sint(tl, dir, NDPI_FOCAL_PLANE, &tmp_err);
      if (tmp_err) {
        g_propagate_error(err, tmp_err);
        return false;
      }
      if (focal_plane != 0) {
        continue;
      }

      // is smallest level?
      if (width < min_width) {
        min_width = width;
        min_width_dir = dir;
      } else {
        // The slide's levels are in an unexpected order.  Reject the slide
        // out of paranoia.
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Unexpected directory layout");
        return false;
      }

      // will the JPEG image dimensions be valid?
      bool dimensions_valid = (width <= JPEG_MAX_DIMENSION &&
                               height <= JPEG_MAX_DIMENSION);

      // validate JPEG
      int32_t jp_w = width;  // overwritten if dimensions_valid
      int32_t jp_h = height; // overwritten if dimensions_valid
      int32_t jp_tw, jp_th;
      int64_t sof_position, header_stop_position;
      if (!_openslide_fseek(f, start_in_file, SEEK_SET, err)) {
        g_prefix_error(err, "Couldn't seek to JPEG start: ");
        return false;
      }
      if (!validate_jpeg_header(f, dimensions_valid,
                                &jp_w, &jp_h,
                                &jp_tw, &jp_th,
                                &sof_position, &header_stop_position,
                                NULL, &tmp_err)) {
        if (g_error_matches(tmp_err, OPENSLIDE_HAMAMATSU_ERROR,
                            OPENSLIDE_HAMAMATSU_ERROR_NO_RESTART_MARKERS)) {
          // non-tiled image
          //g_debug("non-tiled image %"PRId64, dir);
          g_clear_error(&tmp_err);
          jp_w = jp_tw = width;
          jp_h = jp_th = height;
        } else {
          g_propagate_prefixed_error(err, tmp_err,
                                     "Can't validate JPEG for directory "
                                     "%"PRId64": ", dir);
          return false;
        }
      }
      if (width != jp_w || height != jp_h) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "JPEG dimension mismatch for directory %"PRId64": "
                    "expected %"PRId64"x%"PRId64", found %dx%d",
                    dir, width, height, jp_w, jp_h);
        return false;
      }

      // init jpeg
      struct jpeg *jp = g_slice_new0(struct jpeg);
      g_ptr_array_add(setup->jpegs, jp);
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
      jp->sof_position = sof_position;
      jp->header_stop_position = header_stop_position;
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
          //g_debug("loading MCU starts for directory %"PRId64, dir);
          const uint64_t *unreliable_mcu_starts =
            _openslide_tifflike_get_uints(tl, dir, NDPI_MCU_STARTS, NULL);
          if (unreliable_mcu_starts) {
            jp->unreliable_mcu_starts = g_new(int64_t, mcu_start_count);
            for (int64_t tile = 0; tile < mcu_start_count; tile++) {
              jp->unreliable_mcu_starts[tile] =
                jp->start_in_file + unreliable_mcu_starts[tile];
              //g_debug("mcu start at %"PRId64, jp->unreliable_mcu_starts[tile]);
            }
          } else {
            //g_debug("failed to load MCU starts for directory %"PRId64, dir);
          }
        }

        if (jp->unreliable_mcu_starts == NULL) {
          // no marker positions; scan for them in the background
          //g_debug("enabling restart marker thread for directory %"PRId64, dir);
          _openslide_performance_warn("Bad or missing MCU starts for "
                                      "directory %"PRId64, dir);
          restart_marker_scan = true;
        }
      }

      // create level
      g_ptr_array_add(setup->levels, create_jpeg_level(osr, &jp, 1, 1));

    } else if (lens == -1) {
      // macro image
      if (!_openslide_jpeg_add_associated_image(osr, "macro",
                                                filename, start_in_file,
                                                err)) {
        return false;
      }
    }
  }

  // verify we found some levels
  if (setup->levels->len == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't find any pyramid levels");
    return false;
  }

  // init properties and set hash
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    min_width_dir, 0,
                                                    err)) {
    return false;
  }
  ndpi_set_props(osr, tl, 0);

  // init ops
  return init_jpeg_ops(osr, g_steal_pointer(&setup),
                       restart_marker_scan, err);
}

const struct _openslide_format _openslide_format_hamamatsu_ndpi = {
  .name = "hamamatsu-ndpi",
  .vendor = "hamamatsu",
  .detect = hamamatsu_ndpi_detect,
  .open = hamamatsu_ndpi_open,
};
