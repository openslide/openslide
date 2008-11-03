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

#include "config.h"

#include <glib.h>
#include <tiffio.h>

#include "wholeslide-private.h"

struct _ws_tiffopsdata {
  TIFF *tiff;

  int32_t overlap_count;
  int32_t *overlaps;
  int32_t *layers;

  struct _ws_tiff_tilereader *(*tilereader_create)(TIFF *tiff);
  void (*tilereader_read)(struct _ws_tiff_tilereader *wtt,
			  uint32_t *dest,
			  int64_t x, int64_t y);
  void (*tilereader_destroy)(struct _ws_tiff_tilereader *wtt);
};


static void get_overlaps(wholeslide_t *wsd, int32_t layer,
			 int32_t *x, int32_t *y) {
  struct _ws_tiffopsdata *data = wsd->data;

  if (data->overlap_count >= 2 * (layer + 1)) {
    *x = data->overlaps[2 * layer + 0];
    *y = data->overlaps[2 * layer + 1];
  } else {
    *x = 0;
    *y = 0;
  }
}

static void add_in_overlaps(wholeslide_t *wsd,
			    int32_t layer,
			    int64_t tw, int64_t th,
			    int64_t x, int64_t y,
			    int64_t *out_x, int64_t *out_y) {
  int32_t ox, oy;
  get_overlaps(wsd, layer, &ox, &oy);
  *out_x = x + (x / (tw - ox)) * ox;
  *out_y = y + (y / (th - oy)) * oy;
}


static void copy_rgba_tile(const uint32_t *tile,
			   uint32_t *dest,
			   int64_t src_w, int64_t src_h,
			   int64_t dest_origin_x, int64_t dest_origin_y,
			   int64_t dest_w, int64_t dest_h) {
  int64_t src_origin_y;
  if (dest_origin_y < 0) {  // off the top
    src_origin_y = -dest_origin_y;
  } else {
    src_origin_y = 0;
  }

  //  g_debug("src_origin_y: %d, dest_origin_y: %d", src_origin_y, dest_origin_y);

  int64_t src_origin_x;
  if (dest_origin_x < 0) {  // off the left
    src_origin_x = -dest_origin_x;
  } else {
    src_origin_x = 0;
  }

  //  g_debug("src_origin_x: %d, dest_origin_x: %d", src_origin_x, dest_origin_x);

  //  g_debug("");

  for (int64_t src_y = src_origin_y; src_y < src_h; src_y++) {
    int64_t dest_y = dest_origin_y + src_y;
    //    g_debug("src_y: %d, dest_y: %d", src_y, dest_y);
    if (dest_y < dest_h) {
      for (int64_t src_x = src_origin_x; src_x < src_w; src_x++) {
	int64_t dest_x = dest_origin_x + src_x;
	if (dest_x < dest_w) {
	  int64_t dest_i = dest_y * dest_w + dest_x;
	  int64_t i = src_y * src_w + src_x;

	  //      g_debug("%d %d -> %d %d", src_x, src_y, dest_x, dest_y);
	  uint32_t tile_val = tile[i];
	  dest[dest_i] = (tile_val & 0xFF00FF00)
	    | ((tile_val << 16) & 0xFF0000)
	    | ((tile_val >> 16) & 0xFF);
	}
      }
    }
  }
}


static void read_region(wholeslide_t *wsd, uint32_t *dest,
			int64_t x, int64_t y,
			int32_t layer,
			int64_t w, int64_t h) {
  uint32_t tmp;

  struct _ws_tiffopsdata *data = wsd->data;
  TIFF *tiff = data->tiff;

  double downsample = ws_get_layer_downsample(wsd, layer);
  int64_t ds_x = x / downsample;
  int64_t ds_y = y / downsample;

  // select layer
  TIFFSetDirectory(tiff, data->layers[layer]);

  // allocate space for 1 tile
  int64_t tw, th;
  TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tmp);
  tw = tmp;
  TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tmp);
  th = tmp;
  uint32_t *tile = g_slice_alloc(tw * th * sizeof(uint32_t));

  // figure out range of tiles
  int64_t start_x, start_y, end_x, end_y;

  // add in overlaps
  add_in_overlaps(wsd, layer, tw, th, ds_x, ds_y, &start_x, &start_y);
  add_in_overlaps(wsd, layer, tw, th, ds_x + w, ds_y + h,
		  &end_x, &end_y);

  // check bounds
  int64_t raw_w, raw_h;
  TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &tmp);
  raw_w = tmp;
  TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &tmp);
  raw_h = tmp;

  if (end_x >= raw_w) {
    end_x = raw_w - 1;
  }
  if (end_y >= raw_h) {
    end_y = raw_h - 1;
  }

  //g_debug("from (%d,%d) to (%d,%d)", start_x, start_y, end_x, end_y);


  // for each tile, draw it where it should go
  int32_t ovr_x, ovr_y;
  get_overlaps(wsd, layer, &ovr_x, &ovr_y);

  int64_t src_y = start_y;
  int64_t dst_y = 0;

  int num_tiles_decoded = 0;

  struct _ws_tiff_tilereader *tilereader = data->tilereader_create(tiff);

  while (src_y < ((end_y / th) + 1) * th) {
    int64_t src_x = start_x;
    int64_t dst_x = 0;

    while (src_x < ((end_x / tw) + 1) * tw) {
      int round_x = (src_x / tw) * tw;
      int round_y = (src_y / th) * th;
      int off_x = src_x - round_x;
      int off_y = src_y - round_y;

      //      g_debug("going to readRGBA @ %d,%d", round_x, round_y);
      //      g_debug(" offset: %d,%d", off_x, off_y);
      data->tilereader_read(tilereader, tile, round_x, round_y);
      copy_rgba_tile(tile, dest, tw, th, dst_x - off_x, dst_y - off_y, w, h);
      num_tiles_decoded++;

      src_x += tw;
      dst_x += tw - ovr_x;
    }

    src_y += th;
    dst_y += th - ovr_y;
  }

  //g_debug("tiles decoded: %d", num_tiles_decoded);

  data->tilereader_destroy(tilereader);
  g_slice_free1(tw * th * sizeof(uint32_t), tile);
}


static void destroy_data(struct _ws_tiffopsdata *data) {
  TIFFClose(data->tiff);
  g_free(data->overlaps);
  g_free(data->layers);
}

static void destroy(wholeslide_t *wsd) {
  struct _ws_tiffopsdata *data = wsd->data;
  destroy_data(data);
  g_slice_free(struct _ws_tiffopsdata, data);
}

static void get_dimensions(wholeslide_t *wsd, int32_t layer,
			   int64_t *w, int64_t *h) {
  uint32_t tmp;

  struct _ws_tiffopsdata *data = wsd->data;
  TIFF *tiff = data->tiff;

  // check bounds
  if (layer >= wsd->layer_count) {
    *w = 0;
    *h = 0;
    return;
  }

  // get the layer
  TIFFSetDirectory(tiff, data->layers[layer]);

  // figure out tile size
  int64_t tw, th;
  TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tmp);
  tw = tmp;
  TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tmp);
  th = tmp;

  // get image size
  int64_t iw, ih;
  TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &tmp);
  iw = tmp;
  TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &tmp);
  ih = tmp;

  // get num tiles
  int64_t tx = iw / tw;
  int64_t ty = ih / th;

  // overlaps information seems to only make sense when dealing
  // with images that are divided perfectly by tiles ?
  // thus, we have these if-else below

  // subtract overlaps and compute
  int32_t overlap_x, overlap_y;
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

  //  g_debug("layer %d: tile(%dx%d), image(%dx%d), tilecount(%dx%d)\n",
  //	 layer,
  //	 tw, th, iw, ih, tx, ty);
}

static const char* get_comment(wholeslide_t *wsd) {
  struct _ws_tiffopsdata *data = wsd->data;

  char *comment;
  TIFFGetField(data->tiff, TIFFTAG_IMAGEDESCRIPTION, &comment);
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
		      int32_t overlap_count,
		      int32_t *overlaps,
		      int32_t layer_count,
		      int32_t *layers,
		      struct _ws_tiff_tilereader *(*tilereader_create)(TIFF *tiff),
		      void (*tilereader_read)(struct _ws_tiff_tilereader *wtt,
					      uint32_t *dest,
					      int64_t x, int64_t y),
		      void (*tilereader_destroy)(struct _ws_tiff_tilereader *wtt)) {
  // allocate private data
  struct _ws_tiffopsdata *data =  g_slice_new(struct _ws_tiffopsdata);

  // store layer info
  data->layers = layers;

  // populate private data
  data->tiff = tiff;
  data->overlap_count = overlap_count;
  data->overlaps = overlaps;

  data->tilereader_create = tilereader_create;
  data->tilereader_read = tilereader_read;
  data->tilereader_destroy = tilereader_destroy;

  if (wsd == NULL) {
    // free now and return
    destroy_data(data);
    return;
  }

  // store tiff-specific data into wsd
  g_assert(wsd->data == NULL);

  wsd->layer_count = layer_count;
  wsd->data = data;
  wsd->ops = &_ws_tiff_ops;
}


struct _ws_tiff_tilereader {
  TIFFRGBAImage img;
  int64_t tile_width;
  int64_t tile_height;
};

struct _ws_tiff_tilereader *_ws_generic_tiff_tilereader_create(TIFF *tiff) {
  struct _ws_tiff_tilereader *wtt = g_slice_new(struct _ws_tiff_tilereader);

  char emsg[1024] = "";
  TIFFRGBAImageBegin(&wtt->img, tiff, 0, emsg);
  wtt->img.req_orientation = ORIENTATION_TOPLEFT;

  TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &wtt->tile_width);
  TIFFGetField(tiff, TIFFTAG_TILELENGTH, &wtt->tile_height);

  return wtt;
}

void _ws_generic_tiff_tilereader_read(struct _ws_tiff_tilereader *wtt,
				      uint32_t *dest,
				      int64_t x, int64_t y) {
  wtt->img.col_offset = x;
  wtt->img.row_offset = y;
  TIFFRGBAImageGet(&wtt->img, dest, wtt->tile_width, wtt->tile_height);
}

void _ws_generic_tiff_tilereader_destroy(struct _ws_tiff_tilereader *wtt) {
  g_slice_free(struct _ws_tiff_tilereader, wtt);
}
