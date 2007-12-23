/*
 * Part of this file is:
 *
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * This file is part of the Independent JPEG Group's software.
 * For conditions of distribution and use, see the accompanying README file.
 */

#include "config.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <jpeglib.h>
#include <jerror.h>

#include <sys/types.h>   // for off_t ?

#include "wholeslide-private.h"

struct _ws_jpegopsdata {
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  FILE *f;

  uint32_t mcu_row_count;
  int64_t *mcu_row_starts;

  char *comment;

  uint32_t widths[4];    // 4 layers
  uint32_t heights[4];
};

static void read_region(wholeslide_t *wsd, uint32_t *dest,
			uint32_t x, uint32_t y,
			uint32_t layer,
			uint32_t w, uint32_t h) {
  struct _ws_jpegopsdata *data = wsd->data;

  if (layer >= 4) {
    return;
  }

  // clear
  memset(dest, 0, w * h * sizeof(uint32_t));

  // select downsample
  uint32_t downsample = 1 << layer;
  data->cinfo.scale_denom = downsample;
  uint32_t d_x = x / downsample;
  uint32_t d_y = y / downsample;

  // select dct_method
  data->cinfo.dct_method = JDCT_FLOAT;

  // figure out where to start the data stream
  

  // adjust the height of the image to account for a short stream
  

  // begin decompress
  uint32_t rows_left = h;
  jpeg_start_decompress(&data->cinfo);
  g_assert(data->cinfo.output_components == 3);

  // allocate scanline buffers
  JSAMPARRAY buffer =
    g_slice_alloc(sizeof(JSAMPROW) * data->cinfo.rec_outbuf_height);
  gsize row_size =
    sizeof(JSAMPLE)
    * data->cinfo.output_width
    * 3;  // output components
  for (int i = 0; i < data->cinfo.rec_outbuf_height; i++) {
    buffer[i] = g_slice_alloc(row_size);
  }

  // skip to beginning of desired image
  

  // decompress
  while (data->cinfo.output_scanline < data->cinfo.output_height
	 && rows_left > 0) {
    JDIMENSION rows_read = jpeg_read_scanlines(&data->cinfo,
					       buffer,
					       data->cinfo.rec_outbuf_height);
    JSAMPARRAY cur_buffer = buffer;
    while (rows_read > 0 && rows_left > 0) {
      // copy a row
      for (uint32_t i = d_x; i < w && i < data->cinfo.output_width; i++) {
	dest[i] =
	  cur_buffer[0][i * 3 + 0] << 16 | // R
	  cur_buffer[0][i * 3 + 1] << 8 |  // G
	  cur_buffer[0][i * 3 + 2];        // B
      }

      // advance everything 1 row
      cur_buffer++;
      dest += w * sizeof(uint32_t);
      rows_read--;
      rows_left--;
    }
  }

  // free buffers
  for (int i = 0; i < data->cinfo.rec_outbuf_height; i++) {
    g_slice_free1(row_size, buffer[i]);
  }
  g_slice_free1(sizeof(JSAMPROW) * data->cinfo.rec_outbuf_height, buffer);

  // last thing, stop jpeg
  jpeg_abort_decompress(&data->cinfo);
}


static void destroy(wholeslide_t *wsd) {
  struct _ws_jpegopsdata *data = wsd->data;

  jpeg_destroy_decompress(&data->cinfo);
  fclose(data->f);

  g_free(data->mcu_row_starts);
  g_free(data->comment);
  g_slice_free(struct _ws_jpegopsdata, data);
}

static void get_dimensions(wholeslide_t *wsd, uint32_t layer,
			   uint32_t *w, uint32_t *h) {
  struct _ws_jpegopsdata *data = wsd->data;

  // check bounds
  if (layer >= 4) {
    *w = 0;
    *h = 0;
    return;
  }

  *w = data->widths[layer];
  *h = data->heights[layer];
}

static const char* get_comment(wholeslide_t *wsd) {
  struct _ws_jpegopsdata *data = wsd->data;
  return data->comment;
}

static struct _wholeslide_ops _ws_jpeg_ops = {
  .read_region = read_region,
  .destroy = destroy,
  .get_dimensions = get_dimensions,
  .get_comment = get_comment,
};

void _ws_add_jpeg_ops(wholeslide_t *wsd,
		      FILE *f,
		      uint32_t mcu_row_count,
		      int64_t *mcu_row_starts) {
  g_assert(wsd->data == NULL);

  // allocate private data
  struct _ws_jpegopsdata *data =  g_slice_new0(struct _ws_jpegopsdata);
  wsd->data = data;

  // copy parameters
  data->f = f;
  data->mcu_row_count = mcu_row_count;
  data->mcu_row_starts = mcu_row_starts;

  // init jpeg
  rewind(f);
  data->cinfo.err = jpeg_std_error(&data->jerr);
  jpeg_create_decompress(&data->cinfo);
  _ws_jpeg_fancy_src(&data->cinfo, f,
		     data->mcu_row_starts[0], -1);

  // extract comment
  jpeg_save_markers(&data->cinfo, JPEG_COM, 0xFFFF);
  jpeg_read_header(&data->cinfo, FALSE);
  if (data->cinfo.marker_list) {
    // copy everything out
    char *com = g_strndup(data->cinfo.marker_list->data,
			  data->cinfo.marker_list->data_length);
    // but only really save everything up to the first '\0'
    data->comment = g_strdup(com);
    g_free(com);
  }
  jpeg_save_markers(&data->cinfo, JPEG_COM, 0);  // stop saving


  // all JPEG can be downsampled to 1/1, 1/2, 1/4, 1/8
  wsd->layer_count = 4;
  wsd->layers = g_new(uint32_t, wsd->layer_count);

  // save dimensions
  for (int i = 0; i < 4; i++) {
    wsd->layers[i] = i;

    data->cinfo.scale_denom = 1 << i;
    jpeg_calc_output_dimensions(&data->cinfo);
    data->widths[i] = data->cinfo.output_width;
    data->heights[i] = data->cinfo.output_height;
  }

  // quiesce jpeg
  jpeg_abort_decompress(&data->cinfo);

  // set ops
  wsd->ops = &_ws_jpeg_ops;
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
  int64_t header_length;
  int64_t start_position;
};

#define INPUT_BUF_SIZE  4096    /* choose an efficiently fread'able size */

static void init_source (j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;
  src->start_of_file = true;
  src->next_restart_marker = 0;
}

static boolean fill_input_buffer (j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;
  size_t nbytes;

  off_t pos = ftello(src->infile);

  bool in_header = false;
  size_t bytes_to_read = INPUT_BUF_SIZE;
  if (pos < src->header_length) {
    // don't read past the header
    bytes_to_read = MIN(src->header_length - pos, bytes_to_read);
    in_header = true;
  } else if (pos == src->header_length) {
    // skip to the jump point
    fseeko(src->infile, src->start_position, SEEK_SET);
  }

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
  } else if (!in_header && src->header_length != -1) {
    // rewrite the restart markers if we know for sure we are not in the header
    bool last_was_ff = false;

    for (size_t i = 0; i < nbytes; i++) {
      uint8_t b = src->buffer[i];
      if (last_was_ff && b >= 0xD0 && b < 0xD8) {
	src->buffer[i] = 0xD0 | src->next_restart_marker;
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
			 int64_t header_length,
			 int64_t start_position) {
  struct my_src_mgr *src;

  if (cinfo->src == NULL) {     /* first time for this JPEG object? */
    cinfo->src = (struct jpeg_source_mgr *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  sizeof(struct my_src_mgr));
    src = (struct my_src_mgr *) cinfo->src;
    src->buffer = (JOCTET *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  INPUT_BUF_SIZE * sizeof(JOCTET));
  }

  src = (struct my_src_mgr *) cinfo->src;
  src->pub.init_source = init_source;
  src->pub.fill_input_buffer = fill_input_buffer;
  src->pub.skip_input_data = skip_input_data;
  src->pub.resync_to_restart = jpeg_resync_to_restart; /* use default method */
  src->pub.term_source = term_source;
  src->infile = infile;
  src->header_length = header_length;
  src->start_position = start_position;
  src->pub.bytes_in_buffer = 0; /* forces fill_input_buffer on first read */
  src->pub.next_input_byte = NULL; /* until buffer loaded */
}
