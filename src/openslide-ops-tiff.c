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

  _openslide_tiff_tilereader_fn tileread;
};


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
			   int64_t *w, int64_t *h) {
  uint32_t tmp;

  // init
  *w = 0;
  *h = 0;

  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = data->tiff;

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
  int64_t tiles_across = (iw / tw) + !!(iw % tw);   // integer ceiling
  int64_t tiles_down = (ih / th) + !!(ih % th);

  // subtract out the overlaps (there are tiles-1 overlaps in each dimension)
  int64_t iw_minus_o = iw;
  int64_t ih_minus_o = ih;
  if (iw >= tw) {
    iw_minus_o -= (tiles_across - 1) * ox;
  }
  if (ih >= th) {
    ih_minus_o -= (tiles_down - 1) * oy;
  }

  // commit
  *w = iw_minus_o;
  *h = ih_minus_o;
}

static int64_t compute_tile_dimension(TIFF *tiff,
				      ttag_t tile_tag,
				      ttag_t image_tag,
				      int64_t tile,
				      int32_t overlap) {
  uint32_t tmp;

  // figure out tile size
  int64_t tile_size;
  g_return_val_if_fail(TIFFGetField(tiff, tile_tag, &tmp), 0);
  tile_size = tmp;

  // get image size
  int64_t image_size;
  g_return_val_if_fail(TIFFGetField(tiff, image_tag, &tmp), 0);
  image_size = tmp;

  // num tiles in dimension
  int64_t tile_count = (image_size / tile_size) + !!(image_size % tile_size);

  // check bounds
  if ((tile < 0) || (tile >= tile_count)) {
    return 0;
  }

  // inner tile? are we done?
  if (tile < tile_count - 1) {
    return tile_size - overlap;
  }

  // otherwise, return last tile size (slack at end of image)
  return image_size - (tile_count - 1) * tile_size;
}

static int64_t get_tile_width(openslide_t *osr,
			      int32_t layer,
			      int64_t tile_x) {
  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = data->tiff;

  // get the layer
  g_return_val_if_fail(TIFFSetDirectory(tiff, data->layers[layer]), 0);

  int32_t overlap = 0;
  if (layer < data->overlap_count) {
    overlap = data->overlaps[layer * 2];
  }

  return compute_tile_dimension(tiff,
				TIFFTAG_TILEWIDTH, TIFFTAG_IMAGEWIDTH,
				tile_x, overlap);
}

static int64_t get_tile_height(openslide_t *osr,
			       int32_t layer,
			       int64_t tile_y) {
  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = data->tiff;

  // get the layer
  g_return_val_if_fail(TIFFSetDirectory(tiff, data->layers[layer]), 0);

  int32_t overlap = 0;
  if (layer < data->overlap_count) {
    overlap = data->overlaps[(layer * 2) + 1];
  }

  return compute_tile_dimension(tiff,
				TIFFTAG_TILELENGTH, TIFFTAG_IMAGELENGTH,
				tile_y, overlap);
}

static bool read_tile(openslide_t *osr, uint32_t *dest,
		      int32_t layer,
		      int64_t tile_x, int64_t tile_y,
		      int64_t tile_w, int64_t tile_h) {

  //  g_debug("read_tile %" PRId64 " %" PRId64 " %d", tile_x, tile_y, layer);

  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = data->tiff;
  uint32_t tmp;

  // set the layer
  g_return_val_if_fail(TIFFSetDirectory(tiff, data->layers[layer]), 0);

  // figure out raw tile size
  int64_t tw, th;
  g_return_val_if_fail(TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tmp), false);
  tw = tmp;
  g_return_val_if_fail(TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tmp), false);
  th = tmp;

  data->tileread(tiff, dest, tile_x * tw, tile_y * th, tile_w, tile_h);

  return true;
}

static void convert_coordinate(openslide_t *osr,
			       int32_t layer,
			       int64_t x, int64_t y,
			       int64_t *tile_x, int64_t *tile_y,
			       int32_t *offset_x_in_tile,
			       int32_t *offset_y_in_tile) {
  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = data->tiff;
  uint32_t tmp;

  // init
  *tile_x = 0;
  *tile_y = 0;
  *offset_x_in_tile = 0;
  *offset_y_in_tile = 0;

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
  int64_t tiles_across = (iw / tw) + !!(iw % tw);   // integer ceiling
  int64_t tiles_down = (ih / th) + !!(ih % th);

  // overlaps
  int32_t ox = 0;
  int32_t oy = 0;
  if (layer < data->overlap_count) {
    ox = data->overlaps[layer * 2];
    oy = data->overlaps[(layer * 2) + 1];
  }

  _openslide_convert_coordinate(openslide_get_layer_downsample(osr, layer),
				x, y,
				tiles_across, tiles_down,
				tw, th,
				ox, oy,
				1, 1,
				tile_x, tile_y,
				offset_x_in_tile, offset_y_in_tile);
}

static const struct _openslide_ops _openslide_tiff_ops = {
  .get_dimensions = get_dimensions,
  .convert_coordinate = convert_coordinate,
  .get_tile_width = get_tile_width,
  .get_tile_height = get_tile_height,
  .read_tile = read_tile,
  .destroy = destroy
};

void _openslide_add_tiff_ops(openslide_t *osr,
			     TIFF *tiff,
			     int32_t overlap_count,
			     int32_t *overlaps,
			     int32_t layer_count,
			     int32_t *layers,
			     _openslide_tiff_tilereader_fn tileread,
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

void _openslide_generic_tiff_tilereader(TIFF *tiff,
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
