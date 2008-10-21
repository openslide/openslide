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

#define USE_OPJ_DEPRECATED

#include <stdio.h>
#include <glib.h>
#include <openjpeg.h>
#include <math.h>

#include "wholeslide-private.h"

struct _ws_jp2kopsdata {
  FILE *f;

  uint32_t w;
  uint32_t h;
};

static void destroy_data(struct _ws_jp2kopsdata *data) {
  fclose(data->f);
}

static void destroy(wholeslide_t *wsd) {
  struct _ws_jp2kopsdata *data = wsd->data;
  destroy_data(data);
  g_slice_free(struct _ws_jp2kopsdata, data);
}

static const char *get_comment(wholeslide_t *wsd) {
  return "";      // TODO
}

static void get_dimensions(wholeslide_t *wsd, uint32_t layer,
			   uint32_t *w, uint32_t *h) {
  struct _ws_jp2kopsdata *data = wsd->data;

  // check bounds
  if (layer >= wsd->layer_count) {
    *w = 0;
    *h = 0;
    return;
  }

  // shift
  *w = data->w >> layer;
  *h = data->h >> layer;
}

static void info_callback(const OPJ_CHAR *msg, void *data) {
  g_message("%s", msg);
}
static void warning_callback(const OPJ_CHAR *msg, void *data) {
  g_warning("%s", msg);
}
static void error_callback(const OPJ_CHAR *msg, void *data) {
  g_error("%s", msg);
}

static void read_region(wholeslide_t *wsd, uint32_t *dest,
			uint32_t x, uint32_t y,
			uint32_t layer,
			uint32_t w, uint32_t h) {
  struct _ws_jp2kopsdata *data = wsd->data;

  printf("read_region: (%d,%d) layer: %d, size: (%d,%d)\n",
	 x, y, layer, w, h);

  OPJ_INT32 tx0;
  OPJ_INT32 ty0;
  OPJ_INT32 tx1;
  OPJ_INT32 ty1;
  OPJ_UINT32 tw;
  OPJ_UINT32 th;
  OPJ_UINT32 ntx;
  OPJ_UINT32 nty;

  opj_codec_t *codec = opj_create_decompress(CODEC_JP2);
  opj_set_info_handler(codec, info_callback, NULL);
  opj_set_warning_handler(codec, warning_callback, NULL);
  opj_set_error_handler(codec, error_callback, NULL);

  rewind(data->f);
  opj_stream_t *stream = opj_stream_create_default_file_stream(data->f, true);

  opj_dparameters_t parameters;
  opj_image_t *image;

  opj_read_header(codec, &image,
		  &tx0, &ty0, &tw, &th, &ntx, &nty, stream);
  opj_set_default_decoder_parameters(&parameters);
  parameters.cp_reduce = layer;
  opj_setup_decoder(codec, &parameters);

  int32_t ddx1 = x + w;
  int32_t ddy1 = y + h;
  printf("want to set decode area to (%d,%d), (%d,%d)\n",
	 x, y, ddx1, ddy1);

  opj_set_decode_area(codec, x, y, ddx1, ddy1);

  printf("%d %d %d %d %d %d %d %d\n", tx0, ty0, tw, th, ntx, nty, image->numcomps, image->color_space);

  OPJ_UINT32 tile_index;
  OPJ_UINT32 data_size;
  OPJ_UINT32 nb_comps;
  bool should_go_on = true;

  //  while(true) {
    g_debug("reading tile header");
    g_assert(opj_read_tile_header(codec,
				  &tile_index,
				  &data_size,
				  &tx0, &ty0, &tx1, &ty1,
				  &nb_comps, &should_go_on,
				  stream));
    printf("tile_index: %d, data_size: %d, (%d,%d),(%d,%d), comps: %d, go_on: %d\n", tile_index, data_size, tx0, ty0, tx1, ty1, nb_comps, should_go_on);

    printf("data_size: %d\n", data_size);

    if (!should_go_on) {
      //  break;
    }

    OPJ_BYTE *img_data = g_slice_alloc(data_size);
    g_assert(opj_decode_tile_data(codec, tile_index,
				  img_data,
				  data_size,
				  stream));

    // copy
    for (y = 0; y < h; y++) {
      for (x = 0; x < w; x++) {
	int i = y * w + x;

	uint8_t A = 255;
	uint8_t R = img_data[i];
	uint8_t G = img_data[i + data_size / 3];
	uint8_t B = img_data[i + 2 * (data_size / 3)];

	dest[i] = A << 24 | R << 16 | G << 8 | B;
      }
    }

    g_slice_free1(data_size, img_data);

    //  }

  opj_end_decompress(codec, stream);
  opj_image_destroy(image);
  opj_stream_destroy(stream);
  opj_destroy_codec(codec);
}



static struct _wholeslide_ops _ws_jp2k_ops = {
  .read_region = read_region,
  .destroy = destroy,
  .get_dimensions = get_dimensions,
  .get_comment = get_comment,
};

void _ws_add_jp2k_ops(wholeslide_t *wsd,
		      FILE *f,
		      uint32_t w, uint32_t h) {
  // allocate private data
  struct _ws_jp2kopsdata *data = g_slice_new(struct _ws_jp2kopsdata);

  data->f = f;
  data->w = w;
  data->h = h;

  // compute layer info
  uint32_t layer_count = log2(MIN(data->w, data->h));
  printf("layer_count: %d\n", layer_count);

  if (wsd == NULL) {
    // free now and return
    destroy_data(data);
    return;
  }

  g_assert(wsd->data == NULL);

  wsd->layer_count = layer_count;
  wsd->data = data;
  wsd->ops = &_ws_jp2k_ops;
}
