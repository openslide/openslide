#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <tiffio.h>
#include <glib.h>

#include "wholeslide-private.h"

static const char *TRESTLE_SOFTWARE = "MedScan";

static const char *OVERLAPS_XY = "OverlapsXY=";
static const char *OBJECTIVE_POWER = "Objective Power=";
static const char *BACKGROUND_COLOR = "Background Color=";

static void parse_trestle(wholeslide_t *wsd) {
  char *tagval;

  TIFFGetField(wsd->tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);

  char **first_pass = g_strsplit(tagval, ";", -1);
  for (char **cur_str = first_pass; *cur_str != NULL; cur_str++) {
    //fprintf(stderr, " XX: %s\n", *cur_str);
    if (g_str_has_prefix(*cur_str, OVERLAPS_XY)) {
      // found it
      char **second_pass = g_strsplit(*cur_str, " ", -1);

      wsd->overlap_count = g_strv_length(second_pass) - 1; // skip fieldname
      wsd->overlaps = g_new(uint32_t, wsd->overlap_count);

      int i = 0;
      // skip fieldname
      for (char **cur_str2 = second_pass + 1; *cur_str2 != NULL; cur_str2++) {
	wsd->overlaps[i] = atoi(*cur_str2);
	i++;
      }

      g_strfreev(second_pass);
    } else if (g_str_has_prefix(*cur_str, OBJECTIVE_POWER)) {
      // found a different one
      wsd->objective_power = g_ascii_strtod(*cur_str + strlen(OBJECTIVE_POWER),
					    NULL);
    } else if (g_str_has_prefix(*cur_str, BACKGROUND_COLOR)) {
      // RGBA
      wsd->background_color = (strtoul(*cur_str + strlen(BACKGROUND_COLOR),
				       NULL, 16) << 8) | 0xFF;
    }
  }

  // count layers
  do {
    wsd->layer_count++;
  } while (TIFFReadDirectory(wsd->tiff));
  wsd->layers = g_new(uint32_t, wsd->layer_count);
  wsd->downsamples = g_new(double, wsd->layer_count);

  // directories are linear
  for (uint32_t i = 0; i < wsd->layer_count; i++) {
    wsd->layers[i] = i;
  }

  // get baseline size
  uint32_t blw, blh;
  ws_get_baseline_dimensions(wsd, &blw, &blh);

  // compute downsamples
  printf("downsamples\n");
  for (uint32_t i = 0; i < wsd->layer_count; i++) {
    uint32_t w, h;
    ws_get_layer_dimensions(wsd, i, &w, &h);

    wsd->downsamples[i] = (double) blh / (double) h;

    printf(" %g\n", wsd->downsamples[i]);
  }
  printf("\n");


  g_strfreev(first_pass);
}



static bool extract_info(wholeslide_t *wsd) {
  // determine vendor
  char *tagval;

  TIFFGetField(wsd->tiff, TIFFTAG_SOFTWARE, &tagval);
  if (!strncmp(TRESTLE_SOFTWARE, tagval, strlen(TRESTLE_SOFTWARE))) {
    // trestle
    // TODO implement and check return values
    parse_trestle(wsd);
  }

  return true;
}

static void print_info(wholeslide_t *wsd) {
  uint32_t i;

  printf("layers: \n");
  for (i = 0; i < wsd->layer_count; i++) {
    printf(" %d\n", wsd->layers[i]);
  }
  printf("\n");

  printf("objective_power: %g\n\n", wsd->objective_power);

  printf("overlaps: \n");
  for (i = 0; i < wsd->overlap_count; i++) {
    printf(" %d\n", wsd->overlaps[i]);
  }
  printf("\n");
}



static void get_overlaps(wholeslide_t *wsd, uint32_t layer,
			 uint32_t *x, uint32_t *y) {
  if (wsd->overlap_count >= 2 * (layer + 1)) {
    *x = wsd->overlaps[2 * layer + 0];
    *y = wsd->overlaps[2 * layer + 1];
  } else {
    *x = 0;
    *y = 0;
  }
}



wholeslide_t *ws_open(const char *filename) {
  // alloc memory
  wholeslide_t *wsd = g_slice_new(wholeslide_t);

  // open the file
  wsd->tiff = TIFFOpen(filename, "r");

  extract_info(wsd);

  print_info(wsd);

  // return
  return wsd;
}


void ws_close(wholeslide_t *wsd) {
  TIFFClose(wsd->tiff);

  g_free(wsd->layers);
  g_free(wsd->overlaps);
  g_free(wsd->downsamples);
  g_slice_free(wholeslide_t, wsd);
}


void ws_get_baseline_dimensions(wholeslide_t *wsd,
				uint32_t *w, uint32_t *h) {
  ws_get_layer_dimensions(wsd, 0, w, h);
}

void ws_get_layer_dimensions(wholeslide_t *wsd, uint32_t layer,
			     uint32_t *w, uint32_t *h) {
  // check bounds
  if (layer >= wsd->layer_count) {
    *w = 0;
    *h = 0;
    return;
  }

  // get the layer
  TIFFSetDirectory(wsd->tiff, wsd->layers[layer]);

  // figure out tile size
  uint32_t tw, th;
  TIFFGetField(wsd->tiff, TIFFTAG_TILEWIDTH, &tw);
  TIFFGetField(wsd->tiff, TIFFTAG_TILELENGTH, &th);

  // get image size
  uint32_t iw, ih;
  TIFFGetField(wsd->tiff, TIFFTAG_IMAGEWIDTH, &iw);
  TIFFGetField(wsd->tiff, TIFFTAG_IMAGELENGTH, &ih);

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

  printf("layer %d: tile(%dx%d), image(%dx%d), tilecount(%dx%d)\n\n",
	 layer,
	 tw, th, iw, ih, tx, ty);
}

const char *ws_get_comment(wholeslide_t *wsd) {
  char *comment;
  TIFFGetField(wsd->tiff, TIFFTAG_IMAGEDESCRIPTION, &comment);
  return comment;
}


uint32_t ws_get_layer_count(wholeslide_t *wsd) {
  return wsd->layer_count;
}


uint32_t ws_get_best_layer_for_downsample(wholeslide_t *wsd,
					  double downsample) {
  // too small, return first
  if (downsample < wsd->downsamples[0]) {
    return 0;
  }

  // find where we are in the middle
  for (uint32_t i = 1; i < wsd->layer_count; i++) {
    if (downsample < wsd->downsamples[i]) {
      return i - 1;
    }
  }

  // too big, return last
  return wsd->layer_count - 1;
}


double ws_get_layer_downsample(wholeslide_t *wsd, uint32_t layer) {
  if (layer > wsd->layer_count) {
    return 0.0;
  }

  return wsd->downsamples[layer];
}


uint32_t ws_give_prefetch_hint(wholeslide_t *wsd,
			       uint32_t x, uint32_t y,
			       uint32_t layer,
			       uint32_t w, uint32_t h) {
  // TODO
  return 0;
}

void ws_cancel_prefetch_hint(wholeslide_t *wsd, uint32_t prefetch_id) {
  // TODO
  return;
}


size_t ws_get_region_num_bytes(wholeslide_t *wsd,
			       uint32_t w, uint32_t h) {
  return w * h * 4;
}


static void copy_rgba_tile(uint32_t *tile,
			   uint8_t *dest,
			   uint32_t src_w, uint32_t src_h,
			   int32_t dest_origin_x, int32_t dest_origin_y,
			   uint32_t dest_w, uint32_t dest_h) {
  uint32_t src_origin_y;
  if (dest_origin_y < 0) {  // off the top
    src_origin_y = src_h - 1 + dest_origin_y;
  } else {
    src_origin_y = src_h - 1;
  }

  printf("src_origin_y: %d, dest_origin_y: %d\n",
	 src_origin_y, dest_origin_y);

  uint32_t src_origin_x;
  if (dest_origin_x < 0) {  // off the left
    src_origin_x = -dest_origin_x;
  } else {
    src_origin_x = 0;
  }

  printf("src_origin_x: %d, dest_origin_x: %d\n",
	 src_origin_x, dest_origin_x);


  for (uint32_t src_y = src_origin_y; (int32_t) src_y >= 0; src_y--) {
    int32_t dest_y = dest_origin_y + src_h - src_y;  // inverted y
    //    printf("src_y: %d, dest_y: %d\n", src_y, dest_y);
    if (dest_y >= dest_h) {
      break;
    }

    for (uint32_t src_x = src_origin_x; src_x < src_w; src_x++) {
      int32_t dest_x = dest_origin_x + src_x;
      if (dest_x >= dest_w) {
	break;
      }

      uint32_t dest_i = (dest_y * dest_w + dest_x) * 4;
      uint32_t i = src_y * src_w + src_x;

      //      printf("%d %d -> %d %d\n", x, y, dest_x, dest_y);

      if (TIFFGetA(tile[i])) {
	  dest[dest_i + 0] = TIFFGetR(tile[i]);
	  dest[dest_i + 1] = TIFFGetG(tile[i]);
	  dest[dest_i + 2] = TIFFGetB(tile[i]);
	  dest[dest_i + 3] = TIFFGetA(tile[i]);
      }
    }
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


void ws_read_region(wholeslide_t *wsd,
		    void *dest,
		    uint32_t x, uint32_t y,
		    uint32_t layer,
		    uint32_t w, uint32_t h) {
  // fill with background, for now
  for (uint32_t i = 0; i < w * h * 4; i += 4) {
    ((uint8_t *) dest)[i + 0] = (wsd->background_color >> 24) & 0xFF;
    ((uint8_t *) dest)[i + 1] = (wsd->background_color >> 16) & 0xFF;
    ((uint8_t *) dest)[i + 2] = (wsd->background_color >> 8) & 0xFF;
    ((uint8_t *) dest)[i + 3] = (wsd->background_color >> 0) & 0xFF;
  }

  // set directory
  if (layer >= wsd->layer_count) {
    return;
  }

  double downsample = ws_get_layer_downsample(wsd, layer);
  uint32_t ds_x = x / downsample;
  uint32_t ds_y = y / downsample;

  // select layer
  TIFFSetDirectory(wsd->tiff, wsd->layers[layer]);

  // allocate space for 1 tile
  uint32_t tw, th;
  TIFFGetField(wsd->tiff, TIFFTAG_TILEWIDTH, &tw);
  TIFFGetField(wsd->tiff, TIFFTAG_TILELENGTH, &th);
  uint32_t *tile = g_slice_alloc(tw * th * sizeof(uint32_t));

  // figure out range of tiles
  uint32_t start_x, start_y, end_x, end_y;

  // add in overlaps
  add_in_overlaps(wsd, layer, tw, th, ds_x, ds_y, &start_x, &start_y);
  add_in_overlaps(wsd, layer, tw, th, ds_x + w, ds_y + h,
		  &end_x, &end_y);

  // check bounds
  uint32_t raw_w, raw_h;
  TIFFGetField(wsd->tiff, TIFFTAG_IMAGEWIDTH, &raw_w);
  TIFFGetField(wsd->tiff, TIFFTAG_IMAGELENGTH, &raw_h);

  if (end_x >= raw_w) {
    end_x = raw_w - 1;
  }
  if (end_y >= raw_h) {
    end_y = raw_h - 1;
  }

  printf("from (%d,%d) to (%d,%d)\n", start_x, start_y, end_x, end_y);


  // for each tile, draw it where it should go
  uint32_t ovr_x, ovr_y;
  get_overlaps(wsd, layer, &ovr_x, &ovr_y);

  uint32_t src_y = start_y;
  uint32_t dst_y = 0;

  while (src_y < ((end_y / th) + 1) * th) {
    uint32_t src_x = start_x;
    uint32_t dst_x = 0;

    while (src_x < ((end_x / tw) + 1) * tw) {
      uint32_t round_x = (src_x / tw) * tw;
      uint32_t round_y = (src_y / th) * th;
      uint32_t off_x = src_x - round_x;
      uint32_t off_y = src_y - round_y;

      printf("going to readRGBA @ %d,%d\n", round_x, round_y);
      TIFFReadRGBATile(wsd->tiff, round_x, round_y, tile);
      copy_rgba_tile(tile, dest, tw, th, dst_x - off_x, dst_y - off_y, w, h);

      src_x += tw;
      dst_x += tw - ovr_x;
    }

    src_y += th;
    dst_y += th - ovr_y;
  }

  g_slice_free1(tw * th * sizeof(uint32_t), tile);
}
