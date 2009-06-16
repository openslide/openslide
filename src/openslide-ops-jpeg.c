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

  int32_t tile_width;
  int32_t tile_height;

  int32_t width;
  int32_t height;

  // all rest needed only if f != NULL
  int64_t start_in_file;
  int64_t end_in_file;
  int32_t mcu_starts_count;
  int64_t *mcu_starts;
  int64_t *unreliable_mcu_starts;
};

struct layer {
  GHashTable *layer_jpegs; // count given by jpeg_w * jpeg_h

  int32_t jpegs_across;       // how many distinct jpeg files across?
  int32_t jpegs_down;         // how many distinct jpeg files down?

  int32_t scale_denom;
  double no_scale_denom_downsample;  // layer0_w div non_premult_pixel_w

  int32_t tiles_across_per_file;
  int32_t tiles_down_per_file;

  // note: everything below is pre-divided by scale_denom

  // total size
  int64_t pixel_w;
  int64_t pixel_h;

  int32_t tile_width;
  int32_t tile_height;

  double overlap_spacing_x;
  double overlap_spacing_y;

  double overlap_x;
  double overlap_y;
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

static void init_source (j_decompress_ptr cinfo) {
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


static void term_source (j_decompress_ptr cinfo) {
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

static bool is_zxy_successor(int64_t pz, int64_t px, int64_t py,
			     int64_t z, int64_t x, int64_t y) {
  //  g_debug("p_zxy: (%" PRId64 ",%" PRId64 ",%" PRId64 "), zxy: (%"
  //	  PRId64 ",%" PRId64 ",%" PRId64 ")",
  //	  pz, px, py, z, x, y);
  if (z == pz + 1) {
    return x == 0 && y == 0;
  }
  if (z != pz) {
    return false;
  }

  // z == pz

  if (y == py + 1) {
    return x == 0;
  }
  if (y != py) {
    return false;
  }

  // y == py

  return x == px + 1;
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

static void int_free(gpointer data) {
  g_slice_free(int, data);
}

static void layer_free(gpointer data) {
  //  g_debug("layer_free: %p", data);

  struct layer *l = data;

  //  g_debug("g_free(%p)", (void *) l->layer_jpegs);
  g_hash_table_unref(l->layer_jpegs);
  g_slice_free(struct layer, l);
}

static void generate_layer_into_map(GSList *jpegs,
				    int32_t jpegs_across, int32_t jpegs_down,
				    int64_t pixel_w, int64_t pixel_h,
				    int32_t image00_w, int32_t image00_h,
				    int32_t tile_width, int32_t tile_height,
				    int64_t layer0_w,
				    GHashTable *width_to_layer_map,
				    double downsample_override) {
  // JPEG files can give us 1/1, 1/2, 1/4, 1/8 downsamples, so we
  // need to create 4 layers per set of JPEGs

  int32_t num_jpegs = jpegs_across * jpegs_down;

  for (int scale_denom = 1; scale_denom <= 8; scale_denom <<= 1) {
    // check to make sure we get an even division
    if ((pixel_w % scale_denom) || (pixel_h % scale_denom)) {
      //g_debug("scale_denom: %d");
      continue;
    }

    // create layer
    struct layer *l = g_slice_new0(struct layer);
    l->scale_denom = scale_denom;
    if (downsample_override) {
      l->no_scale_denom_downsample = downsample_override;
    } else {
      l->no_scale_denom_downsample = (double) layer0_w / (double) pixel_w;
    }

    l->tiles_across_per_file = image00_w / tile_width;
    l->tiles_down_per_file = image00_h / tile_height;
    g_assert(image00_w % tile_width == 0);
    g_assert(image00_h % tile_height == 0);

    l->jpegs_across = jpegs_across;
    l->jpegs_down = jpegs_down;

    l->pixel_w = pixel_w / scale_denom;
    l->pixel_h = pixel_h / scale_denom;
    l->tile_width = tile_width / scale_denom;
    l->tile_height = tile_height / scale_denom;

    // create array and copy
    l->layer_jpegs = g_hash_table_new_full(g_int_hash, g_int_equal,
					   int_free, NULL);
    //    g_debug("g_new(struct one_jpeg *) -> %p", (void *) l->layer_jpegs);
    GSList *jj = jpegs;
    for (int32_t i = 0; i < num_jpegs; i++) {
      g_assert(jj);

      // only insert if not blank
      struct one_jpeg *oj = jj->data;
      if (oj->f) {
	int *key = g_slice_new(int);
	*key = i;
	g_hash_table_insert(l->layer_jpegs, key, jj->data);
	//	g_debug("insert (%p): %d, %p, scale_denom: %d", l->layer_jpegs, i, jj->data, scale_denom);
      }
	jj = jj->next;
    }

    // put into map
    int64_t *key = g_slice_new(int64_t);
    *key = l->pixel_w;

    //    g_debug("insert %" PRId64 ", scale_denom: %d", *key, scale_denom);
    g_hash_table_insert(width_to_layer_map, key, l);
  }
}

static GHashTable *create_width_to_layer_map(int32_t count,
					     struct _openslide_jpeg_fragment **fragments,
					     struct one_jpeg **jpegs,
					     double downsample_override) {
  int64_t prev_z = -1;
  int64_t prev_x = -1;
  int64_t prev_y = -1;

  GSList *layer_jpegs_tmp = NULL;
  int64_t l_pw = 0;
  int64_t l_ph = 0;

  int32_t img00_w = 0;
  int32_t img00_h = 0;

  int32_t img00_tw = 0;
  int32_t img00_th = 0;

  int64_t layer0_w = 0;
  int64_t layer0_h = 0;

  // int* -> struct layer*
  GHashTable *width_to_layer_map = g_hash_table_new_full(int64_hash,
							 int64_equal,
							 int64_free,
							 layer_free);

  // go through the fragments, accumulating to layers
  for (int32_t i = 0; i < count; i++) {
    struct _openslide_jpeg_fragment *fr = fragments[i];
    struct one_jpeg *oj = jpegs[i];

    // the fragments MUST be in sorted order by z,x,y
    g_assert(is_zxy_successor(prev_z, prev_x, prev_y,
			      fr->z, fr->x, fr->y));

    // special case for first layer
    if (prev_z == -1) {
      prev_z = 0;
      prev_x = 0;
      prev_y = 0;
    }

    // save first image dimensions
    if (fr->x == 0 && fr->y == 0) {
      img00_w = oj->width;
      img00_h = oj->height;
      img00_tw = oj->tile_width;
      img00_th = oj->tile_height;
    }

    // assert all tile sizes are the same in a layer
    g_assert(img00_tw == oj->tile_width);
    g_assert(img00_th == oj->tile_height);

    // accumulate size
    if (fr->y == 0) {
      l_pw += oj->width;
    }
    if (fr->x == 0) {
      l_ph += oj->height;
    }

    //    g_debug(" pw: %" PRId64 ", ph: %" PRId64, l_pw, l_ph);

    // accumulate to layer
    layer_jpegs_tmp = g_slist_prepend(layer_jpegs_tmp, oj);

    // is this the end of this layer? then flush
    if (i == count - 1 || fragments[i + 1]->z != fr->z) {
      layer_jpegs_tmp = g_slist_reverse(layer_jpegs_tmp);

      // first layer?
      if (fr->z == 0) {
	// save layer0 width
	layer0_w = l_pw;
	layer0_h = l_ph;
      } else if (downsample_override) {
	// otherwise, maybe override
	g_debug("overriding from %" PRId64 " %" PRId64, l_pw, l_ph);
	l_pw = nearbyint(layer0_w / pow(downsample_override, fr->z));
	l_ph = nearbyint(layer0_h / pow(downsample_override, fr->z));
	g_debug(" to %" PRId64 " %" PRId64, l_pw, l_ph);
      }

      generate_layer_into_map(layer_jpegs_tmp, fr->x + 1, fr->y + 1,
			      l_pw, l_ph,
			      img00_w, img00_h,
			      img00_tw, img00_th,
			      layer0_w,
			      width_to_layer_map,
			      pow(downsample_override, fr->z));

      // clear for next round
      l_pw = 0;
      l_ph = 0;
      img00_w = 0;
      img00_h = 0;

      while (layer_jpegs_tmp != NULL) {
	layer_jpegs_tmp = g_slist_delete_link(layer_jpegs_tmp, layer_jpegs_tmp);
      }
    }

    // update prevs
    prev_z = fr->z;
    prev_x = fr->x;
    prev_y = fr->y;
  }

  return width_to_layer_map;
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


static void read_from_one_jpeg (struct one_jpeg *jpeg,
				uint32_t *dest,
				int32_t tile_x, int32_t tile_y,
				double ovr_spacing_x, double ovr_spacing_y,
				double overlap_x, double overlap_y,
				int32_t scale_denom) {
  //g_debug("read_from_one_jpeg: %p, dest: %p, x: %d, y: %d, scale_denom: %d", (void *) jpeg, (void *) dest, tile_x, tile_y, scale_denom);

  // figure out where to start the data stream
  int64_t mcu_start = tile_y * (jpeg->width / jpeg->tile_width) + tile_x;

  int64_t stop_position;

  g_assert(jpeg->f);

  compute_mcu_start(jpeg->f,
		    jpeg->mcu_starts,
		    jpeg->unreliable_mcu_starts,
		    jpeg->start_in_file,
		    jpeg->end_in_file,
		    mcu_start);
  if (jpeg->mcu_starts_count == mcu_start + 1) {
    // EOF
    stop_position = jpeg->end_in_file;
  } else {
    compute_mcu_start(jpeg->f,
		      jpeg->mcu_starts,
		      jpeg->unreliable_mcu_starts,
		      jpeg->start_in_file,
		      jpeg->end_in_file,
		      mcu_start + 1);
    stop_position = jpeg->mcu_starts[mcu_start + 1];
  }

  // begin decompress
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  jmp_buf env;

  gsize row_size = 0;

  JSAMPARRAY buffer = g_slice_alloc0(sizeof(JSAMPROW) * MAX_SAMP_FACTOR);

  if (setjmp(env) == 0) {
    //cinfo.err = jpeg_std_error(&jerr);
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
    jpeg_create_decompress(&cinfo);

    jpeg_random_access_src(&cinfo, jpeg->f,
			   jpeg->start_in_file,
			   jpeg->mcu_starts[0],
			   jpeg->mcu_starts[mcu_start],
			   stop_position);

    jpeg_read_header(&cinfo, TRUE);
    cinfo.scale_denom = scale_denom;
    cinfo.image_width = jpeg->tile_width;  // cunning
    cinfo.image_height = jpeg->tile_height;
    cinfo.out_color_space = JCS_RGB;

    jpeg_start_decompress(&cinfo);

    //g_debug("output_width: %d", cinfo.output_width);
    //g_debug("output_height: %d", cinfo.output_height);

    // allocate scanline buffers
    row_size = sizeof(JSAMPLE) * cinfo.output_width * cinfo.output_components;
    for (int i = 0; i < cinfo.rec_outbuf_height; i++) {
      buffer[i] = g_slice_alloc(row_size);
      //g_debug("buffer[%d]: %p", i, buffer[i]);
    }

    // decompress
    int current_row = 0;
    int rows_to_skip = 0;
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
	  dest[dest_i++] = 0xFF000000 |           // A
	    buffer[cur_buffer][i * 3 + 0] << 16 | // R
	    buffer[cur_buffer][i * 3 + 1] << 8 |  // G
	    buffer[cur_buffer][i * 3 + 2];        // B
	}

	// advance everything 1 row
	dest += cinfo.output_width;
	cur_buffer++;
	current_row++;
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

  // last thing, stop jpeg
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo.src;   // sorry
  g_slice_free1(src->buffer_size, src->buffer);
  jpeg_destroy_decompress(&cinfo);
}

static bool read_tile_unlocked(struct layer *l,
			       uint32_t *dest,
			       int64_t tile_x, int64_t tile_y) {
  int32_t file_x = tile_x / l->tiles_across_per_file;
  int32_t file_y = tile_y / l->tiles_down_per_file;

  int jpeg_number = file_y * l->jpegs_across + file_x;
  g_assert(jpeg_number < l->jpegs_across * l->jpegs_down);

  struct one_jpeg *jpeg = g_hash_table_lookup(l->layer_jpegs, &jpeg_number);
  //  g_debug("lookup (%p): %d -> %p", l->layer_jpegs, jpeg_number, jpeg);

  if (!jpeg) {
    return false;
  } else {
    read_from_one_jpeg(jpeg, dest,
		       tile_x % l->tiles_across_per_file,
		       tile_y % l->tiles_down_per_file,
		       l->overlap_spacing_x, l->overlap_spacing_y,
		       l->overlap_x, l->overlap_y,
		       l->scale_denom);
    return true;
  }
}

static bool read_tile(openslide_t *osr, uint32_t *dest,
		      int32_t layer,
		      int64_t tile_x, int64_t tile_y) {
  struct jpegops_data *data = osr->data;

  // get the layer
  struct layer *l = data->layers + layer;

  // tell the background thread to pause
  g_mutex_lock(data->restart_marker_cond_mutex);
  data->restart_marker_thread_state = R_M_THREAD_STATE_PAUSE;
  //  g_debug("telling thread to pause");
  g_mutex_unlock(data->restart_marker_cond_mutex);

  // wait until thread is paused
  g_mutex_lock(data->restart_marker_mutex);
  bool result = read_tile_unlocked(l, dest, tile_x, tile_y);
  g_mutex_unlock(data->restart_marker_mutex);

  // tell the background thread to resume
  g_mutex_lock(data->restart_marker_cond_mutex);
  g_timer_start(data->restart_marker_timer);
  data->restart_marker_thread_state = R_M_THREAD_STATE_RUN;
  //  g_debug("telling thread to awaken");
  g_cond_signal(data->restart_marker_cond);
  g_mutex_unlock(data->restart_marker_cond_mutex);

  return result;
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
    g_hash_table_unref(l->layer_jpegs);
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

static void get_layer_dimensions(struct layer *l,
				 int64_t *tiles_across, int64_t *tiles_down,
				 int32_t *tile_width, int32_t *tile_height,
				 int32_t *last_tile_width, int32_t *last_tile_height) {
  *tiles_across = (l->pixel_w / l->tile_width) + !!(l->pixel_w % l->tile_width);
  *tiles_down = (l->pixel_h / l->tile_height) + !!(l->pixel_h % l->tile_height);

  // overlaps
  int32_t overlaps_per_tile_across = 0;
  int64_t overlaps_across = 0;
  if (l->overlap_spacing_x) {
    overlaps_per_tile_across = l->tile_width / l->overlap_spacing_x;
    overlaps_across = (*tiles_across * overlaps_per_tile_across) - 1;
  }
  int32_t overlaps_per_tile_down = 0;
  int64_t overlaps_down = 0;
  if (l->overlap_spacing_y) {
    overlaps_per_tile_down = l->tile_height / l->overlap_spacing_y;
    overlaps_down = (*tiles_down * overlaps_per_tile_down) - 1;
  }

  g_debug("o_p_t_a: %d, o_p_t_d: %d, o_a: %" PRId64 ", o_d: %" PRId64,
	  overlaps_per_tile_across, overlaps_per_tile_down, overlaps_across, overlaps_down);
  g_debug(" overlap_x: %g, overlap_y: %g", l->overlap_x, l->overlap_y);

  double overlap_in_tile_x = overlaps_per_tile_across * l->overlap_x;
  double overlap_in_tile_y = overlaps_per_tile_down * l->overlap_y;
  g_debug(" overlap in tile: %g %g", overlap_in_tile_x, overlap_in_tile_y);

  *tile_width = l->tile_width - overlaps_per_tile_across * l->overlap_x;
  *tile_height = l->tile_height - overlaps_per_tile_down * l->overlap_y;

  g_debug(" tile size: %d %d", *tile_width, *tile_height);

  int64_t w_minus_overlaps = l->pixel_w - (int64_t) (overlaps_across * l->overlap_x);
  int64_t h_minus_overlaps = l->pixel_h - (int64_t) (overlaps_down * l->overlap_y);

  g_debug(" w-o: %" PRId64 ", h-o: %" PRId64, w_minus_overlaps, h_minus_overlaps);

  *last_tile_width = w_minus_overlaps - (*tile_width * (*tiles_across - 1));
  *last_tile_height = h_minus_overlaps - (*tile_height * (*tiles_down - 1));

  g_debug(" last tile size: %d %d", *last_tile_width, *last_tile_height);
}


static void get_dimensions(openslide_t *osr,
			   int32_t layer,
			   int64_t *tiles_across, int64_t *tiles_down,
			   int32_t *tile_width, int32_t *tile_height,
			   int32_t *last_tile_width, int32_t *last_tile_height) {
  struct jpegops_data *data = osr->data;
  get_layer_dimensions(data->layers + layer,
		       tiles_across, tiles_down,
		       tile_width, tile_height,
		       last_tile_width, last_tile_height);
}

static struct _openslide_ops jpeg_ops = {
  .destroy = destroy,
  .read_tile = read_tile,
  .get_dimensions = get_dimensions,
};


static void init_one_jpeg(struct one_jpeg *onej,
			  struct _openslide_jpeg_fragment *fragment) {
  // file is present
  if (fragment->f) {
    onej->f = fragment->f;
    onej->start_in_file = fragment->start_in_file;
    onej->end_in_file = fragment->end_in_file;
    onej->unreliable_mcu_starts = fragment->mcu_starts;
  }

  g_assert(fragment->w && fragment->h && fragment->tw && fragment->th);

  onej->width = fragment->w;
  onej->height = fragment->h;
  onej->tile_width = fragment->tw;
  onej->tile_height = fragment->th;

  if (onej->f) {
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
}

static gint width_compare(gconstpointer a, gconstpointer b) {
  int64_t w1 = *((const int64_t *) a);
  int64_t w2 = *((const int64_t *) b);

  g_assert(w1 >= 0 && w2 >= 0);

  return (w1 < w2) - (w1 > w2);
}

static void get_keys(gpointer key, gpointer value,
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
			     int32_t count,
			     struct _openslide_jpeg_fragment **fragments,
			     int32_t overlap_count,
			     double *overlaps,
			     double downsample_override,
			     enum _openslide_overlap_mode overlap_mode) {
  //  g_debug("count: %d", count);
  //  for (int32_t i = 0; i < count; i++) {
    //    struct _openslide_jpeg_fragment *frag = fragments[i];
    //    g_debug("%d: file: %p, x: %d, y: %d, z: %d",
    //	    i, (void *) frag->f, frag->x, frag->y, frag->z);
  //  }

  if (osr == NULL) {
    // free now and return
    GHashTable *fclose_hashtable = filehandle_hashtable_new();
    for (int32_t i = 0; i < count; i++) {
      if (fragments[i]->f) {
	filehandle_hashtable_conditional_insert(fclose_hashtable,
						fragments[i]->f);
	g_free(fragments[i]->mcu_starts);
      }
      g_slice_free(struct _openslide_jpeg_fragment, fragments[i]);
    }
    g_hash_table_unref(fclose_hashtable);
    g_free(fragments);
    g_free(overlaps);
    return;
  }

  g_assert(osr->data == NULL);


  // allocate private data
  struct jpegops_data *data = g_slice_new0(struct jpegops_data);
  osr->data = data;

  // load all jpegs (assume all are useful)
  data->jpeg_count = count;
  data->all_jpegs = g_new0(struct one_jpeg *, count);
  for (int32_t i = 0; i < data->jpeg_count; i++) {
    //    g_debug("init JPEG %d", i);
    data->all_jpegs[i] = g_slice_new0(struct one_jpeg);
    init_one_jpeg(data->all_jpegs[i], fragments[i]);
  }

  // create map from width to layers, using the fragments
  GHashTable *width_to_layer_map = create_width_to_layer_map(count,
							     fragments,
							     data->all_jpegs,
							     downsample_override);

  // sort all_jpegs by file and start position, so we can avoid seeks
  // when background finding mcus
  qsort(data->all_jpegs, count, sizeof(struct one_jpeg *), one_jpeg_compare);

  // delete all the fragments
  for (int32_t i = 0; i < count; i++) {
    g_slice_free(struct _openslide_jpeg_fragment, fragments[i]);
  }
  g_free(fragments);

  // get sorted keys
  GList *layer_keys = NULL;
  g_hash_table_foreach(width_to_layer_map, get_keys, &layer_keys);
  layer_keys = g_list_sort(layer_keys, width_compare);

  //  g_debug("number of keys: %d", g_list_length(layer_keys));


  // populate the layer_count
  osr->layer_count = g_hash_table_size(width_to_layer_map);

  // load into data array
  data->layers = g_new(struct layer, g_hash_table_size(width_to_layer_map));
  GList *tmp_list = layer_keys;

  int i = 0;

  //  g_debug("copying sorted layers");
  while(tmp_list != NULL) {
    // get a key and value
    struct layer *l = g_hash_table_lookup(width_to_layer_map, tmp_list->data);

    // copy
    struct layer *dest = data->layers + i;
    *dest = *l;    // shallow copy

    // manually free some things, because of that shallow copy
    g_hash_table_steal(width_to_layer_map, tmp_list->data);
    int64_free(tmp_list->data);  // key
    g_slice_free(struct layer, l); // shallow deletion of layer

    // consume the head and continue
    tmp_list = g_list_delete_link(tmp_list, tmp_list);
    i++;
  }

  // from the layers, generate the overlaps
  if (overlap_count) {
    for (int32_t i = 0; i < osr->layer_count; i++) {
      struct layer *l = data->layers + i;

      int32_t scale_denom = l->scale_denom;
      g_assert(scale_denom);

      int32_t lg2_scale_denom = g_bit_nth_lsf(scale_denom, -1) + 1;
      int32_t overlaps_i = i - (lg2_scale_denom - 1);
      g_debug("scale_denom: %d, lg2: %d", scale_denom, lg2_scale_denom);

      if (overlaps_i >= overlap_count) {
	// we have more layers than overlaps, stop
	break;
      }

      double orig_ox = overlaps[overlaps_i * 2];
      double orig_oy = overlaps[(overlaps_i * 2) + 1];

      g_debug("orig overlaps: %g %g", orig_ox, orig_oy);

      l->overlap_x = overlaps[overlaps_i * 2] / scale_denom;
      l->overlap_y = overlaps[(overlaps_i * 2) + 1] / scale_denom;

      g_debug("overlaps: %g %g", l->overlap_x, l->overlap_y);

      // set the spacing
      switch(overlap_mode) {
      case OPENSLIDE_OVERLAP_MODE_SANE:
	l->overlap_spacing_x = l->tile_width;
	l->overlap_spacing_y = l->tile_height;
	break;

      case OPENSLIDE_OVERLAP_MODE_INTERNAL:
	l->overlap_spacing_x = l->tile_width / l->no_scale_denom_downsample;
	l->overlap_spacing_y = l->tile_height / l->no_scale_denom_downsample;
	break;

      default:
	g_assert_not_reached();
      }

      g_debug("overlap spacing: %g %g", l->overlap_spacing_x, l->overlap_spacing_y);
    }
  }

  // init cache
  data->cache = _openslide_cache_create(_OPENSLIDE_USEFUL_CACHE_SIZE);

  // unref the hash table
  g_hash_table_unref(width_to_layer_map);

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
  /*
  g_thread_join(data->restart_marker_thread);
  verify_mcu_starts(data);
  */

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
