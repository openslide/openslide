/*
 * Part of this file is:
 *
 * Copyright (C) 1994-1996, Thomas G. Lane.
 * This file is part of the Independent JPEG Group's software.
 * For conditions of distribution and use, see the accompanying README file.
 */

#include <glib.h>
#include <jpeglib.h>

#include "wholeslide-private.h"

struct _ws_jpegopsdata {
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  FILE *f;

  uint64_t *mcu_row_starts;
};

static void read_region(wholeslide_t *wsd, uint32_t *dest,
			uint32_t x, uint32_t y,
			uint32_t layer,
			uint32_t w, uint32_t h) {
  struct _ws_jpegopsdata *jpegopsdata = wsd->data;

  double downsample = ws_get_layer_downsample(wsd, layer);
  uint32_t ds_x = x / downsample;
  uint32_t ds_y = y / downsample;

  // select layer
  TIFFSetDirectory(tiff, wsd->layers[layer]);

  // allocate space for 1 tile
  uint32_t tw, th;
  TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tw);
  TIFFGetField(tiff, TIFFTAG_TILELENGTH, &th);
  uint32_t *tile = g_slice_alloc(tw * th * sizeof(uint32_t));

  // figure out range of tiles
  uint32_t start_x, start_y, end_x, end_y;

  // add in overlaps
  add_in_overlaps(wsd, layer, tw, th, ds_x, ds_y, &start_x, &start_y);
  add_in_overlaps(wsd, layer, tw, th, ds_x + w, ds_y + h,
		  &end_x, &end_y);

  // check bounds
  uint32_t raw_w, raw_h;
  TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &raw_w);
  TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &raw_h);

  if (end_x >= raw_w) {
    end_x = raw_w - 1;
  }
  if (end_y >= raw_h) {
    end_y = raw_h - 1;
  }

  //printf("from (%d,%d) to (%d,%d)\n", start_x, start_y, end_x, end_y);


  // for each tile, draw it where it should go
  uint32_t ovr_x, ovr_y;
  get_overlaps(wsd, layer, &ovr_x, &ovr_y);

  uint32_t src_y = start_y;
  uint32_t dst_y = 0;

  uint32_t num_tiles_decoded = 0;

  while (src_y < ((end_y / th) + 1) * th) {
    uint32_t src_x = start_x;
    uint32_t dst_x = 0;

    while (src_x < ((end_x / tw) + 1) * tw) {
      uint32_t round_x = (src_x / tw) * tw;
      uint32_t round_y = (src_y / th) * th;
      uint32_t off_x = src_x - round_x;
      uint32_t off_y = src_y - round_y;

      //      printf("going to readRGBA @ %d,%d\n", round_x, round_y);
      //      printf(" offset: %d,%d\n", off_x, off_y);
      TIFFReadRGBATile(tiff, round_x, round_y, tile);
      copy_rgba_tile(tile, dest, tw, th, dst_x - off_x, dst_y - off_y, w, h);
      num_tiles_decoded++;

      src_x += tw;
      dst_x += tw - ovr_x;
    }

    src_y += th;
    dst_y += th - ovr_y;
  }

  printf("tiles decoded: %d\n", num_tiles_decoded);

  g_slice_free1(tw * th * sizeof(uint32_t), tile);
}


static void destroy(wholeslide_t *wsd) {
  struct _ws_tiffopsdata *tiffopsdata = wsd->data;

  TIFFClose(tiffopsdata->tiff);
  g_free(tiffopsdata->overlaps);
  g_slice_free(struct _ws_tiffopsdata, tiffopsdata);
}

static void get_dimensions(wholeslide_t *wsd, uint32_t layer,
			   uint32_t *w, uint32_t *h) {
  struct _ws_tiffopsdata *tiffopsdata = wsd->data;
  TIFF *tiff = tiffopsdata->tiff;

  // check bounds
  if (layer >= wsd->layer_count) {
    *w = 0;
    *h = 0;
    return;
  }

  // get the layer
  TIFFSetDirectory(tiff, wsd->layers[layer]);

  // figure out tile size
  uint32_t tw, th;
  TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tw);
  TIFFGetField(tiff, TIFFTAG_TILELENGTH, &th);

  // get image size
  uint32_t iw, ih;
  TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &iw);
  TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &ih);

  // get num tiles
  uint32_t tx = iw / tw;
  uint32_t ty = ih / th;

  // overlaps information seems to only make sense when dealing
  // with images that are divided perfectly by tiles ?
  // thus, we have these if-else below

  // subtract overlaps and compute
  uint32_t overlap_x, overlap_y;
  get_overlaps(wsd, layer, &overlap_x, &overlap_y);

  if (overlap_x) {
    *w = (tx * tw) - overlap_x * (tx - 1);
  } else {
    *w = iw;
  }

  if (overlap_y) {
    *h = (ty * th) - overlap_y * (ty - 1);
  } else {
    *h = ih;
  }

  //  printf("layer %d: tile(%dx%d), image(%dx%d), tilecount(%dx%d)\n\n",
  //	 layer,
  //	 tw, th, iw, ih, tx, ty);
}

static const char* get_comment(wholeslide_t *wsd) {
  struct _ws_tiffopsdata *tiffopsdata = wsd->data;

  char *comment;
  TIFFGetField(tiffopsdata->tiff, TIFFTAG_IMAGEDESCRIPTION, &comment);
  return comment;
}

static struct _wholeslide_ops _ws_tiff_ops = {
  .read_region = read_region,
  .destroy = destroy,
  .get_dimensions = get_dimensions,
  .get_comment = get_comment,
};

void _ws_add_tiff_ops(wholeslide_t *wsd,
		      TIFF *tiff,
		      uint32_t overlap_count,
		      uint32_t *overlaps) {
  g_assert(wsd->data == NULL);

  // allocate private data
  struct _ws_tiffopsdata *data =  g_slice_new(struct _ws_tiffopsdata);

  // populate private data
  data->tiff = tiff;
  data->overlap_count = overlap_count;
  data->overlaps = overlaps;

  // store tiff-specific data into wsd
  wsd->data = data;
  wsd->ops = &_ws_tiff_ops;
}



/*
 * Source manager for doing fancy things with libjpeg and restart markers,
 * initially copied from jdatasrc.c from IJG libjpeg.
 */
struct my_source_mgr {
  struct jpeg_source_mgr pub;   /* public fields */

  FILE *infile;                 /* source stream */
  JOCTET *buffer;               /* start of buffer */
  bool start_of_file;
  uint8_t next_restart_marker;
  uint64_t header_length;
  uint64_t start_position;
};

#define INPUT_BUF_SIZE  4096    /* choose an efficiently fread'able size */

static void init_source (j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;
  src->start_of_file = true;
  src->next_restart_marker = 0;
}

static bool fill_input_buffer (j_decompress_ptr cinfo) {
  struct my_src_mgr *src = (struct my_src_mgr *) cinfo->src;
  size_t nbytes;

  off_t pos = ftello(src->infile);


  bool in_header = false;
  size_t bytes_to_read = INPUT_BUF_SIZE;
  if (pos < header_length) {
    // don't read past the header
    bytes_to_read = min(header_length - pos, bytes_to_read);
    in_header = true;
  } else if (pos == header_length) {
    // skip to the jump point
    fseeko(src->infile, src->start_position, SEEK_SET);
  }

  nbytes = fread(src->infile, 1, src->buffer, bytes_to_read);

  if (nbytes <= 0) {
    if (src->start_of_file) {
      ERREXIT(cinfo, JERR_INPUT_EMPTY);
    }
    WARNMS(cinfo, JWRN_JPEG_EOF);

    /* Insert a fake EOI marker */
    src->buffer[0] = (JOCTET) 0xFF;
    src->buffer[1] = (JOCTET) JPEG_EOI;
    nbytes = 2;
  } else if (!in_header) {
    // rewrite the restart markers
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
    }
  }

  src->pub.next_input_byte = src->buffer;
  src->pub.bytes_in_buffer = nbytes;
  src->start_of_file = false;

  return true;
}


static void skip_input_data (j_decompress_ptr cinfo, long num_bytes) {
  my_src_ptr src = (my_src_ptr) cinfo->src;

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

void _ws_jpeg_fancy_src (j_decompress_ptr cinfo, FILE *infile,
			 uint64_t header_length,
			 uint64_t start_position) {
{
  struct my_str_mgr *src;

  if (cinfo->src == NULL) {     /* first time for this JPEG object? */
    cinfo->src = (struct jpeg_source_mgr *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  SIZEOF(struct my_source_mgr));
    src = (struct my_src_mgr *) cinfo->src;
    src->buffer = (JOCTET *)
      (*cinfo->mem->alloc_small) ((j_common_ptr) cinfo, JPOOL_PERMANENT,
				  INPUT_BUF_SIZE * SIZEOF(JOCTET));
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
