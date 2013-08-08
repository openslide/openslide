/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
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

#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <jpeglib.h>
#include <jerror.h>
#include <math.h>

#include <cairo.h>

#include "openslide-cache.h"
#include "openslide-tilehelper.h"

struct one_jpeg {
  char *filename;
  int64_t start_in_file;
  int64_t end_in_file;

  int32_t tile_width;
  int32_t tile_height;

  int32_t mcu_starts_count;
  int64_t *mcu_starts;
  int64_t *unreliable_mcu_starts;
};

struct tile {
  struct one_jpeg *jpeg;
  int32_t jpegno;   // used only for cache lookup
  int32_t tileno;

  // physical tile size (after scaling)
  int32_t tile_width;
  int32_t tile_height;

  // bounds in the physical tile?
  double src_x;
  double src_y;
  double w;
  double h;
};

struct level {
  struct _openslide_level info;
  struct _openslide_grid_tilemap *grid;

  int32_t tiles_across;
  int32_t tiles_down;

  int32_t scale_denom;

  // note: everything below is pre-divided by scale_denom

  double tile_advance_x;
  double tile_advance_y;
};

struct jpegops_data {
  int32_t jpeg_count;
  struct one_jpeg **all_jpegs;

  // thread stuff, for background search of restart markers
  GTimer *restart_marker_timer;
  GMutex *restart_marker_mutex;
  GThread *restart_marker_thread;

  GCond *restart_marker_cond;
  GMutex *restart_marker_cond_mutex;
  uint32_t restart_marker_users;
  bool restart_marker_thread_stop;
};

struct jpeg_associated_image_ctx {
  char *filename;
  int64_t offset;
};


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

static void jpeg_random_access_src (openslide_t *osr,
				    j_decompress_ptr cinfo, FILE *infile,
				    int64_t header_start_position,
				    int64_t header_stop_position,
				    int64_t start_position, int64_t stop_position) {
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
  if ((header_start_position == -1) || (header_stop_position == -1) ||
      (start_position == -1) || (stop_position == -1) ||
      (header_start_position >= header_stop_position) ||
      (header_stop_position > start_position) ||
      (start_position >= stop_position)) {
    _openslide_set_error(osr, "Can't do random access JPEG read: "
	       "header_start_position: %" G_GINT64_FORMAT ", "
	       "header_stop_position: %" G_GINT64_FORMAT ", "
	       "start_position: %" G_GINT64_FORMAT ", "
	       "stop_position: %" G_GINT64_FORMAT,
	       header_start_position, header_stop_position,
	       start_position, stop_position);

    src->buffer_size = 0;
    src->pub.bytes_in_buffer = 0;
    src->buffer = NULL;
    return;
  }

  // compute size of buffer and allocate
  int header_length = header_stop_position - header_start_position;
  int data_length = stop_position - start_position;

  src->buffer_size = header_length + data_length;
  src->pub.bytes_in_buffer = src->buffer_size;
  src->buffer = g_slice_alloc(src->buffer_size);

  src->pub.next_input_byte = src->buffer;

  // read in the 2 parts
  //  g_debug("reading header from %" G_GINT64_FORMAT, header_start_position);
  fseeko(infile, header_start_position, SEEK_SET);
  if (!fread(src->buffer, header_length, 1, infile)) {
    _openslide_set_error(osr, "Cannot read header in JPEG");
    return;
  }
  //  g_debug("reading from %" G_GINT64_FORMAT, start_position);
  fseeko(infile, start_position, SEEK_SET);
  if (!fread(src->buffer + header_length, data_length, 1, infile)) {
    _openslide_set_error(osr, "Cannot read data in JPEG");
    return;
  }

  // change the final byte to EOI
  if (src->buffer[src->buffer_size - 2] != 0xFF) {
    _openslide_set_error(osr, "Expected 0xFF byte at end of JPEG data");
    return;
  }
  src->buffer[src->buffer_size - 1] = JPEG_EOI;
}

static void level_free(gpointer data) {
  //g_debug("level_free: %p", data);

  struct level *l = data;

  //  g_debug("g_free(%p)", l->level_jpegs);
  _openslide_grid_tilemap_destroy(l->grid);
  g_slice_free(struct level, l);
}

static void tile_free(gpointer data) {
  g_slice_free(struct tile, data);
}

static void struct_openslide_jpeg_tile_free(gpointer data) {
  g_slice_free(struct _openslide_jpeg_tile, data);
}

struct convert_tiles_args {
  struct level *new_l;
  struct one_jpeg **all_jpegs;
};

static void convert_tiles(gpointer key,
			  gpointer value,
			  gpointer user_data) {
  struct convert_tiles_args *args = user_data;
  struct _openslide_jpeg_tile *old_tile = value;
  struct level *new_l = args->new_l;

  // create new tile
  struct tile *new_tile = g_slice_new(struct tile);
  new_tile->jpeg = args->all_jpegs[old_tile->fileno];
  new_tile->jpegno = old_tile->fileno;
  new_tile->tileno = old_tile->tileno;
  new_tile->tile_width = new_tile->jpeg->tile_width / new_l->scale_denom;
  new_tile->tile_height = new_tile->jpeg->tile_height / new_l->scale_denom;
  new_tile->src_x = old_tile->src_x / new_l->scale_denom;
  new_tile->src_y = old_tile->src_y / new_l->scale_denom;
  new_tile->w = old_tile->w / new_l->scale_denom;
  new_tile->h = old_tile->h / new_l->scale_denom;

  // we only issue tile size hints if:
  // - advances are integers (checked below)
  // - no tile has a delta from the standard advance
  // - no tiles overlap
  if (new_tile->w != new_l->tile_advance_x ||
      new_tile->h != new_l->tile_advance_y ||
      old_tile->dest_offset_x ||
      old_tile->dest_offset_y) {
    // clear
    new_l->info.tile_w = 0;
    new_l->info.tile_h = 0;
  }

  // add to grid
  int64_t index = *((int64_t *) key);
  _openslide_grid_tilemap_add_tile(new_l->grid,
                                   index % new_l->tiles_across,
                                   index / new_l->tiles_across,
                                   old_tile->dest_offset_x / new_l->scale_denom,
                                   old_tile->dest_offset_y / new_l->scale_denom,
                                   new_tile->w, new_tile->h,
                                   new_tile);
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

static void _compute_mcu_start(openslide_t *osr,
			       struct one_jpeg *jpeg,
			       FILE *f,
			       int64_t target) {
  // special case for first
  if (jpeg->mcu_starts[0] == -1) {
    struct jpeg_decompress_struct cinfo;
    struct _openslide_jpeg_error_mgr jerr;
    jmp_buf env;

    // init jpeg
    fseeko(f, jpeg->start_in_file, SEEK_SET);

    if (setjmp(env) == 0) {
      cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
      jpeg_create_decompress(&cinfo);
      _openslide_jpeg_stdio_src(&cinfo, f);
      jpeg_read_header(&cinfo, TRUE);
      jpeg_start_decompress(&cinfo);
    } else {
      // setjmp returns again
      _openslide_set_error(osr, "Error initializing JPEG: %s",
                           jerr.err->message);
      g_clear_error(&jerr.err);
    }

    // set the first entry
    jpeg->mcu_starts[0] = ftello(f) - cinfo.src->bytes_in_buffer;

    // done
    jpeg_destroy_decompress(&cinfo);
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
        _openslide_set_error(osr, "Restart marker not found in expected place");
      } else {
        //  g_debug("accepted unreliable marker %"G_GINT64_FORMAT, first_good);
        jpeg->mcu_starts[first_good] = offset;
        break;
      }
    }
  }

  if (first_good == target) {
    // we're done
    return;
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
      _openslide_set_error(osr, "after_marker_pos == -1");
      break;
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
}

static void compute_mcu_start(openslide_t *osr,
			      struct one_jpeg *jpeg,
			      FILE *f,
			      int64_t tileno,
			      int64_t *header_stop_position,
			      int64_t *start_position,
			      int64_t *stop_position) {
  struct jpegops_data *data = osr->data;

  g_mutex_lock(data->restart_marker_mutex);

  _compute_mcu_start(osr, jpeg, f, tileno);

  // end of header; always computed by _compute_mcu_start
  if (header_stop_position) {
    *header_stop_position = jpeg->mcu_starts[0];
  }

  // start of data stream
  if (start_position) {
    *start_position = jpeg->mcu_starts[tileno];
  }

  // end of data stream
  if (stop_position) {
    if (jpeg->mcu_starts_count == tileno + 1) {
      // EOF
      *stop_position = jpeg->end_in_file;
    } else {
      _compute_mcu_start(osr, jpeg, f, tileno + 1);
      *stop_position = jpeg->mcu_starts[tileno + 1];
    }
  }

  g_mutex_unlock(data->restart_marker_mutex);
}

static uint32_t *read_from_one_jpeg (openslide_t *osr,
				     struct one_jpeg *jpeg,
				     int32_t tileno,
				     int32_t scale_denom,
				     int w, int h) {
  GError *tmp_err = NULL;

  uint32_t *dest = g_slice_alloc(w * h * 4);

  // open file
  FILE *f = _openslide_fopen(jpeg->filename, "rb", &tmp_err);
  if (f == NULL) {
    // fail
    _openslide_set_error_from_gerror(osr, tmp_err);
    g_clear_error(&tmp_err);
    memset(dest, 0, w * h * 4);
    return dest;
  }

  // begin decompress
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  jmp_buf env;

  gsize row_size = 0;

  JSAMPARRAY buffer = (JSAMPARRAY) g_slice_alloc0(sizeof(JSAMPROW) * MAX_SAMP_FACTOR);

  if (setjmp(env) == 0) {
    // figure out where to start the data stream
    int64_t header_stop_position;
    int64_t start_position;
    int64_t stop_position;
    compute_mcu_start(osr, jpeg, f, tileno,
                      &header_stop_position,
                      &start_position,
                      &stop_position);

    // set error handler, this will longjmp if necessary
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);

    // start decompressing
    jpeg_create_decompress(&cinfo);

    jpeg_random_access_src(osr, &cinfo, f,
			   jpeg->start_in_file,
			   header_stop_position,
			   start_position,
			   stop_position);

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
      _openslide_set_error(osr,
			   "Dimensional mismatch in read_from_one_jpeg, "
			   "expected %dx%d, got %dx%d",
			   w, h, cinfo.output_width, cinfo.output_height);
    } else {
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
	  for (int i = 0; i < w; i++) {
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
    }
  } else {
    // setjmp returns again
    _openslide_set_error(osr, "JPEG decompression failed: %s",
                         jerr.err->message);
    g_clear_error(&jerr.err);
  }

  // free buffers
  for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
    g_slice_free1(row_size, buffer[i]);
  }
  g_slice_free1(sizeof(JSAMPROW) * MAX_SAMP_FACTOR, buffer);

  // stop jpeg
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo.src;   // sorry
  g_slice_free1(src->buffer_size, src->buffer);
  jpeg_destroy_decompress(&cinfo);

  fclose(f);

  return dest;
}

static void read_tile(openslide_t *osr,
		      cairo_t *cr,
		      struct _openslide_level *level,
		      void *data,
		      void *arg G_GNUC_UNUSED) {
  //g_debug("read_tile");
  struct level *l = (struct level *) level;
  struct tile *requested_tile = data;

  int tw = requested_tile->tile_width;
  int th = requested_tile->tile_height;

  //g_debug("jpeg read_tile: src: %g %g, dim: %d %d, tile dim: %g %g", requested_tile->src_x, requested_tile->src_y, requested_tile->jpeg->tile_width, requested_tile->jpeg->tile_height, requested_tile->w, requested_tile->h);

  // get the jpeg data, possibly from cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            requested_tile->jpegno,
                                            requested_tile->tileno,
                                            level,
                                            &cache_entry);

  if (!tiledata) {
    tiledata = read_from_one_jpeg(osr,
				  requested_tile->jpeg,
				  requested_tile->tileno,
				  l->scale_denom,
				  tw, th);

    _openslide_cache_put(osr->cache,
			 requested_tile->jpegno, requested_tile->tileno, level,
			 tiledata,
			 tw * th * 4,
			 &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
								 CAIRO_FORMAT_RGB24,
								 tw, th,
								 tw * 4);

  double src_x = requested_tile->src_x;
  double src_y = requested_tile->src_y;

  // if we are drawing a subregion of the tile, we must do an additional copy,
  // because cairo lacks source clipping
  if ((requested_tile->tile_width > requested_tile->w) ||
      (requested_tile->tile_height > requested_tile->h)) {
    cairo_surface_t *surface2 = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
							   ceil(requested_tile->w),
							   ceil(requested_tile->h));
    cairo_t *cr2 = cairo_create(surface2);
    cairo_set_source_surface(cr2, surface, -src_x, -src_y);

    // replace original image surface and reset origin
    cairo_surface_destroy(surface);
    surface = surface2;
    src_x = 0;
    src_y = 0;

    cairo_rectangle(cr2, 0, 0,
		    ceil(requested_tile->w),
		    ceil(requested_tile->h));
    cairo_fill(cr2);
    _openslide_check_cairo_status_possibly_set_error(osr, cr2);
    cairo_destroy(cr2);
  }

  cairo_set_source_surface(cr, surface,
			   -src_x, -src_y);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  /*
  cairo_save(cr);
  cairo_set_source_rgba(cr, 1.0, 0, 0, 0.2);
  cairo_rectangle(cr, 0, 0, 4, 4);
  cairo_fill(cr);
  */

  /*
  cairo_fill_preserve(cr);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_stroke(cr);
  char *yt = g_strdup_printf("%d", tile_y);
  cairo_move_to(cr, 0, tile->h);
  cairo_show_text(cr, yt);
  cairo_translate(cr,
		  -tile->dest_offset_x,
		  -tile->dest_offset_y);
  cairo_set_source_rgba(cr, 0, 0, 1, 0.2);
  cairo_rectangle(cr, 0, 0,
		  tile->w, tile->h);
  cairo_stroke(cr);
  cairo_move_to(cr, 0, tile->h);
  cairo_show_text(cr, yt);
  g_free(yt);
  cairo_restore(cr);
  */


  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);
}


static void paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 struct _openslide_level *level,
			 int32_t w, int32_t h) {
  struct jpegops_data *data = osr->data;
  struct level *l = (struct level *) level;

  // tell the background thread to pause
  g_mutex_lock(data->restart_marker_cond_mutex);
  data->restart_marker_users++;
  //  g_debug("telling thread to pause");
  g_mutex_unlock(data->restart_marker_cond_mutex);

  // paint
  _openslide_grid_tilemap_paint_region(l->grid, cr, NULL, x, y, level, w, h);

  // maybe tell the background thread to resume
  g_mutex_lock(data->restart_marker_cond_mutex);
  if (!--data->restart_marker_users) {
    g_timer_start(data->restart_marker_timer);
    //  g_debug("telling thread to awaken");
    g_cond_signal(data->restart_marker_cond);
  }
  g_mutex_unlock(data->restart_marker_cond_mutex);
}

static void destroy(openslide_t *osr) {
  struct jpegops_data *data = osr->data;

  // tell the thread to finish and wait
  g_mutex_lock(data->restart_marker_cond_mutex);
  g_warn_if_fail(data->restart_marker_users == 0);
  data->restart_marker_thread_stop = true;
  g_cond_signal(data->restart_marker_cond);
  g_mutex_unlock(data->restart_marker_cond_mutex);
  g_thread_join(data->restart_marker_thread);

  // each jpeg in turn
  for (int32_t i = 0; i < data->jpeg_count; i++) {
    struct one_jpeg *jpeg = data->all_jpegs[i];
    g_free(jpeg->filename);
    g_free(jpeg->mcu_starts);
    g_free(jpeg->unreliable_mcu_starts);
    g_slice_free(struct one_jpeg, jpeg);
  }

  // each level in turn
  for (int32_t i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];

    //    g_debug("g_free(%p)", l->level_jpegs);
    _openslide_grid_tilemap_destroy(l->grid);
    g_slice_free(struct level, l);
  }

  // the level array
  g_free(osr->levels);

  // the JPEG array
  g_free(data->all_jpegs);

  // the background stuff
  g_mutex_free(data->restart_marker_mutex);
  g_timer_destroy(data->restart_marker_timer);
  g_cond_free(data->restart_marker_cond);
  g_mutex_free(data->restart_marker_cond_mutex);

  // the structure
  g_slice_free(struct jpegops_data, data);
}

static const struct _openslide_ops jpeg_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static void init_one_jpeg(struct one_jpeg *onej,
			  struct _openslide_jpeg_file *file) {
  g_assert(file->filename);

  onej->filename = file->filename;
  onej->start_in_file = file->start_in_file;
  onej->end_in_file = file->end_in_file;
  onej->unreliable_mcu_starts = file->mcu_starts;

  g_assert(file->w && file->h && file->tw && file->th);

  onej->tile_width = file->tw;
  onej->tile_height = file->th;

  // compute the mcu starts stuff
  onej->mcu_starts_count =
    (file->w / onej->tile_width) *
    (file->h / onej->tile_height);

  onej->mcu_starts = g_new(int64_t,
			   onej->mcu_starts_count);

  // init all to -1
  for (int32_t i = 0; i < onej->mcu_starts_count; i++) {
    (onej->mcu_starts)[i] = -1;
  }
}

static gint width_compare(gconstpointer a, gconstpointer b) {
  int64_t w1 = *((const int64_t *) a);
  int64_t w2 = *((const int64_t *) b);

  g_assert(w1 >= 0 && w2 >= 0);

  return (w1 < w2) - (w1 > w2);
}

// warning: calls g_assert for trivial things, use only for debugging
static void verify_mcu_starts(struct jpegops_data *data) {
  g_debug("verifying mcu starts");

  int32_t current_jpeg = 0;
  int32_t current_mcu_start = 0;

  while(current_jpeg < data->jpeg_count) {
    struct one_jpeg *oj = data->all_jpegs[current_jpeg];

    g_assert(oj->filename);
    if (current_mcu_start > 0) {
      int64_t offset = oj->mcu_starts[current_mcu_start];
      g_assert(offset != -1);
      FILE *f = _openslide_fopen(oj->filename, "rb", NULL);
      g_assert(f);
      fseeko(f, offset - 2, SEEK_SET);
      g_assert(getc(f) == 0xFF);
      int marker = getc(f);
      g_assert(marker >= 0xD0 && marker <= 0xD7);
      fclose(f);
    }

    current_mcu_start++;
    if (current_mcu_start >= oj->mcu_starts_count) {
      current_mcu_start = 0;
      current_jpeg++;
      g_debug("done verifying jpeg %d", current_jpeg);
    }
  }
}

static gpointer restart_marker_thread_func(gpointer d) {
  openslide_t *osr = d;
  struct jpegops_data *data = osr->data;

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

    struct one_jpeg *oj = data->all_jpegs[current_jpeg];
    if (oj->mcu_starts_count > 1) {
      if (current_file == NULL) {
	current_file = _openslide_fopen(oj->filename, "rb", &tmp_err);
	if (current_file == NULL) {
	  //g_debug("restart_marker_thread_func fopen failed");
	  _openslide_set_error_from_gerror(osr, tmp_err);
	  g_clear_error(&tmp_err);
	  break;
	}
      }

      compute_mcu_start(osr, oj, current_file, current_mcu_start,
                        NULL, NULL, NULL);
      if (openslide_get_error(osr)) {
        //g_debug("restart_marker_thread_func compute_mcu_start failed");
        break;
      }

      current_mcu_start++;
      if (current_mcu_start >= oj->mcu_starts_count) {
	current_mcu_start = 0;
	current_jpeg++;
	fclose(current_file);
	current_file = NULL;
      }
    } else {
      current_jpeg++;
    }
  }

  //  g_debug("restart_marker_thread_func done!");
  return NULL;
}

static int one_jpeg_compare(const void *a, const void *b) {
  const struct one_jpeg *aa = *(struct one_jpeg * const *) a;
  const struct one_jpeg *bb = *(struct one_jpeg * const *) b;

  // compare files
  int res = strcmp(aa->filename, bb->filename);
  if (res != 0) {
    return res;
  }

  // compare offsets
  if (aa->start_in_file < bb->start_in_file) {
    return -1;
  } else if (aa->start_in_file > bb->start_in_file) {
    return 1;
  } else {
    return 0;
  }
}

void _openslide_add_jpeg_ops(openslide_t *osr,
			     int32_t file_count,
			     struct _openslide_jpeg_file **files,
			     int32_t level_count,
			     struct _openslide_jpeg_level **levels) {
  /*
  for (int32_t i = 0; i < level_count; i++) {
    struct _openslide_jpeg_level *l = levels[i];
    g_debug("level %d", i);
    g_debug(" size %" G_GINT64_FORMAT " %" G_GINT64_FORMAT, l->level_w, l->level_h);
    g_debug(" tiles %d %d", l->tiles_across, l->tiles_down);
    g_debug(" raw tile size %d %d", l->raw_tile_width, l->raw_tile_height);
    g_debug(" tile advance %g %g", l->tile_advance_x, l->tile_advance_y);
  }

  g_debug("file_count: %d", file_count);
  */

  g_assert(level_count);
  g_assert(file_count);

  if (osr == NULL) {
    // free now and return
    for (int32_t i = 0; i < file_count; i++) {
      g_free(files[i]->filename);
      g_free(files[i]->mcu_starts);
      g_slice_free(struct _openslide_jpeg_file, files[i]);
    }
    g_free(files);

    for (int32_t i = 0; i < level_count; i++) {
      g_hash_table_unref(levels[i]->tiles);
      g_slice_free(struct _openslide_jpeg_level, levels[i]);
    }
    g_free(levels);

    return;
  }

  g_assert(osr->data == NULL);


  // allocate private data
  struct jpegops_data *data = g_slice_new0(struct jpegops_data);
  osr->data = data;


  // convert all struct _openslide_jpeg_file into struct one_jpeg
  data->jpeg_count = file_count;
  data->all_jpegs = g_new0(struct one_jpeg *, file_count);
  for (int32_t i = 0; i < data->jpeg_count; i++) {
    //    g_debug("init JPEG %d", i);
    data->all_jpegs[i] = g_slice_new0(struct one_jpeg);
    init_one_jpeg(data->all_jpegs[i], files[i]);
    g_slice_free(struct _openslide_jpeg_file, files[i]);
  }
  g_free(files);
  files = NULL;

  // convert all struct _openslide_jpeg_level into struct level, and
  //  (internally) convert all struct _openslide_jpeg_tile into struct tile
  GHashTable *expanded_levels = g_hash_table_new_full(_openslide_int64_hash,
						      _openslide_int64_equal,
						      _openslide_int64_free,
						      level_free);
  for (int32_t i = 0; i < level_count; i++) {
    struct _openslide_jpeg_level *old_l = levels[i];

    struct level *new_l = g_slice_new0(struct level);
    new_l->info.downsample = old_l->downsample;
    new_l->info.w = old_l->level_w;
    new_l->info.h = old_l->level_h;
    new_l->tiles_across = old_l->tiles_across;
    new_l->tiles_down = old_l->tiles_down;
    new_l->scale_denom = 1;
    new_l->tile_advance_x = old_l->tile_advance_x;
    new_l->tile_advance_y = old_l->tile_advance_y;
    // initialize tile size hints if potentially valid (may be cleared later)
    if (((int64_t) old_l->tile_advance_x) == old_l->tile_advance_x &&
        ((int64_t) old_l->tile_advance_y) == old_l->tile_advance_y) {
      new_l->info.tile_w = new_l->tile_advance_x;
      new_l->info.tile_h = new_l->tile_advance_y;
    }

    // convert tiles
    new_l->grid = _openslide_grid_tilemap_create(osr,
                                                 new_l->tiles_across,
                                                 new_l->tiles_down,
                                                 new_l->tile_advance_x,
                                                 new_l->tile_advance_y,
                                                 read_tile, tile_free);
    struct convert_tiles_args ct_args = { new_l, data->all_jpegs };
    g_hash_table_foreach(old_l->tiles, convert_tiles, &ct_args);

    //g_debug("level margins %d %d %d %d", new_l->extra_tiles_top, new_l->extra_tiles_left, new_l->extra_tiles_bottom, new_l->extra_tiles_right);

    // now, new_l is all initialized, so add it
    int64_t *key = g_slice_new(int64_t);
    *key = new_l->info.w;
    g_hash_table_insert(expanded_levels, key, new_l);

    // try adding scale_denom levels
    for (int scale_denom = 2; scale_denom <= 8; scale_denom <<= 1) {
      // check to make sure we get an even division
      if ((old_l->raw_tile_width % scale_denom) ||
	  (old_l->raw_tile_height % scale_denom)) {
	continue;
      }

      // create a derived level
      struct level *sd_l = g_slice_new0(struct level);
      sd_l->tiles_across = new_l->tiles_across;
      sd_l->tiles_down = new_l->tiles_down;
      sd_l->info.downsample = new_l->info.downsample * scale_denom;

      sd_l->scale_denom = scale_denom;

      sd_l->info.w = new_l->info.w / scale_denom;
      sd_l->info.h = new_l->info.h / scale_denom;
      sd_l->tile_advance_x = new_l->tile_advance_x / scale_denom;
      sd_l->tile_advance_y = new_l->tile_advance_y / scale_denom;
      if (new_l->info.tile_w && new_l->info.tile_h &&
          ((int64_t) sd_l->tile_advance_x) == sd_l->tile_advance_x &&
          ((int64_t) sd_l->tile_advance_y) == sd_l->tile_advance_y) {
        sd_l->info.tile_w = sd_l->tile_advance_x;
        sd_l->info.tile_h = sd_l->tile_advance_y;
      }

      sd_l->grid = _openslide_grid_tilemap_create(osr,
                                                  sd_l->tiles_across,
                                                  sd_l->tiles_down,
                                                  sd_l->tile_advance_x,
                                                  sd_l->tile_advance_y,
                                                  read_tile, tile_free);
      struct convert_tiles_args ct_args = { sd_l, data->all_jpegs };
      g_hash_table_foreach(old_l->tiles, convert_tiles, &ct_args);

      key = g_slice_new(int64_t);
      *key = sd_l->info.w;
      g_hash_table_insert(expanded_levels, key, sd_l);
    }

    // delete the old level
    g_hash_table_unref(old_l->tiles);
    g_slice_free(struct _openslide_jpeg_level, old_l);
  }
  g_free(levels);
  levels = NULL;


  // sort all_jpegs by file and start position, so we can avoid seeks
  // when background finding mcus
  qsort(data->all_jpegs, file_count, sizeof(struct one_jpeg *), one_jpeg_compare);

  // get sorted keys
  GList *level_keys = g_hash_table_get_keys(expanded_levels);
  level_keys = g_list_sort(level_keys, width_compare);

  //  g_debug("number of keys: %d", g_list_length(level_keys));


  // populate the level_count
  osr->level_count = g_hash_table_size(expanded_levels);

  // load into level array
  g_assert(osr->levels == NULL);
  osr->levels = g_new(struct _openslide_level *, osr->level_count);
  GList *tmp_list = level_keys;

  int i = 0;

  //  g_debug("moving sorted levels");
  while(tmp_list != NULL) {
    // get a key and value
    struct level *l = g_hash_table_lookup(expanded_levels, tmp_list->data);

    // move
    osr->levels[i] = (struct _openslide_level *) l;
    g_hash_table_steal(expanded_levels, tmp_list->data);
    _openslide_int64_free(tmp_list->data);  // key

    // consume the head and continue
    tmp_list = g_list_delete_link(tmp_list, tmp_list);
    i++;
  }
  g_hash_table_unref(expanded_levels);

  // if any level is missing tile size hints, we must invalidate all hints
  for (int32_t i = 0; i < osr->level_count; i++) {
    if (!osr->levels[i]->tile_w || !osr->levels[i]->tile_h) {
      // invalidate
      for (i = 0; i < osr->level_count; i++) {
        osr->levels[i]->tile_w = 0;
        osr->levels[i]->tile_h = 0;
      }
      break;
    }
  }

  // init background thread for finding restart markers
  data->restart_marker_timer = g_timer_new();
  data->restart_marker_mutex = g_mutex_new();
  data->restart_marker_cond = g_cond_new();
  data->restart_marker_cond_mutex = g_mutex_new();
  data->restart_marker_thread = g_thread_create(restart_marker_thread_func,
						osr,
						TRUE,
						NULL);

  // for debugging
  if (false) {
    g_thread_join(data->restart_marker_thread);
    verify_mcu_starts(data);
  }

  // set ops
  osr->ops = &jpeg_ops;
}


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

  g_set_error(&jerr->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
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

GHashTable *_openslide_jpeg_create_tiles_table(void) {
  return g_hash_table_new_full(_openslide_int64_hash, _openslide_int64_equal,
			       _openslide_int64_free,
			       struct_openslide_jpeg_tile_free);
}

static void jpeg_get_associated_image_data(openslide_t *osr, void *_ctx,
                                           uint32_t *_dest,
                                           int64_t w, int64_t h) {
  struct jpeg_associated_image_ctx *ctx = _ctx;
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  FILE *f;
  jmp_buf env;
  GError *tmp_err = NULL;

  // g_debug("read JPEG associated image: %s %" G_GINT64_FORMAT, ctx->filename,
  //         ctx->offset);

  // open file
  f = _openslide_fopen(ctx->filename, "rb", &tmp_err);
  if (f == NULL) {
    _openslide_set_error_from_gerror(osr, tmp_err);
    g_clear_error(&tmp_err);
    return;
  }
  if (ctx->offset && fseeko(f, ctx->offset, SEEK_SET) == -1) {
    _openslide_set_error(osr, "Cannot seek file %s", ctx->filename);
    fclose(f);
    return;
  }

  JSAMPARRAY buffer = (JSAMPARRAY) g_slice_alloc0(sizeof(JSAMPROW) * MAX_SAMP_FACTOR);

  if (setjmp(env) == 0) {
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
    jpeg_create_decompress(&cinfo);

    int header_result;

    // read header
    _openslide_jpeg_stdio_src(&cinfo, f);
    header_result = jpeg_read_header(&cinfo, TRUE);
    if ((header_result != JPEG_HEADER_OK
	 && header_result != JPEG_HEADER_TABLES_ONLY)) {
      _openslide_set_error(osr, "Cannot read associated image header");
      goto DONE;
    }

    cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    // ensure dimensions have not changed
    int32_t width = cinfo.output_width;
    int32_t height = cinfo.output_height;
    if (w != width || h != height) {
      _openslide_set_error(osr, "Unexpected associated image size");
      goto DONE;
    }

    // allocate scanline buffers
    for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
      buffer[i] = (JSAMPROW) g_malloc(sizeof(JSAMPLE)
				      * cinfo.output_width
				      * cinfo.output_components);
    }

    // decompress
    uint32_t *dest = _dest;
    while (cinfo.output_scanline < cinfo.output_height) {
      JDIMENSION rows_read = jpeg_read_scanlines(&cinfo,
						 buffer,
						 cinfo.rec_outbuf_height);
      int cur_buffer = 0;
      while (rows_read > 0) {
	// copy a row
	int32_t i;
	for (i = 0; i < (int32_t) cinfo.output_width; i++) {
	  dest[i] = 0xFF000000 |                  // A
	    buffer[cur_buffer][i * 3 + 0] << 16 | // R
	    buffer[cur_buffer][i * 3 + 1] << 8 |  // G
	    buffer[cur_buffer][i * 3 + 2];        // B
	}

	// advance everything 1 row
	rows_read--;
	cur_buffer++;
	dest += cinfo.output_width;
      }
    }
  } else {
    // setjmp has returned again
    _openslide_set_error(osr, "Cannot read associated image: %s",
                         jerr.err->message);
    g_clear_error(&jerr.err);
  }

DONE:
  // free buffers
  for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
    g_free(buffer[i]);
  }

  g_slice_free1(sizeof(JSAMPROW) * MAX_SAMP_FACTOR, buffer);

  jpeg_destroy_decompress(&cinfo);

  fclose(f);
}

static void jpeg_destroy_associated_image_ctx(void *_ctx) {
  struct jpeg_associated_image_ctx *ctx = _ctx;

  g_free(ctx->filename);
  g_slice_free(struct jpeg_associated_image_ctx, ctx);
}

bool _openslide_add_jpeg_associated_image(GHashTable *ht,
					  const char *name,
					  const char *filename,
					  int64_t offset,
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
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Couldn't read JPEG header");
      goto DONE;
    }

    jpeg_calc_output_dimensions(&cinfo);

    if (ht) {
      struct jpeg_associated_image_ctx *ctx =
        g_slice_new(struct jpeg_associated_image_ctx);
      ctx->filename = g_strdup(filename);
      ctx->offset = offset;

      struct _openslide_associated_image *aimg =
	g_slice_new(struct _openslide_associated_image);
      aimg->w = cinfo.output_width;
      aimg->h = cinfo.output_height;
      aimg->ctx = ctx;
      aimg->get_argb_data = jpeg_get_associated_image_data;
      aimg->destroy_ctx = jpeg_destroy_associated_image_ctx;

      g_hash_table_insert(ht, g_strdup(name), aimg);
    }

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
