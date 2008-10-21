/*
 *  Wholeslide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  Wholeslide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  Wholeslide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Wholeslide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking Wholeslide statically or dynamically with other modules is
 *  making a combined work based on Wholeslide. Thus, the terms and
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <jpeglib.h>
#include <jerror.h>
#include <inttypes.h>

#include <sys/types.h>   // for off_t ?

#include "wholeslide-private.h"

struct one_jpeg {
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  FILE *f;

  uint64_t mcu_starts_count;
  int64_t *mcu_starts;

  uint32_t tile_width;
  uint32_t tile_height;

  uint32_t width;
  uint32_t height;

  char *comment;
};

struct layer_lookup {
  uint32_t jpeg_number;
  uint32_t scale_denom;
};

struct jpegops_data {
  uint32_t jpeg_count;
  struct one_jpeg *jpegs;

  struct layer_lookup *layers;
};

static void read_region(wholeslide_t *wsd, uint32_t *dest,
			uint32_t x, uint32_t y,
			uint32_t layer,
			uint32_t w, uint32_t h) {
  struct jpegops_data *data = wsd->data;

  // clear
  memset(dest, 0, w * h * sizeof(uint32_t));

  // in layer bounds?
  if (layer >= wsd->layer_count) {
    return;
  }

  // figure out jpeg and downsample
  struct layer_lookup *ll = &data->layers[layer];
  struct one_jpeg *jpeg = &data->jpegs[ll->jpeg_number];
  uint32_t scale_denom = ll->scale_denom;
  uint32_t rel_downsample = data->jpegs[0].width / jpeg->width;

  //  printf("jpeg: %d, rel_downsample: %d, scale_denom: %d\n",
  //	 ll->jpeg_number, rel_downsample, scale_denom);

  // scale x and y into this jpeg's space
  x /= rel_downsample;
  y /= rel_downsample;
  if (x >= jpeg->width || y >= jpeg->height) {
    return;
  }

  // figure out where to start the data stream
  uint32_t tile_y = y / jpeg->tile_height;
  uint32_t tile_x = x / jpeg->tile_width;

  uint32_t stride_in_tiles = jpeg->width / jpeg->tile_width;
  uint32_t img_height_in_tiles = jpeg->height / jpeg->tile_height;

  imaxdiv_t divtmp;
  divtmp = imaxdiv((w * scale_denom) + (x % jpeg->tile_width), jpeg->tile_width);
  uint32_t width_in_tiles = divtmp.quot + !!divtmp.rem;  // integer ceil
  divtmp = imaxdiv((h * scale_denom) + (y % jpeg->tile_height), jpeg->tile_height);
  uint32_t height_in_tiles = divtmp.quot + !!divtmp.rem;

  // clamp width and height
  width_in_tiles = MIN(width_in_tiles, stride_in_tiles - tile_x);
  height_in_tiles = MIN(height_in_tiles, img_height_in_tiles - tile_y);

  //  printf("width_in_tiles: %d, stride_in_tiles: %d\n", width_in_tiles, stride_in_tiles);
  //  printf("tile_x: %d, tile_y: %d\n", tile_x, tile_y);

  rewind(jpeg->f);
  _ws_jpeg_fancy_src(&jpeg->cinfo, jpeg->f,
  		     jpeg->mcu_starts,
		     jpeg->mcu_starts_count,
		     tile_y * stride_in_tiles + tile_x,
		     width_in_tiles,
		     stride_in_tiles);

  // begin decompress
  uint32_t rows_left = h;
  jpeg_read_header(&jpeg->cinfo, FALSE);
  jpeg->cinfo.scale_denom = scale_denom;
  jpeg->cinfo.image_width = width_in_tiles * jpeg->tile_width;  // cunning
  jpeg->cinfo.image_height = height_in_tiles * jpeg->tile_height;

  jpeg_start_decompress(&jpeg->cinfo);
  g_assert(jpeg->cinfo.output_components == 3); // XXX remove this assertion

  //  printf("output_width: %d\n", jpeg->cinfo.output_width);
  //  printf("output_height: %d\n", jpeg->cinfo.output_height);

  // allocate scanline buffers
  JSAMPARRAY buffer =
    g_slice_alloc(sizeof(JSAMPROW) * jpeg->cinfo.rec_outbuf_height);
  gsize row_size =
    sizeof(JSAMPLE)
    * jpeg->cinfo.output_width
    * 3;  // output components
  for (int i = 0; i < jpeg->cinfo.rec_outbuf_height; i++) {
    buffer[i] = g_slice_alloc(row_size);
    //printf("buffer[%d]: %p\n", i, buffer[i]);
  }

  // decompress
  uint32_t d_x = (x % jpeg->tile_width) / scale_denom;
  uint32_t d_y = (y % jpeg->tile_height) / scale_denom;
  uint32_t rows_to_skip = d_y;

  //  printf("d_x: %d, d_y: %d\n", d_x, d_y);

  uint64_t pixels_wasted = rows_to_skip * jpeg->cinfo.output_width;

  //  abort();

  while (jpeg->cinfo.output_scanline < jpeg->cinfo.output_height
	 && rows_left > 0) {
    JDIMENSION rows_read = jpeg_read_scanlines(&jpeg->cinfo,
					       buffer,
					       jpeg->cinfo.rec_outbuf_height);
    //    printf("just read scanline %d\n", jpeg->cinfo.output_scanline - rows_read);
    //    printf(" rows read: %d\n", rows_read);
    int cur_buffer = 0;
    while (rows_read > 0 && rows_left > 0) {
      // copy a row
      if (rows_to_skip == 0) {
	uint32_t i;
	for (i = 0; i < w && i < (jpeg->cinfo.output_width - d_x); i++) {
	  dest[i] = 0xFF000000 |                          // A
	    buffer[cur_buffer][(d_x + i) * 3 + 0] << 16 | // R
	    buffer[cur_buffer][(d_x + i) * 3 + 1] << 8 |  // G
	    buffer[cur_buffer][(d_x + i) * 3 + 2];        // B
	}
	pixels_wasted += d_x + jpeg->cinfo.output_width - i;
      }

      // advance everything 1 row
      rows_read--;
      cur_buffer++;

      if (rows_to_skip > 0) {
	rows_to_skip--;
      } else {
	rows_left--;
	dest += w;
      }
    }
  }

  //  printf("pixels wasted: %llu\n", pixels_wasted);

  // free buffers
  for (int i = 0; i < jpeg->cinfo.rec_outbuf_height; i++) {
    g_slice_free1(row_size, buffer[i]);
  }
  g_slice_free1(sizeof(JSAMPROW) * jpeg->cinfo.rec_outbuf_height, buffer);

  // last thing, stop jpeg
  jpeg_abort_decompress(&jpeg->cinfo);
}


static void destroy(wholeslide_t *wsd) {
  struct jpegops_data *data = wsd->data;

  // layer_lookup
  g_free(data->layers);

  // each jpeg in turn
  for (uint32_t i = 0; i < data->jpeg_count; i++) {
    struct one_jpeg *jpeg = &data->jpegs[i];

    jpeg_destroy_decompress(&jpeg->cinfo);
    fclose(jpeg->f);
    g_free(jpeg->mcu_starts);
    g_free(jpeg->comment);
  }

  // the array
  g_free(data->jpegs);

  // the structure
  g_slice_free(struct jpegops_data, data);
}

static void get_dimensions(wholeslide_t *wsd, uint32_t layer,
			   uint32_t *w, uint32_t *h) {
  struct jpegops_data *data = wsd->data;

  // check bounds
  if (layer >= wsd->layer_count) {
    *w = 0;
    *h = 0;
    return;
  }

  struct layer_lookup *ll = &data->layers[layer];
  struct one_jpeg *jpeg = &data->jpegs[ll->jpeg_number];
  *w = jpeg->width / ll->scale_denom;
  *h = jpeg->height / ll->scale_denom;
}

static const char* get_comment(wholeslide_t *wsd) {
  struct jpegops_data *data = wsd->data;
  return data->jpegs[0].comment;
}

static struct _wholeslide_ops jpeg_ops = {
  .read_region = read_region,
  .destroy = destroy,
  .get_dimensions = get_dimensions,
  .get_comment = get_comment,
};


static void init_one_jpeg(struct one_jpeg *jpeg) {
  FILE *f = jpeg->f;

  // init jpeg
  rewind(f);
  jpeg->cinfo.err = jpeg_std_error(&jpeg->jerr);
  jpeg_create_decompress(&jpeg->cinfo);
  _ws_jpeg_fancy_src(&jpeg->cinfo, f,
		     NULL, 0, 0, 0, 0);

  // extract comment
  jpeg_save_markers(&jpeg->cinfo, JPEG_COM, 0xFFFF);
  jpeg_read_header(&jpeg->cinfo, FALSE);
  if (jpeg->cinfo.marker_list) {
    // copy everything out
    char *com = g_strndup((const gchar *) jpeg->cinfo.marker_list->data,
			  jpeg->cinfo.marker_list->data_length);
    // but only really save everything up to the first '\0'
    jpeg->comment = g_strdup(com);
    g_free(com);
  }
  jpeg_save_markers(&jpeg->cinfo, JPEG_COM, 0);  // stop saving

  // save dimensions
  jpeg_calc_output_dimensions(&jpeg->cinfo);
  jpeg->width = jpeg->cinfo.output_width;
  jpeg->height = jpeg->cinfo.output_height;

  // save "tile" dimensions
  jpeg_start_decompress(&jpeg->cinfo);
  jpeg->tile_width = jpeg->width /
    (jpeg->cinfo.MCUs_per_row / jpeg->cinfo.restart_interval);
  jpeg->tile_height = jpeg->height / jpeg->cinfo.MCU_rows_in_scan;

  //  printf("jpeg \"tile\" dimensions: %dx%d\n", jpeg->tile_width, jpeg->tile_height);

  // quiesce jpeg
  jpeg_abort_decompress(&jpeg->cinfo);
}

struct populate_layer_data {
  uint32_t i;
  struct jpegops_data *data;
};

static void populate_layer_array_helper(gpointer key, gpointer value, gpointer user_data) {
  uint32_t w = (uint32_t) key;
  uint32_t n = (uint32_t) value;
  struct populate_layer_data *d = user_data;

  //  printf("%d: %d -> %d\n", d->i, w, n);

  struct layer_lookup *ll = &d->data->layers[d->i++];
  ll->jpeg_number = n;
  ll->scale_denom = d->data->jpegs[n].width / w;
  //  printf(" %d: %d\n", ll->jpeg_number, ll->scale_denom);
}

static int layer_lookup_compare(const void *p1, const void *p2) {
  const struct layer_lookup *ll1 = p1;
  const struct layer_lookup *ll2 = p2;

  uint32_t j1 = ll1->jpeg_number;
  uint32_t j2 = ll2->jpeg_number;
  uint32_t s1 = ll1->scale_denom;
  uint32_t s2 = ll2->scale_denom;

  // sort by jpeg_number, then scale_denom
  int v1 = (j1 > j2) - (j1 < j2);
  if (v1 != 0) {
    return v1;
  } else {
    return (s1 > s2) - (s1 < s2);
  }
}

static int jpegops_width_compare(const void *p1, const void *p2) {
  uint32_t w1 = ((const struct one_jpeg *) p1)->width;
  uint32_t w2 = ((const struct one_jpeg *) p2)->width;

  return (w1 < w2) - (w1 > w2);
}

static void compute_optimization(FILE *f,
				 uint64_t *mcu_starts_count,
				 int64_t **mcu_starts) {
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  // generate the optimization list, by finding restart markers
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  rewind(f);
  _ws_jpeg_fancy_src(&cinfo, f, NULL, 0, 0, 0, 0);

  jpeg_read_header(&cinfo, TRUE);
  jpeg_start_decompress(&cinfo);

  uint64_t MCUs = cinfo.MCUs_per_row * cinfo.MCU_rows_in_scan;
  *mcu_starts_count = MCUs / cinfo.restart_interval;
  *mcu_starts = g_new0(int64_t, *mcu_starts_count);

  // the first entry
  (*mcu_starts)[0] = _ws_jpeg_fancy_src_get_filepos(&cinfo);

  // now find the rest of the MCUs
  bool last_was_ff = false;
  uint64_t marker = 0;
  while (marker < *mcu_starts_count) {
    if (cinfo.src->bytes_in_buffer == 0) {
      (cinfo.src->fill_input_buffer)(&cinfo);
    }
    uint8_t b = *(cinfo.src->next_input_byte++);
    cinfo.src->bytes_in_buffer--;

    if (last_was_ff) {
      // EOI?
      if (b == JPEG_EOI) {
	// we're done
	break;
      } else if (b >= 0xD0 && b < 0xD8) {
	// marker
	(*mcu_starts)[1 + marker++] = _ws_jpeg_fancy_src_get_filepos(&cinfo);
      }
    }
    last_was_ff = b == 0xFF;
  }

  /*
  for (uint64_t i = 0; i < *mcu_starts_count; i++) {
    printf(" %lld\n", (*mcu_starts)[i]);
  }
  */

  // success, now clean up
  jpeg_destroy_decompress(&cinfo);
}


void _ws_add_jpeg_ops(wholeslide_t *wsd,
		      uint32_t file_count,
		      FILE **f) {
  if (wsd == NULL) {
    // free now and return
    for (uint32_t i = 0; i < file_count; i++) {
      fclose(f[i]);
    }
    return;
  }


  g_assert(wsd->data == NULL);

  // allocate private data
  struct jpegops_data *data = g_slice_new0(struct jpegops_data);
  wsd->data = data;

  data->jpeg_count = file_count;
  data->jpegs = g_new0(struct one_jpeg, data->jpeg_count);

  for (uint32_t i = 0; i < data->jpeg_count; i++) {
    // copy parameters
    struct one_jpeg *jpeg = &data->jpegs[i];

    uint64_t mcu_starts_count;
    int64_t *mcu_starts;

    compute_optimization(f[i], &mcu_starts_count, &mcu_starts);

    jpeg->f = f[i];
    jpeg->mcu_starts_count = mcu_starts_count;
    jpeg->mcu_starts = mcu_starts;

    init_one_jpeg(jpeg);
  }

  // sort the jpegs by base width, larger to smaller
  qsort(data->jpegs, data->jpeg_count, sizeof(struct one_jpeg),
	jpegops_width_compare);

  // map downsampled width to jpeg number, favoring smaller scale_denoms
  GHashTable *layer_hash = g_hash_table_new(NULL, NULL);
  for (uint32_t i = 0; i < data->jpeg_count; i++) {
    for (uint32_t j = 0; j < 4; j++) {
      // each JPEG can be read as 1/1, 1/2, 1/4, 1/8
      uint32_t d = 1 << j;
      uint32_t w = data->jpegs[i].width / d;

      g_hash_table_insert(layer_hash,
			  (gpointer) w, (gpointer) i); // will replace previous ones
    }
  }

  // populate the layer list
  wsd->layer_count = g_hash_table_size(layer_hash);
  data->layers = g_new(struct layer_lookup, wsd->layer_count);

  struct populate_layer_data pld = { .i = 0, .data = data };
  g_hash_table_foreach(layer_hash, populate_layer_array_helper, &pld);
  g_hash_table_destroy(layer_hash);


  // now, make sure the layer list is sorted
  qsort(data->layers, wsd->layer_count, sizeof(struct layer_lookup),
	layer_lookup_compare);

  for (uint32_t i = 0; i < wsd->layer_count; i++) {
    //    printf("%d: %d\n", data->layers[i].jpeg_number,
    //	   data->layers[i].scale_denom);
  }

  // set ops
  wsd->ops = &jpeg_ops;
  g_warning("JPEG support is buggy and unfinished");
}



/*
 * Source manager for doing fancy things with libjpeg and restart markers,
 * initially copied from jdatasrc.c from IJG libjpeg.
 */
struct my_src_mgr {
  struct jpeg_source_mgr pub;   /* public fields */

  FILE *infile;                 /* source stream */
  JOCTET *buffer;               /* start of buffer */
  bool start_of_file;
  uint8_t next_restart_marker;

  int64_t next_start_offset;
  int64_t next_start_position;
  int64_t stop_position;

  int64_t header_length;
  int64_t *start_positions;
  uint64_t start_positions_count;
  uint64_t topleft;
  uint32_t width;
  uint32_t stride;
};

#define INPUT_BUF_SIZE  4096    /* choose an efficiently fread'able size */

static void compute_next_positions (struct my_src_mgr *src) {
  if (src->start_positions_count == 0) {
    // no positions given, do the whole file
    src->next_start_position = 0;
    src->stop_position = INT64_MAX;

    //    printf("next start offset: %lld\n", src->next_start_offset);
    //    printf("(count==0) next start: %lld, stop: %lld\n", src->next_start_position, src->stop_position);

    return;
  }

  // do special case for header
  if (src->start_of_file) {
    src->next_start_offset = src->topleft - src->stride;  // next time, start at topleft
    src->stop_position = src->start_positions[0];         // stop at data start
    g_assert(src->next_start_offset < (int64_t) src->start_positions_count);
    src->next_start_position = 0;

    //    printf("next start offset: %lld\n", src->next_start_offset);
    //    printf("(start_of_file) next start: %lld, stop: %lld\n", src->next_start_position, src->stop_position);

    return;
  }

  // advance
  src->next_start_offset += src->stride;

  // compute next jump point
  g_assert(src->next_start_offset >= 0
	   && src->next_start_offset < (int64_t) src->start_positions_count);
  src->next_start_position = src->start_positions[src->next_start_offset];

  // compute stop point, or end of file
  uint64_t stop_offset = src->next_start_offset + src->width;
  if (stop_offset < src->start_positions_count) {
    src->stop_position = src->start_positions[stop_offset];
  } else {
    src->stop_position = INT64_MAX;
  }

  //  printf("next start offset: %lld\n", src->next_start_offset);
  //  printf("next start: %lld, stop: %lld\n", src->next_start_position, src->stop_position);
}

static void init_source (j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;
  src->start_of_file = true;
  src->next_restart_marker = 0;
  compute_next_positions(src);
}

static boolean fill_input_buffer (j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;
  size_t nbytes;

  off_t pos = ftello(src->infile);

  boolean rewrite_markers = true;
  if (src->start_positions_count == 0 || pos < src->start_positions[0]) {
    rewrite_markers = false; // we are in the header, or we don't know where it is
  }

  g_assert(pos <= src->stop_position);

  size_t bytes_to_read = INPUT_BUF_SIZE;
  if (pos < src->stop_position) {
    // don't read past
    bytes_to_read = MIN((uint64_t) (src->stop_position - pos), bytes_to_read);
  } else if (pos == src->stop_position) {
    // skip to the jump point
    compute_next_positions(src);
    //    printf("at %lld, jump to %lld, will stop again at %lld\n", pos, src->next_start_position, src->stop_position);

    fseeko(src->infile, src->next_start_position, SEEK_SET);

    // figure out new stop position
    bytes_to_read = MIN((uint64_t) (src->stop_position - src->next_start_position),
			bytes_to_read);
  }

  //  printf(" bytes_to_read: %d\n", bytes_to_read);

  nbytes = fread(src->buffer, 1, bytes_to_read, src->infile);

  if (nbytes <= 0) {
    if (src->start_of_file) {
      ERREXIT(cinfo, JERR_INPUT_EMPTY);
    }
    WARNMS(cinfo, JWRN_JPEG_EOF);

    /* Insert a fake EOI marker */
    src->buffer[0] = (JOCTET) 0xFF;
    src->buffer[1] = (JOCTET) JPEG_EOI;
    nbytes = 2;
  } else if (rewrite_markers) {
    // rewrite the restart markers if we know for sure we are not in the header
    bool last_was_ff = false;

    for (size_t i = 0; i < nbytes; i++) {
      uint8_t b = src->buffer[i];
      if (last_was_ff && b >= 0xD0 && b < 0xD8) {
	src->buffer[i] = 0xD0 | src->next_restart_marker;
	//	printf("rewrite %x -> %x\n", b, src->buffer[i]);
	src->next_restart_marker = (src->next_restart_marker + 1) % 8;
      }
      last_was_ff = b == 0xFF;
    }

    // don't end on ff, unless it is the very last byte
    if (last_was_ff && nbytes > 1) {
      nbytes--;
      fseek(src->infile, -1, SEEK_CUR);
    }
  }

  src->pub.next_input_byte = src->buffer;
  src->pub.bytes_in_buffer = nbytes;
  src->start_of_file = false;

  return TRUE;
}


static void skip_input_data (j_decompress_ptr cinfo, long num_bytes) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;

  /* Just a dumb implementation for now.  Could use fseek() except
   * it doesn't work on pipes.  Not clear that being smart is worth
   * any trouble anyway --- large skips are infrequent.
   */
  if (num_bytes > 0) {
    while (num_bytes > (long) src->pub.bytes_in_buffer) {
      num_bytes -= (long) src->pub.bytes_in_buffer;
      (void) fill_input_buffer(cinfo);
      /* note we assume that fill_input_buffer will never return FALSE,
       * so suspension need not be handled.
       */
    }
    src->pub.next_input_byte += (size_t) num_bytes;
    src->pub.bytes_in_buffer -= (size_t) num_bytes;
  }
}


static void term_source (j_decompress_ptr cinfo) {
  /* no work necessary here */
}

int64_t _ws_jpeg_fancy_src_get_filepos(j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;

  return ftello(src->infile) - src->pub.bytes_in_buffer;
}

void _ws_jpeg_fancy_src (j_decompress_ptr cinfo, FILE *infile,
			 int64_t *start_positions,
			 uint64_t start_positions_count,
			 uint64_t topleft,
			 uint32_t width, uint32_t stride) {
  struct my_src_mgr *src;

  if (cinfo->src == NULL) {     /* first time for this JPEG object? */
    cinfo->src = (struct jpeg_source_mgr *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  sizeof(struct my_src_mgr));
    src = (struct my_src_mgr *) cinfo->src;
    src->buffer = (JOCTET *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  INPUT_BUF_SIZE * sizeof(JOCTET));
    //    printf("init fancy src with %p\n", src);
  }

  //  printf("fancy: start_positions_count: %llu, topleft: %llu, width: %d, stride: %d\n",
  //	 start_positions_count, topleft, width, stride);

  src = (struct my_src_mgr *) cinfo->src;
  src->pub.init_source = init_source;
  src->pub.fill_input_buffer = fill_input_buffer;
  src->pub.skip_input_data = skip_input_data;
  src->pub.resync_to_restart = jpeg_resync_to_restart; /* use default method */
  src->pub.term_source = term_source;
  src->infile = infile;
  src->start_positions = start_positions;
  src->start_positions_count = start_positions_count;
  src->topleft = topleft;
  src->width = width;
  src->stride = stride;
  src->pub.bytes_in_buffer = 0; /* forces fill_input_buffer on first read */
  src->pub.next_input_byte = NULL; /* until buffer loaded */
}
