#include "config.h"

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

static void read_region(wholeslide_t *wsd, uint32_t *dest,
			uint32_t x, uint32_t y,
			uint32_t layer,
			uint32_t w, uint32_t h) {
  struct _ws_jp2kopsdata *data = wsd->data;

  OPJ_INT32 tx0;
  OPJ_INT32 ty0;
  OPJ_UINT32 tw;
  OPJ_UINT32 th;
  OPJ_UINT32 ntx;
  OPJ_UINT32 nty;

  rewind(data->f);
  opj_stream_t *stream = opj_stream_create_default_file_stream(data->f, true);
  opj_codec_t *codec = opj_create_decompress(CODEC_JP2);
  opj_dparameters_t parameters;
  opj_image_t *image;

  opj_set_default_decoder_parameters(&parameters);
  parameters.cp_reduce = layer;
  opj_setup_decoder(codec, &parameters);
  opj_read_header(codec, &image,
		  &tx0, &ty0, &tw, &th, &ntx, &nty, stream);
  opj_set_decode_area(codec, x, y, x + (w << layer), y + (h << layer));

  printf("%d %d %d %d %d %d %d %d\n", tx0, ty0, tw, th, ntx, nty, image->numcomps, image->color_space);

  image = opj_decode(codec, stream);
  opj_end_decompress(codec, stream);


  opj_image_comp_t *comps = image->comps;

  for (int i = 0; i < w * h; i++) {
    uint8_t A = 255;
    uint8_t R = comps[0].data[i];
    uint8_t G = comps[1].data[i];
    uint8_t B = comps[2].data[i];

    dest[i] = A << 24 | R << 16 | G << 8 | B;
  }

  opj_image_destroy(image);
  opj_destroy_codec(codec);
  opj_stream_destroy(stream);
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
  uint32_t layer_count = 1 + log2(MIN(data->w, data->h));
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
