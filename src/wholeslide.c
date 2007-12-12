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
  for (unsigned int i = 0; i < wsd->layer_count; i++) {
    wsd->layers[i] = i;
  }

  // get baseline size
  uint32_t blw, blh;
  ws_get_baseline_dimensions(wsd, &blw, &blh);

  // compute downsamples
  printf("downsamples\n");
  for (unsigned int i = 0; i < wsd->layer_count; i++) {
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
  unsigned int i;

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


wholeslide_t *ws_open(const char *filename) {
  // alloc memory
  wholeslide_t *wsd = g_new0(wholeslide_t, 1);

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
  // thus, we have this if-else below

  // compute
  if (wsd->overlap_count >= 2 * (layer + 1)) {
    // subtract overlaps
    *w = (tx * tw) - wsd->overlaps[2 * layer + 0] * (tx - 1);
    *h = (ty * th) - wsd->overlaps[2 * layer + 1] * (ty - 1);
  } else {
    *w = iw;
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
  for (unsigned int i = 1; i < wsd->layer_count; i++) {
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


void ws_read_region(wholeslide_t *wsd,
		    void *dest,
		    uint32_t x, uint32_t y,
		    uint32_t layer,
		    uint32_t w, uint32_t h) {
  // fill with background, for now
  for (unsigned int i = 0; i < w * h * 4; i += 4) {
    ((uint8_t *) dest)[i + 0] = (wsd->background_color >> 24) & 0xFF;
    ((uint8_t *) dest)[i + 1] = (wsd->background_color >> 16) & 0xFF;
    ((uint8_t *) dest)[i + 2] = (wsd->background_color >> 8) & 0xFF;
    ((uint8_t *) dest)[i + 3] = (wsd->background_color >> 0) & 0xFF;
  }
}
