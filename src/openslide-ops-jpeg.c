/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

/*
 * Part of this file is:
 *
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * This file is part of the Independent JPEG Group's software.
 * For conditions of distribution and use, see the accompanying README file.
 */

#include "config.h"

#include <glib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <jpeglib.h>
#include <jerror.h>
#include <inttypes.h>
#include <math.h>

#include <cairo.h>

#include <sys/types.h>   // for off_t ?

#include "openslide-private.h"
#include "openslide-cache.h"
#include "openslide-tilehelper.h"

enum restart_marker_thread_state {
  R_M_THREAD_STATE_RUN,
  R_M_THREAD_STATE_PAUSE,
  R_M_THREAD_STATE_STOP,
};

struct one_jpeg {
  FILE *f;
  int64_t start_in_file;
  int64_t end_in_file;

  int32_t tile_width;
  int32_t tile_height;

  int32_t width;
  int32_t height;

  int32_t mcu_starts_count;
  int64_t *mcu_starts;
  int64_t *unreliable_mcu_starts;
};

struct tile {
  struct one_jpeg *jpeg;
  int32_t jpegno;   // used only for cache lookup
  int32_t tileno;

  // bounds in the physical tile?
  float src_x;
  float src_y;
  float w;
  float h;

  // delta from the "natural" position
  float dest_offset_x;
  float dest_offset_y;
};

struct layer {
  GHashTable *tiles;

  int32_t tiles_across;
  int32_t tiles_down;

  int32_t scale_denom;

  // how much extra we might need to read to get all relevant tiles?
  // computed from dest offsets
  int32_t extra_tiles_top;
  int32_t extra_tiles_bottom;
  int32_t extra_tiles_left;
  int32_t extra_tiles_right;


  // note: everything below is pre-divided by scale_denom

  // total size
  int64_t pixel_w;
  int64_t pixel_h;

  double tile_advance_x;
  double tile_advance_y;
};

struct jpegops_data {
  int32_t jpeg_count;
  struct one_jpeg **all_jpegs;

  // layer_count is in the osr struct
  struct layer *layers;

  // cache
  struct _openslide_cache *cache;

  // thread stuff, for background search of restart markers
  GTimer *restart_marker_timer;
  GMutex *restart_marker_mutex;
  GThread *restart_marker_thread;

  GCond *restart_marker_cond;
  GMutex *restart_marker_cond_mutex;
  enum restart_marker_thread_state restart_marker_thread_state;
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

static void init_source (j_decompress_ptr _OPENSLIDE_UNUSED(cinfo)) {
  /* nothing to be done */
}

static boolean fill_input_buffer (j_decompress_ptr cinfo) {
  /* this should never be called, there is nothing to fill */
  ERREXIT(cinfo, JERR_INPUT_EMPTY);

  return TRUE;
}


static void skip_input_data (j_decompress_ptr cinfo, long num_bytes) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;

  src->pub.next_input_byte += (size_t) num_bytes;
  src->pub.bytes_in_buffer -= (size_t) num_bytes;
}


static void term_source (j_decompress_ptr _OPENSLIDE_UNUSED(cinfo)) {
  /* nothing to do */
}

static void jpeg_random_access_src (j_decompress_ptr cinfo, FILE *infile,
				    int64_t header_start_position,
				    int64_t header_stop_position,
				    int64_t start_position, int64_t stop_position) {
  struct my_src_mgr *src;

  if (cinfo->src == NULL) {     /* first time for this JPEG object? */
    cinfo->src = (struct jpeg_source_mgr *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  sizeof(struct my_src_mgr));
  }

  src = (struct my_src_mgr *) cinfo->src;
  src->pub.init_source = init_source;
  src->pub.fill_input_buffer = fill_input_buffer;
  src->pub.skip_input_data = skip_input_data;
  src->pub.resync_to_restart = jpeg_resync_to_restart; /* use default method */
  src->pub.term_source = term_source;

  // check for problems
  if ((header_start_position == -1) || (header_stop_position == -1) ||
      (start_position == -1) || (stop_position == -1) ||
      (header_start_position >= header_stop_position) ||
      (header_stop_position > start_position) ||
      (start_position >= stop_position)) {
    g_critical("Can't do random access JPEG read: "
	       "header_start_position: %" PRId64 ", "
	       "header_stop_position: %" PRId64 ", "
	       "start_position: %" PRId64 ", "
	       "stop_position: %" PRId64,
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
  //  g_debug("reading header from %" PRId64, header_start_position);
  fseeko(infile, header_start_position, SEEK_SET);
  fread(src->buffer, header_length, 1, infile);
  //  g_debug("reading from %" PRId64, start_position);
  fseeko(infile, start_position, SEEK_SET);
  fread(src->buffer + header_length, data_length, 1, infile);

  // change the final byte to EOI
  g_return_if_fail(src->buffer[src->buffer_size - 2] == 0xFF);
  src->buffer[src->buffer_size - 1] = JPEG_EOI;
}

static void filehandle_free(gpointer data) {
  //g_debug("fclose(%p)", data);
  fclose(data);
}

static GHashTable *filehandle_hashtable_new(void) {
  return g_hash_table_new_full(g_direct_hash,
			       g_direct_equal,
			       filehandle_free,
			       NULL);
}

static void filehandle_hashtable_conditional_insert(GHashTable *h,
						    FILE *f) {
  if (f && !g_hash_table_lookup_extended(h, f, NULL, NULL)) {
    g_hash_table_insert(h, f, NULL);
  }
}

static guint int64_hash(gconstpointer v) {
  int64_t i = *((const int64_t *) v);
  return i ^ (i >> 32);
}

static gboolean int64_equal(gconstpointer v1, gconstpointer v2) {
  return *((int64_t *) v1) == *((int64_t *) v2);
}

static void int64_free(gpointer data) {
  g_slice_free(int64_t, data);
}

static void layer_free(gpointer data) {
  //g_debug("layer_free: %p", data);

  struct layer *l = data;

  //  g_debug("g_free(%p)", (void *) l->layer_jpegs);
  g_hash_table_unref(l->tiles);
  g_slice_free(struct layer, l);
}

static void tile_free(gpointer data) {
  g_slice_free(struct tile, data);
}

static void struct_openslide_jpeg_tile_free(gpointer data) {
  g_slice_free(struct _openslide_jpeg_tile, data);
}

struct convert_tiles_args {
  struct layer *new_l;
  struct one_jpeg **all_jpegs;
};

static void convert_tiles(gpointer key,
			  gpointer value,
			  gpointer user_data) {
  struct convert_tiles_args *args = user_data;
  struct _openslide_jpeg_tile *old_tile = value;
  struct layer *new_l = args->new_l;

  // create new tile
  struct tile *new_tile = g_slice_new(struct tile);
  new_tile->jpeg = args->all_jpegs[old_tile->fileno];
  new_tile->jpegno = old_tile->fileno;
  new_tile->tileno = old_tile->tileno;
  new_tile->src_x = old_tile->src_x;
  new_tile->src_y = old_tile->src_y;
  new_tile->w = old_tile->w;
  new_tile->h = old_tile->h;
  new_tile->dest_offset_x = old_tile->dest_offset_x;
  new_tile->dest_offset_y = old_tile->dest_offset_y;

  // margin stuff
  float dsx = new_tile->dest_offset_x;
  float dsy = new_tile->dest_offset_y;
  if (dsx > 0) {
    // extra on left
    int extra_left = ceil(dsx / new_l->tile_advance_x);
    new_l->extra_tiles_left = MAX(new_l->extra_tiles_left,
				  extra_left);
  } else {
    // extra on right
    int extra_right = ceil(-dsx / new_l->tile_advance_x);
    new_l->extra_tiles_right = MAX(new_l->extra_tiles_right,
				   extra_right);
  }
  if (dsy > 0) {
    // extra on top
    int extra_top = ceil(dsy / new_l->tile_advance_y);
    new_l->extra_tiles_top = MAX(new_l->extra_tiles_top,
				 extra_top);
  } else {
    // extra on bottom
    int extra_bottom = ceil(-dsy / new_l->tile_advance_y);
    new_l->extra_tiles_bottom = MAX(new_l->extra_tiles_bottom,
				    extra_bottom);
  }



  // insert tile into new table
  int64_t *newkey = g_slice_new(int64_t);
  *newkey = *((int64_t *) key);
  g_hash_table_insert(new_l->tiles, newkey, new_tile);
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
  boolean last_was_ff = false;
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

static void compute_mcu_start(FILE *f,
			      int64_t *mcu_starts,
			      int64_t *unreliable_mcu_starts,
			      int64_t start_in_file,
			      int64_t end_in_file,
			      int64_t target) {
  // special case for first
  if (mcu_starts[0] == -1) {
    struct jpeg_decompress_struct cinfo;
    struct _openslide_jpeg_error_mgr jerr;
    jmp_buf env;

    // init jpeg
    fseeko(f, start_in_file, SEEK_SET);

    if (setjmp(env) == 0) {
      cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
      jpeg_create_decompress(&cinfo);
      jpeg_stdio_src(&cinfo, f);
      jpeg_read_header(&cinfo, TRUE);
      jpeg_start_decompress(&cinfo);
    } else {
      // setjmp returns again
      g_critical("Error initializing JPEG");
      // TODO _openslide_convert_to_error_ops
    }

    // set the first entry
    mcu_starts[0] = ftello(f) - cinfo.src->bytes_in_buffer;

    // done
    jpeg_destroy_decompress(&cinfo);
  }

  // check if already done
  if (mcu_starts[target] != -1) {
    return;
  }

  // check the unreliable_mcu_starts store first,
  // and use it if valid
  int64_t offset = -1;
  if (unreliable_mcu_starts != NULL) {
    offset = unreliable_mcu_starts[target];
  }

  if (offset != -1) {
    uint8_t buf[2];
    fseeko(f, offset - 2, SEEK_SET);

    size_t result = fread(buf, 2, 1, f);
    if (result == 0 ||
	buf[0] != 0xFF || buf[1] < 0xD0 || buf[1] > 0xD7) {
      g_warning("Restart marker not found in expected place");
    } else {
      mcu_starts[target] = offset;
      return;
    }
  }


  // otherwise, walk backwards, to find the first non -1 offset
  int64_t first_good = target - 1;
  while (mcu_starts[first_good] == -1) {
    first_good--;
  }
  //  g_debug("target: %d, first_good: %d", target, first_good);

  // now search for the new restart markers
  fseeko(f, mcu_starts[first_good], SEEK_SET);

  uint8_t buf_start[4096];
  uint8_t *buf = buf_start;
  int bytes_in_buf = 0;
  while (first_good < target) {
    int64_t after_marker_pos;
    uint8_t b = find_next_ff_marker(f, buf_start, &buf, 4096,
				    end_in_file,
				    &after_marker_pos,
				    &bytes_in_buf);
    g_assert(after_marker_pos > 0 || after_marker_pos == -1);
    if (after_marker_pos == -1) {
      g_critical("after_marker_pos == -1");
      break;
    }
    //g_debug("after_marker_pos: %" PRId64, after_marker_pos);

    // EOI?
    if (b == JPEG_EOI) {
      // we're done
      break;
    } else if (b >= 0xD0 && b < 0xD8) {
      // marker
      mcu_starts[1 + first_good++] = after_marker_pos;
    }
  }
}


static uint32_t *read_from_one_jpeg (struct one_jpeg *jpeg,
				     int32_t tileno,
				     int32_t scale_denom) {
  g_assert(jpeg->f);

  // begin decompress
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  jmp_buf env;

  gsize row_size = 0;

  JSAMPARRAY buffer = g_slice_alloc0(sizeof(JSAMPROW) * MAX_SAMP_FACTOR);

  uint32_t *dest = NULL;

  if (setjmp(env) == 0) {
    // figure out where to start the data stream
    int64_t stop_position;
    compute_mcu_start(jpeg->f,
		      jpeg->mcu_starts,
		      jpeg->unreliable_mcu_starts,
		      jpeg->start_in_file,
		      jpeg->end_in_file,
		      tileno);
    if (jpeg->mcu_starts_count == tileno + 1) {
      // EOF
      stop_position = jpeg->end_in_file;
    } else {
      compute_mcu_start(jpeg->f,
			jpeg->mcu_starts,
			jpeg->unreliable_mcu_starts,
			jpeg->start_in_file,
			jpeg->end_in_file,
			tileno + 1);
      stop_position = jpeg->mcu_starts[tileno + 1];
    }

    // set error handler, this will longjmp if necessary
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);

    // start decompressing
    jpeg_create_decompress(&cinfo);

    jpeg_random_access_src(&cinfo, jpeg->f,
			   jpeg->start_in_file,
			   jpeg->mcu_starts[0],
			   jpeg->mcu_starts[tileno],
			   stop_position);

    jpeg_read_header(&cinfo, TRUE);
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
      buffer[i] = g_slice_alloc(row_size);
      //g_debug("buffer[%d]: %p", i, buffer[i]);
    }

    int dest_size = cinfo.output_width * cinfo.output_height * 4;
    dest = g_slice_alloc(dest_size);

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
	for (int32_t i = 0; i < (int32_t) cinfo.output_width; i++) {
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
  } else {
    // setjmp returns again
    g_critical("JPEG decompression failed");
    // TODO _openslide_convert_to_error_ops
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

  return dest;
}

static void read_tile(openslide_t *osr,
		      cairo_t *cr,
		      int32_t layer,
		      int64_t tile_x, int64_t tile_y,
		      struct _openslide_cache *cache) {
  //  g_debug("jpeg read_tile: %d, %" PRId64 " %" PRId64, layer, tile_x, tile_y);

  struct jpegops_data *data = osr->data;
  struct layer *l = data->layers + layer;

  if ((tile_x >= l->tiles_across) || (tile_y >= l->tiles_down)) {
    return;
  }

  int64_t tileindex = tile_y * l->tiles_across + tile_x;
  struct tile *tile = g_hash_table_lookup(l->tiles, &tileindex);

  bool cachemiss;

  if (!tile) {
    //    g_debug("no tile at index %" PRId64, tileindex);
    return;
  }


  // get the jpeg data, possibly from cache
  uint32_t *tiledata = _openslide_cache_get(cache,
					    tile->jpegno,
					    tile->tileno,
					    layer);
  cachemiss = !tiledata;
  if (!tiledata) {
    tiledata = read_from_one_jpeg(tile->jpeg,
				  tile->tileno,
				  l->scale_denom);
  }

  // draw it
  int tw = tile->jpeg->tile_width / l->scale_denom;
  int th = tile->jpeg->tile_height / l->scale_denom;
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
								 CAIRO_FORMAT_RGB24,
								 tw, th,
								 tw * 4);

  double src_x = tile->src_x / l->scale_denom;
  double src_y = tile->src_y / l->scale_denom;

  // if we are drawing a subregion of the tile, we must do an additional copy,
  // because cairo lacks source clipping
  if ((tile->jpeg->tile_width > tile->w) ||
      (tile->jpeg->tile_height > tile->h)) {
    cairo_surface_t *surface2 = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
							   ceil(tile->w / l->scale_denom),
							   ceil(tile->h / l->scale_denom));
    cairo_t *cr2 = cairo_create(surface2);
    cairo_set_source_surface(cr2, surface, -src_x, -src_y);
    cairo_surface_destroy(surface);
    surface = surface2;
    src_x = 0;
    src_y = 0;

    cairo_rectangle(cr2, 0, 0,
		    tile->w / l->scale_denom,
		    tile->h / l->scale_denom);
    cairo_fill(cr2);
    cairo_destroy(cr2);
  }

  cairo_save(cr);
  cairo_translate(cr,
		  tile->dest_offset_x / l->scale_denom,
		  tile->dest_offset_y / l->scale_denom);
  cairo_set_source_surface(cr, surface,
			   -src_x, -src_y);
  cairo_surface_destroy(surface);
  cairo_rectangle(cr, 0, 0,
		  tile->w / l->scale_denom, tile->h / l->scale_denom);
  cairo_fill(cr);
  cairo_restore(cr);

  // put into cache last, because the cache can free this tile
  if (cachemiss) {
    _openslide_cache_put(cache, tile->jpegno, tile->tileno, layer,
			 tiledata,
			 tw * th * 4);
  }
}


static void paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 int32_t layer,
			 int32_t w, int32_t h) {
  struct jpegops_data *data = osr->data;
  struct layer *l = data->layers + layer;

  // tell the background thread to pause
  g_mutex_lock(data->restart_marker_cond_mutex);
  data->restart_marker_thread_state = R_M_THREAD_STATE_PAUSE;
  //  g_debug("telling thread to pause");
  g_mutex_unlock(data->restart_marker_cond_mutex);

  // wait until thread is paused
  g_mutex_lock(data->restart_marker_mutex);

  // compute coordinates
  double ds = openslide_get_layer_downsample(osr, layer);
  int64_t ds_x = x / ds;
  int64_t ds_y = y / ds;
  int64_t start_tile_x = (ds_x / l->tile_advance_x) - l->extra_tiles_left;
  double offset_x = (ds_x - (start_tile_x * l->tile_advance_x))
    + (l->extra_tiles_left * l->tile_advance_x);
  int64_t end_tile_x = ((ds_x + w) / l->tile_advance_x) + 1 + l->extra_tiles_right;
  int64_t start_tile_y = (ds_y / l->tile_advance_y) - l->extra_tiles_top;
  double offset_y = (ds_y - (start_tile_y * l->tile_advance_y))
    + (l->extra_tiles_top * l->tile_advance_y);
  int64_t end_tile_y = ((ds_y + h) / l->tile_advance_y) + 1 + l->extra_tiles_bottom;

  _openslide_read_tiles(cr,
			layer,
			start_tile_x, start_tile_y,
			end_tile_x, end_tile_y,
			offset_x, offset_y,
			l->tile_advance_x,
			l->tile_advance_y,
			osr, osr->cache,
			read_tile);

  // unlock
  g_mutex_unlock(data->restart_marker_mutex);

  // tell the background thread to resume
  g_mutex_lock(data->restart_marker_cond_mutex);
  g_timer_start(data->restart_marker_timer);
  data->restart_marker_thread_state = R_M_THREAD_STATE_RUN;
  //  g_debug("telling thread to awaken");
  g_cond_signal(data->restart_marker_cond);
  g_mutex_unlock(data->restart_marker_cond_mutex);
}

static void destroy(openslide_t *osr) {
  struct jpegops_data *data = osr->data;

  // tell the thread to finish and wait
  g_mutex_lock(data->restart_marker_cond_mutex);
  data->restart_marker_thread_state = R_M_THREAD_STATE_STOP;
  g_cond_signal(data->restart_marker_cond);
  g_mutex_unlock(data->restart_marker_cond_mutex);
  g_thread_join(data->restart_marker_thread);

  // each jpeg in turn, don't close a file handle more than once
  GHashTable *fclose_hashtable = filehandle_hashtable_new();
  for (int32_t i = 0; i < data->jpeg_count; i++) {
    struct one_jpeg *jpeg = data->all_jpegs[i];
    filehandle_hashtable_conditional_insert(fclose_hashtable, jpeg->f);
    g_free(jpeg->mcu_starts);
    g_free(jpeg->unreliable_mcu_starts);
    g_slice_free(struct one_jpeg, jpeg);
  }
  g_hash_table_unref(fclose_hashtable);

  // each layer in turn
  for (int32_t i = 0; i < osr->layer_count; i++) {
    struct layer *l = data->layers + i;

    //    g_debug("g_free(%p)", (void *) l->layer_jpegs);
    g_hash_table_unref(l->tiles);
  }

  // the JPEG array
  g_free(data->all_jpegs);

  // the layer array
  g_free(data->layers);

  // the cache
  _openslide_cache_destroy(data->cache);

  // the background stuff
  g_mutex_free(data->restart_marker_mutex);
  g_timer_destroy(data->restart_marker_timer);
  g_cond_free(data->restart_marker_cond);
  g_mutex_free(data->restart_marker_cond_mutex);

  // the structure
  g_slice_free(struct jpegops_data, data);
}

static void get_dimensions(openslide_t *osr,
			   int32_t layer,
			   int64_t *w, int64_t *h) {
  struct jpegops_data *data = osr->data;
  struct layer *l = data->layers + layer;

  *w = l->pixel_w;
  *h = l->pixel_h;
}

static const struct _openslide_ops jpeg_ops = {
  .get_dimensions = get_dimensions,
  .paint_region = paint_region,
  .destroy = destroy
};


static void init_one_jpeg(struct one_jpeg *onej,
			  struct _openslide_jpeg_file *file) {
  onej->f = file->f;
  onej->start_in_file = file->start_in_file;
  onej->end_in_file = file->end_in_file;
  onej->unreliable_mcu_starts = file->mcu_starts;

  g_assert(file->w && file->h && file->tw && file->th);

  onej->width = file->w;
  onej->height = file->h;
  onej->tile_width = file->tw;
  onej->tile_height = file->th;

  // compute the mcu starts stuff
  onej->mcu_starts_count =
    (onej->width / onej->tile_width) *
    (onej->height / onej->tile_height);

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

static void get_keys(gpointer key,
		     gpointer _OPENSLIDE_UNUSED(value),
		     gpointer user_data) {
  GList *keys = *((GList **) user_data);
  keys = g_list_prepend(keys, key);
  *((GList **) user_data) = keys;
}

static void verify_mcu_starts(struct jpegops_data *data) {
  g_debug("verifying mcu starts");

  int32_t current_jpeg = 0;
  int32_t current_mcu_start = 0;

  while(current_jpeg < data->jpeg_count) {
    struct one_jpeg *oj = data->all_jpegs[current_jpeg];
    if (!oj->f) {
      current_jpeg++;
      continue;
    }

    if (current_mcu_start > 0) {
      int64_t offset = oj->mcu_starts[current_mcu_start];
      g_assert(offset != -1);
      fseeko(oj->f, offset - 2, SEEK_SET);
      g_assert(getc(oj->f) == 0xFF);
      int marker = getc(oj->f);
      g_assert(marker >= 0xD0 && marker <= 0xD7);
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
  struct jpegops_data *data = d;

  int32_t current_jpeg = 0;
  int32_t current_mcu_start = 0;

  while(current_jpeg < data->jpeg_count) {
    g_mutex_lock(data->restart_marker_cond_mutex);

    // should we pause?
    while(data->restart_marker_thread_state == R_M_THREAD_STATE_PAUSE) {
      //      g_debug("thread paused");
      g_cond_wait(data->restart_marker_cond,
		  data->restart_marker_cond_mutex); // zzz
      //      g_debug("thread awoken");
    }

    // should we stop?
    if (data->restart_marker_thread_state == R_M_THREAD_STATE_STOP) {
      //      g_debug("thread stopping");
      g_mutex_unlock(data->restart_marker_cond_mutex);
      break;
    }

    g_assert(data->restart_marker_thread_state == R_M_THREAD_STATE_RUN);

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

    if (!g_mutex_trylock(data->restart_marker_mutex)) {
      // just kidding, still not ready, go back and sleep
      continue;
    }

    // locked


    //g_debug("current_jpeg: %d, current_mcu_start: %d",
    //        current_jpeg, current_mcu_start);

    struct one_jpeg *oj = data->all_jpegs[current_jpeg];
    if (oj->f) {
      compute_mcu_start(oj->f, oj->mcu_starts,
			oj->unreliable_mcu_starts,
			oj->start_in_file,
			oj->end_in_file,
			current_mcu_start);

      current_mcu_start++;
      if (current_mcu_start >= oj->mcu_starts_count) {
	current_mcu_start = 0;
	current_jpeg++;
      }
    } else {
      current_jpeg++;
    }

    g_mutex_unlock(data->restart_marker_mutex);
  }

  //  g_debug("restart_marker_thread_func done!");
  return NULL;
}

static int one_jpeg_compare(const void *a, const void *b) {
  const struct one_jpeg *aa = *(struct one_jpeg * const *) a;
  const struct one_jpeg *bb = *(struct one_jpeg * const *) b;

  // compare files
  if (aa->f < bb->f) {
    return -1;
  } else if (aa->f > bb->f) {
    return 1;
  }

  // compare offsets
  if (aa->f && bb->f) {
    if (aa->start_in_file < bb->start_in_file) {
      return -1;
    } else if (aa->start_in_file > bb->start_in_file) {
      return 1;
    } else {
      return 0;
    }
  } else {
    return 0;
  }
}

void _openslide_add_jpeg_ops(openslide_t *osr,
			     int32_t file_count,
			     struct _openslide_jpeg_file **files,
			     int32_t layer_count,
			     struct _openslide_jpeg_layer **layers) {
  /*
  for (int32_t i = 0; i < layer_count; i++) {
    struct _openslide_jpeg_layer *l = layers[i];
    g_debug("layer %d", i);
    g_debug(" size %" PRId64 " %" PRId64, l->layer_w, l->layer_h);
    g_debug(" tiles %d %d", l->tiles_across, l->tiles_down);
    g_debug(" raw tile size %d %d", l->raw_tile_width, l->raw_tile_height);
    g_debug(" tile advance %g %g", l->tile_advance_x, l->tile_advance_y);
  }

  g_debug("file_count: %d", file_count);
  */

  g_assert(layer_count);
  g_assert(file_count);

  if (osr == NULL) {
    // free now and return
    GHashTable *fclose_hashtable = filehandle_hashtable_new();
    for (int32_t i = 0; i < file_count; i++) {
      if (files[i]->f) {
	filehandle_hashtable_conditional_insert(fclose_hashtable,
						files[i]->f);
	g_free(files[i]->mcu_starts);
      }
      g_slice_free(struct _openslide_jpeg_file, files[i]);
    }
    g_hash_table_unref(fclose_hashtable);
    g_free(files);

    for (int32_t i = 0; i < layer_count; i++) {
      g_hash_table_unref(layers[i]->tiles);
      g_slice_free(struct _openslide_jpeg_layer, layers[i]);
    }
    g_free(layers);

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

  // convert all struct _openslide_jpeg_layer into struct layer, and
  //  (internally) convert all struct _openslide_jpeg_tile into struct tile
  GHashTable *expanded_layers = g_hash_table_new_full(int64_hash, int64_equal,
						      int64_free, layer_free);
  for (int32_t i = 0; i < layer_count; i++) {
    struct _openslide_jpeg_layer *old_l = layers[i];

    struct layer *new_l = g_slice_new0(struct layer);
    new_l->tiles_across = old_l->tiles_across;
    new_l->tiles_down = old_l->tiles_down;
    new_l->scale_denom = 1;
    new_l->pixel_w = old_l->layer_w;
    new_l->pixel_h = old_l->layer_h;
    new_l->tile_advance_x = old_l->tile_advance_x;
    new_l->tile_advance_y = old_l->tile_advance_y;

    // convert tiles
    new_l->tiles = g_hash_table_new_full(int64_hash, int64_equal,
					 int64_free, tile_free);
    struct convert_tiles_args ct_args = { new_l, data->all_jpegs };
    g_hash_table_foreach(old_l->tiles, convert_tiles, &ct_args);

    //g_debug("layer margins %d %d %d %d", new_l->extra_tiles_top, new_l->extra_tiles_left, new_l->extra_tiles_bottom, new_l->extra_tiles_right);

    // now, new_l is all initialized, so add it
    int64_t *key = g_slice_new(int64_t);
    *key = new_l->pixel_w;
    g_hash_table_insert(expanded_layers, key, new_l);

    // try adding scale_denom layers
    for (int scale_denom = 2; scale_denom <= 8; scale_denom <<= 1) {
      // check to make sure we get an even division
      if ((old_l->raw_tile_width % scale_denom) ||
	  (old_l->raw_tile_height % scale_denom)) {
	//g_debug("scale_denom: %d");
	continue;
      }

      // create a derived layer
      struct layer *sd_l = g_slice_new0(struct layer);
      sd_l->tiles = g_hash_table_ref(new_l->tiles);
      sd_l->tiles_across = new_l->tiles_across;
      sd_l->tiles_down = new_l->tiles_down;
      sd_l->extra_tiles_top = new_l->extra_tiles_top;
      sd_l->extra_tiles_bottom = new_l->extra_tiles_bottom;
      sd_l->extra_tiles_left = new_l->extra_tiles_left;
      sd_l->extra_tiles_right = new_l->extra_tiles_right;

      sd_l->scale_denom = scale_denom;

      sd_l->pixel_w = new_l->pixel_w / scale_denom;
      sd_l->pixel_h = new_l->pixel_h / scale_denom;
      sd_l->tile_advance_x = new_l->tile_advance_x / scale_denom;
      sd_l->tile_advance_y = new_l->tile_advance_y / scale_denom;

      key = g_slice_new(int64_t);
      *key = sd_l->pixel_w;
      g_hash_table_insert(expanded_layers, key, sd_l);
    }

    // delete the old layer
    g_hash_table_unref(old_l->tiles);
    g_slice_free(struct _openslide_jpeg_layer, old_l);
  }
  g_free(layers);
  layers = NULL;


  // sort all_jpegs by file and start position, so we can avoid seeks
  // when background finding mcus
  qsort(data->all_jpegs, file_count, sizeof(struct one_jpeg *), one_jpeg_compare);

  // get sorted keys
  GList *layer_keys = NULL;
  g_hash_table_foreach(expanded_layers, get_keys, &layer_keys);
  layer_keys = g_list_sort(layer_keys, width_compare);

  //  g_debug("number of keys: %d", g_list_length(layer_keys));


  // populate the layer_count
  osr->layer_count = g_hash_table_size(expanded_layers);

  // load into data array
  data->layers = g_new(struct layer, g_hash_table_size(expanded_layers));
  GList *tmp_list = layer_keys;

  int i = 0;

  //  g_debug("copying sorted layers");
  while(tmp_list != NULL) {
    // get a key and value
    struct layer *l = g_hash_table_lookup(expanded_layers, tmp_list->data);

    // copy
    struct layer *dest = data->layers + i;
    *dest = *l;    // shallow copy

    // manually free some things, because of that shallow copy
    g_hash_table_steal(expanded_layers, tmp_list->data);
    int64_free(tmp_list->data);  // key
    g_slice_free(struct layer, l); // shallow deletion of layer

    // consume the head and continue
    tmp_list = g_list_delete_link(tmp_list, tmp_list);
    i++;
  }
  g_hash_table_unref(expanded_layers);


  // init cache
  data->cache = _openslide_cache_create(_OPENSLIDE_USEFUL_CACHE_SIZE);

  // init background thread for finding restart markers
  data->restart_marker_thread_state = R_M_THREAD_STATE_RUN;
  data->restart_marker_timer = g_timer_new();
  data->restart_marker_mutex = g_mutex_new();
  data->restart_marker_cond = g_cond_new();
  data->restart_marker_cond_mutex = g_mutex_new();
  data->restart_marker_thread = g_thread_create(restart_marker_thread_func,
						data,
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
  struct _openslide_jpeg_error_mgr *err =
    (struct _openslide_jpeg_error_mgr *) cinfo->err;

  (err->pub.output_message) (cinfo);

  //  g_debug("JUMP");
  longjmp(*(err->env), 1);
}

static void my_output_message(j_common_ptr cinfo) {
  char buffer[JMSG_LENGTH_MAX];

  (*cinfo->err->format_message) (cinfo, buffer);

  g_warning("%s", buffer);
}

struct jpeg_error_mgr *_openslide_jpeg_set_error_handler(struct _openslide_jpeg_error_mgr *err,
							 jmp_buf *env) {
  jpeg_std_error(&(err->pub));
  err->pub.error_exit = my_error_exit;
  err->pub.output_message = my_output_message;
  err->env = env;

  return (struct jpeg_error_mgr *) err;
}

GHashTable *_openslide_jpeg_create_tiles_table(void) {
  return g_hash_table_new_full(int64_hash, int64_equal,
			       int64_free, struct_openslide_jpeg_tile_free);
}
