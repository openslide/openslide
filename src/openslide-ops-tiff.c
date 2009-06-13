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

#include "config.h"

#include <glib.h>
#include <tiffio.h>
#include <inttypes.h>

#include "openslide-private.h"
#include "openslide-tilehelper.h"

struct _openslide_tiffopsdata {
  TIFF *tiff;

  int32_t overlap_count;
  int32_t *overlaps;
  int32_t *layers;

  _openslide_tiff_tilereader_read_fn tileread;
};


static void add_in_overlaps(openslide_t *osr,
			    int32_t layer,
			    int64_t tw, int64_t th,
			    int64_t total_tiles_across,
			    int64_t total_tiles_down,
			    int64_t x, int64_t y,
			    int64_t *out_x, int64_t *out_y) {
  struct _openslide_tiffopsdata *data = osr->data;

  if (layer >= data->overlap_count) {
    *out_x = x;
    *out_y = y;
    return;
  }

  int32_t ox = data->overlaps[layer * 2];
  int32_t oy = data->overlaps[(layer * 2) + 1];

  // the last tile doesn't have an overlap to skip
  int64_t max_skip_x = (total_tiles_across - 1) * ox;
  int64_t max_skip_y = (total_tiles_down - 1) * oy;

  int64_t skip_x = (x / (tw - ox)) * ox;
  int64_t skip_y = (y / (th - oy)) * oy;

  *out_x = x + MIN(max_skip_x, skip_x);
  *out_y = y + MIN(max_skip_y, skip_y);
}


static void store_string_property(TIFF *tiff, GHashTable *ht,
				  const char *name, ttag_t tag) {
  char *value;
  if (TIFFGetField(tiff, tag, &value)) {
    g_hash_table_insert(ht, g_strdup(name), g_strdup(value));
  }
}

static void store_float_property(TIFF *tiff, GHashTable *ht,
				  const char *name, ttag_t tag) {
  float value;
  if (TIFFGetField(tiff, tag, &value)) {
    g_hash_table_insert(ht, g_strdup(name), g_strdup_printf("%g", value));
  }
}

static void store_properties(TIFF *tiff, GHashTable *ht) {
  // strings
  store_string_property(tiff, ht, _OPENSLIDE_COMMENT_NAME, TIFFTAG_IMAGEDESCRIPTION);
  store_string_property(tiff, ht, "tiff.ImageDescription", TIFFTAG_IMAGEDESCRIPTION);
  store_string_property(tiff, ht, "tiff.Make", TIFFTAG_MAKE);
  store_string_property(tiff, ht, "tiff.Model", TIFFTAG_MODEL);
  store_string_property(tiff, ht, "tiff.Software", TIFFTAG_SOFTWARE);
  store_string_property(tiff, ht, "tiff.DateTime", TIFFTAG_DATETIME);
  store_string_property(tiff, ht, "tiff.Artist", TIFFTAG_ARTIST);
  store_string_property(tiff, ht, "tiff.HostComputer", TIFFTAG_HOSTCOMPUTER);
  store_string_property(tiff, ht, "tiff.Copyright", TIFFTAG_COPYRIGHT);

  // floats
  store_float_property(tiff, ht, "tiff.XResolution", TIFFTAG_XRESOLUTION);
  store_float_property(tiff, ht, "tiff.YResolution", TIFFTAG_YRESOLUTION);

  // special
  uint16_t resolution_unit;
  if (TIFFGetField(tiff, TIFFTAG_RESOLUTIONUNIT, &resolution_unit)) {
    const char *result;

    switch(resolution_unit) {
    case 1:
      result = "none";
      break;
    case 2:
      result = "inch";
      break;
    case 3:
      result = "centimeter";
      break;
    default:
      result = "unknown";
    }

    g_hash_table_insert(ht, g_strdup("tiff.ResolutionUnit"), g_strdup(result));
  }
}

static void destroy_data(struct _openslide_tiffopsdata *data) {
  TIFFClose(data->tiff);
  g_free(data->layers);
  g_free(data->overlaps);
}

static void destroy(openslide_t *osr) {
  struct _openslide_tiffopsdata *data = osr->data;
  destroy_data(data);
  g_slice_free(struct _openslide_tiffopsdata, data);
}


static void get_dimensions(openslide_t *osr, int32_t layer,
			   int64_t *tiles_across, int64_t *tiles_down,
			   int32_t *tile_width, int32_t *tile_height,
			   int32_t *last_tile_width, int32_t *last_tile_height) {

  uint32_t tmp;

  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = data->tiff;

  // init all
  *tiles_across = 0;
  *tiles_down = 0;
  *tile_width = 0;
  *tile_height = 0;
  *last_tile_width = 0;
  *last_tile_height = 0;

  int32_t ox = 0;
  int32_t oy = 0;
  if (layer < data->overlap_count) {
    ox = data->overlaps[layer * 2];
    oy = data->overlaps[(layer * 2) + 1];
  }

  // get the layer
  g_return_if_fail(TIFFSetDirectory(tiff, data->layers[layer]));

  // figure out tile size
  int64_t tw, th;
  g_return_if_fail(TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tmp));
  tw = tmp;
  g_return_if_fail(TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tmp));
  th = tmp;

  // get image size
  int64_t iw, ih;
  g_return_if_fail(TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &tmp));
  iw = tmp;
  g_return_if_fail(TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &tmp));
  ih = tmp;

  // num tiles in each dimension
  *tiles_across = (iw / tw) + !!(iw % tw);   // integer ceiling
  *tiles_down = (ih / th) + !!(ih % th);

  // subtract out the overlaps (there are tiles-1 overlaps in each dimension)
  int64_t iw_minus_o = iw;
  int64_t ih_minus_o = ih;
  if (iw >= tw) {
    iw_minus_o -= (*tiles_across - 1) * ox;
  }
  if (ih >= th) {
    ih_minus_o -= (*tiles_down - 1) * oy;
  }

  // the returned tile size is the raw tile size minus overlap
  *tile_width = tw - ox;
  *tile_height = th - oy;

  // the last tile can be larger or smaller
  //  larger: typical, because there is no overlap
  //  smaller: there is still no overlap, but the image may
  //           be smaller, and the tile has padding (allowed by TIFF)
  *last_tile_width = iw_minus_o - (*tiles_across - 1) * *tile_width;
  *last_tile_height = ih_minus_o - (*tiles_down - 1) * *tile_height;

  /*
  g_debug("TIFF dimensions: ta %" PRId64 ", td %" PRId64 ", tw %d, th %d, ltw %d, ltd %d",
	  *tiles_across, *tiles_down, *tile_width, *tile_height,
	  *last_tile_width, *last_tile_height);
  g_debug(" raw: tw %" PRId64 ", th %" PRId64 ", iw %" PRId64 ", ih %" PRId64,
	  tw, th, iw, ih);
  */
}


static bool read_tile(openslide_t *osr, uint32_t *dest,
		      int32_t layer,
		      int64_t tile_x, int64_t tile_y) {

  //  g_debug("read_tile %" PRId64 " %" PRId64 " %d", tile_x, tile_y, layer);

  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = data->tiff;
  uint32_t tmp;

  int64_t tiles_across;
  int64_t tiles_down;
  int32_t tile_width;
  int32_t tile_height;
  int32_t last_tile_width;
  int32_t last_tile_height;

  // get_dimensions sets layer implicitly
  get_dimensions(osr, layer, &tiles_across, &tiles_down,
		 &tile_width, &tile_height,
		 &last_tile_width, &last_tile_height);

  // figure out raw tile size
  int64_t tw, th;
  g_return_val_if_fail(TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tmp), false);
  tw = tmp;
  g_return_val_if_fail(TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tmp), false);
  th = tmp;

  // get tile dimensions
  int32_t w, h;
  if (tile_x == tiles_across - 1) {
    w = last_tile_width;
  } else {
    w = tile_width;
  }

  if (tile_y == tiles_down - 1) {
    h = last_tile_height;
  } else {
    h = tile_height;
  }

  data->tileread(tiff, dest, tile_x * tw, tile_y * th, w, h);

  return true;
}

static void convert_coordinate(openslide_t *osr,
			       int32_t layer,
			       int64_t x, int64_t y,
			       int64_t *tile_x, int64_t *tile_y,
			       int32_t *offset_x_in_tile, int32_t *offset_y_in_tile) {
  int64_t tiles_across;
  int64_t tiles_down;
  int32_t tile_width;
  int32_t tile_height;
  int32_t last_tile_width;
  int32_t last_tile_height;

  get_dimensions(osr, layer, &tiles_across, &tiles_down,
		 &tile_width, &tile_height,
		 &last_tile_width, &last_tile_height);

  double downsample = openslide_get_layer_downsample(osr, layer);
  int64_t ds_x = x / downsample;
  int64_t ds_y = y / downsample;

  // x
  *tile_x = ds_x / tile_width;
  *offset_x_in_tile = ds_x % tile_width;
  if (*tile_x >= tiles_across - 1) {
    // this is the last tile
    *tile_x = tiles_across - 1;
    *offset_x_in_tile = ds_x - ((tiles_across - 1) * tile_width);
  }

  // y
  *tile_y = ds_y / tile_height;
  *offset_y_in_tile = ds_y % tile_height;
  if (*tile_y >= tiles_down - 1) {
    // this is the last tile
    *tile_y = tiles_down - 1;
    *offset_y_in_tile = ds_y - ((tiles_down - 1) * tile_height);
  }

  /*
  g_debug("convert_coordinate: (%" PRId64 ",%" PRId64") ->"
	  " t(%" PRId64 ",%" PRId64 ") + (%d,%d)",
	  x, y, *tile_x, *tile_y, *offset_x_in_tile, *offset_y_in_tile);
  */
}

static struct _openslide_ops _openslide_tiff_ops = {
  .destroy = destroy,
  .read_tile = read_tile,
  .get_dimensions = get_dimensions,
  .convert_coordinate = convert_coordinate
};

void _openslide_add_tiff_ops(openslide_t *osr,
			     TIFF *tiff,
			     int32_t overlap_count,
			     int32_t *overlaps,
			     int32_t layer_count,
			     int32_t *layers,
			     _openslide_tiff_tilereader_read_fn tileread,
			     enum _openslide_overlap_mode overlap_mode) {
  g_assert(overlap_mode == OPENSLIDE_OVERLAP_MODE_SANE);

  // allocate private data
  struct _openslide_tiffopsdata *data =
    g_slice_new(struct _openslide_tiffopsdata);

  // store layer info
  data->layers = layers;

  // populate private data
  data->tiff = tiff;
  data->tileread = tileread;
  data->overlap_count = overlap_count;
  data->overlaps = overlaps;

  if (osr == NULL) {
    // free now and return
    destroy_data(data);
    return;
  }

  // load TIFF properties
  TIFFSetDirectory(data->tiff, 0);    // ignoring return value, but nothing we can do if failed
  store_properties(data->tiff, osr->properties);

  // store tiff-specific data into osr
  g_assert(osr->data == NULL);

  // general osr data
  osr->layer_count = layer_count;
  osr->data = data;
  osr->ops = &_openslide_tiff_ops;
}

void _openslide_generic_tiff_tilereader_read(TIFF *tiff,
					     uint32_t *dest,
					     int64_t x, int64_t y,
					     int32_t w, int32_t h) {
  TIFFRGBAImage img;
  char emsg[1024] = "";

  // init
  g_return_if_fail(TIFFRGBAImageOK(tiff, emsg));
  g_return_if_fail(TIFFRGBAImageBegin(&img, tiff, 0, emsg));
  img.req_orientation = ORIENTATION_TOPLEFT;
  img.col_offset = x;
  img.row_offset = y;

  // draw it
  g_return_if_fail(TIFFRGBAImageGet(&img, dest, w, h));

  // permute
  uint32_t *p = dest;
  uint32_t *end = dest + w * h;
  while (p < end) {
    uint32_t val = *p;
    *p++ = (val & 0xFF00FF00)
      | ((val << 16) & 0xFF0000)
      | ((val >> 16) & 0xFF);
  }

  // done
  TIFFRGBAImageEnd(&img);
}
