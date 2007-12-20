#include <glib.h>
#include <tiffio.h>

#include "wholeslide-private.h"

struct _ws_tiffdata {
  TIFF *tiff;

  uint32_t overlap_count;
  uint32_t *overlaps;
};


static void get_overlaps(wholeslide_t *wsd, uint32_t layer,
			 uint32_t *x, uint32_t *y) {
  struct _ws_tiffdata *tiffdata = wsd->data;

  if (tiffdata->overlap_count >= 2 * (layer + 1)) {
    *x = tiffdata->overlaps[2 * layer + 0];
    *y = tiffdata->overlaps[2 * layer + 1];
  } else {
    *x = 0;
    *y = 0;
  }
}

static void add_in_overlaps(wholeslide_t *wsd,
			    uint32_t layer,
			    uint32_t tw, uint32_t th,
			    uint32_t x, uint32_t y,
			    uint32_t *out_x, uint32_t *out_y) {
  uint32_t ox, oy;
  get_overlaps(wsd, layer, &ox, &oy);
  *out_x = x + (x / (tw - ox)) * ox;
  *out_y = y + (y / (th - oy)) * oy;
}


static void copy_rgba_tile(uint32_t *tile,
			   uint32_t *dest,
			   uint32_t src_w, uint32_t src_h,
			   int32_t dest_origin_x, int32_t dest_origin_y,
			   uint32_t dest_w, uint32_t dest_h) {
  uint32_t src_origin_y;
  if (dest_origin_y < 0) {  // off the top
    src_origin_y = src_h - 1 + dest_origin_y;
  } else {
    src_origin_y = src_h - 1;
  }

  //  printf("src_origin_y: %d, dest_origin_y: %d\n", src_origin_y, dest_origin_y);

  uint32_t src_origin_x;
  if (dest_origin_x < 0) {  // off the left
    src_origin_x = -dest_origin_x;
  } else {
    src_origin_x = 0;
  }

  //  printf("src_origin_x: %d, dest_origin_x: %d\n", src_origin_x, dest_origin_x);

  //  printf("\n");

  for (uint32_t src_y = src_origin_y; (int32_t) src_y >= 0; src_y--) {
    int32_t dest_y = dest_origin_y + (src_h - 1) - src_y;  // inverted y
    //    printf("src_y: %d, dest_y: %d\n", src_y, dest_y);
    if (dest_y >= dest_h) {
      break;
    }

    for (uint32_t src_x = src_origin_x; src_x < src_w; src_x++) {
      int32_t dest_x = dest_origin_x + src_x;
      if (dest_x >= dest_w) {
	break;
      }

      uint32_t dest_i = dest_y * dest_w + dest_x;
      uint32_t i = src_y * src_w + src_x;

      //      printf("%d %d -> %d %d\n", src_x, src_y, dest_x, dest_y);

      if (TIFFGetA(tile[i])) {
	dest[dest_i] = TIFFGetA(tile[i]) << 24 | TIFFGetR(tile[i]) << 16
	  | TIFFGetG(tile[i]) << 8 | TIFFGetB(tile[i]);
      }
    }
  }
}


static void read_region(wholeslide_t *wsd, uint32_t *dest,
			uint32_t x, uint32_t y,
			uint32_t layer,
			uint32_t w, uint32_t h) {
  struct _ws_tiffdata *tiffdata = wsd->data;
  TIFF *tiff = tiffdata->tiff;

  // fill
  //  memset(dest, 0, w * h * sizeof(uint32_t));

  // set directory
  if (layer >= wsd->layer_count) {
    return;
  }

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
  struct _ws_tiffdata *tiffdata = wsd->data;

  TIFFClose(tiffdata->tiff);
  g_free(tiffdata->overlaps);
  g_slice_free(struct _ws_tiffdata, tiffdata);
}

static void get_dimensions(wholeslide_t *wsd, uint32_t layer,
			   uint32_t *w, uint32_t *h) {
  struct _ws_tiffdata *tiffdata = wsd->data;
  TIFF *tiff = tiffdata->tiff;

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
  struct _ws_tiffdata *tiffdata = wsd->data;

  char *comment;
  TIFFGetField(tiffdata->tiff, TIFFTAG_IMAGEDESCRIPTION, &comment);
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
  // allocate private data
  struct _ws_tiffdata *data =  g_slice_new(struct _ws_tiffdata);

  // populate private data
  data->tiff = tiff;
  data->overlap_count = overlap_count;
  data->overlaps = overlaps;

  // store tiff-specific data into wsd
  wsd->data = data;
  wsd->ops = &_ws_tiff_ops;
}
