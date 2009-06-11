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

#include "openslide-private.h"
#include "openslide-cache.h"
#include "openslide-tilehelper.h"

struct _openslide_tiffopsdata {
  TIFF *tiff;

  int32_t overlap_count;
  int32_t *overlaps;
  int32_t *layers;

  struct _openslide_cache *cache;

  _openslide_tiff_tilereader_create_fn tilereader_create;
  _openslide_tiff_tilereader_read_fn tilereader_read;
  _openslide_tiff_tilereader_destroy_fn tilereader_destroy;
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

struct tilereader {
  struct _openslide_tiff_tilereader *tilereader;
  _openslide_tiff_tilereader_read_fn tilereader_read;
};

static bool tilereader_read(void *tilereader_data,
			    uint32_t *dest, int64_t x, int64_t y,
			    int32_t w, int32_t h) {
  struct tilereader *tilereader = tilereader_data;
  tilereader->tilereader_read(tilereader->tilereader, dest, x, y, w, h);

  return true;
}

static void read_region(openslide_t *osr, uint32_t *dest,
			int64_t x, int64_t y,
			int32_t layer,
			int64_t w, int64_t h) {
  uint32_t tmp;

  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = data->tiff;

  double downsample = openslide_get_layer_downsample(osr, layer);
  int64_t ds_x = x / downsample;
  int64_t ds_y = y / downsample;

  // select layer
  g_return_if_fail(TIFFSetDirectory(tiff, data->layers[layer]));

  // determine space for 1 tile
  int64_t tw, th;
  g_return_if_fail(TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tmp));
  tw = tmp;
  g_return_if_fail(TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tmp));
  th = tmp;

  // determine full size
  int64_t raw_w, raw_h;
  g_return_if_fail(TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &tmp));
  raw_w = tmp;
  g_return_if_fail(TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &tmp));
  raw_h = tmp;

  // figure out range
  int64_t start_x, start_y, end_x, end_y;

  // adjust for overlaps
  int32_t ovr_x = 0;
  int32_t ovr_y = 0;
  if (layer < data->overlap_count) {
    ovr_x = data->overlaps[layer * 2];
    ovr_y = data->overlaps[(layer * 2) + 1];
  }

  int64_t total_tiles_across = raw_w / tw;
  int64_t total_tiles_down = raw_h / th;
  add_in_overlaps(osr, layer, tw, th,
		  total_tiles_across, total_tiles_down,
		  ds_x, ds_y, &start_x, &start_y);
  add_in_overlaps(osr, layer, tw, th,
		  total_tiles_across, total_tiles_down,
		  ds_x + w, ds_y + h,
		  &end_x, &end_y);

  // check bounds
  if (end_x >= raw_w) {
    end_x = raw_w - 1;
  }
  if (end_y >= raw_h) {
    end_y = raw_h - 1;
  }

  //g_debug("from (%d,%d) to (%d,%d)", start_x, start_y, end_x, end_y);


  // for each tile, draw it where it should go
  struct _openslide_tiff_tilereader *tilereader = data->tilereader_create(tiff);

  struct tilereader tilereader_data = { .tilereader = tilereader,
					.tilereader_read = data->tilereader_read };

  _openslide_read_tiles(start_x, start_y, end_x, end_y, ovr_x, ovr_y,
			w, h, layer, tw, th, tilereader_read,
			&tilereader_data,
			dest, data->cache);

  data->tilereader_destroy(tilereader);
}


static void destroy_data(struct _openslide_tiffopsdata *data) {
  TIFFClose(data->tiff);
  g_free(data->layers);
}

static void destroy(openslide_t *osr) {
  struct _openslide_tiffopsdata *data = osr->data;
  _openslide_cache_destroy(data->cache);
  destroy_data(data);
  g_slice_free(struct _openslide_tiffopsdata, data);
}

static void get_dimensions(openslide_t *osr, int32_t layer,
			   int64_t *w, int64_t *h) {
  uint32_t tmp;

  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = data->tiff;

  *w = 0;
  *h = 0;

  // check bounds
  if (layer >= osr->layer_count) {
    return;
  }

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

  // subtract out the overlaps
  *w = iw;
  *h = ih;
  if (iw >= tw) {
    *w -= ((iw / tw) - 1) * ox;
  }
  if (ih >= th) {
    *h -= ((ih / th) - 1) * oy;
  }
}

static struct _openslide_ops _openslide_tiff_ops = {
  .read_region = read_region,
  .destroy = destroy,
  .get_dimensions = get_dimensions,
};

void _openslide_add_tiff_ops(openslide_t *osr,
			     TIFF *tiff,
			     int32_t overlap_count,
			     int32_t *overlaps,
			     int32_t layer_count,
			     int32_t *layers,
			     _openslide_tiff_tilereader_create_fn creator,
			     _openslide_tiff_tilereader_read_fn reader,
			     _openslide_tiff_tilereader_destroy_fn destroyer,
			     enum _openslide_overlap_mode overlap_mode) {
  g_assert(overlap_mode == OPENSLIDE_OVERLAP_MODE_SANE);

  // allocate private data
  struct _openslide_tiffopsdata *data =
    g_slice_new(struct _openslide_tiffopsdata);

  // store layer info
  data->layers = layers;

  // populate private data
  data->tiff = tiff;

  data->tilereader_create = creator;
  data->tilereader_read = reader;
  data->tilereader_destroy = destroyer;

  if (osr == NULL) {
    // free now and return
    destroy_data(data);
    return;
  }

  // create cache
  data->cache = _openslide_cache_create(_OPENSLIDE_USEFUL_CACHE_SIZE);

  // load TIFF properties
  TIFFSetDirectory(data->tiff, 0);    // ignoring return value, but nothing we can do if failed
  store_properties(data->tiff, osr->properties);

  // store tiff-specific data into osr
  g_assert(osr->data == NULL);

  osr->layer_count = layer_count;
  osr->data = data;
  osr->ops = &_openslide_tiff_ops;
}


struct _openslide_tiff_tilereader {
  TIFFRGBAImage img;
};

struct _openslide_tiff_tilereader *_openslide_generic_tiff_tilereader_create(TIFF *tiff) {
  // image
  TIFFRGBAImage img;
  char emsg[1024] = "";
  g_return_val_if_fail(TIFFRGBAImageBegin(&img, tiff, 0, emsg), NULL);
  img.req_orientation = ORIENTATION_TOPLEFT;

  // success! allocate and return
  struct _openslide_tiff_tilereader *wtt =
    g_slice_new(struct _openslide_tiff_tilereader);
  wtt->img = img;

  return wtt;
}

void _openslide_generic_tiff_tilereader_read(struct _openslide_tiff_tilereader *wtt,
					     uint32_t *dest,
					     int64_t x, int64_t y,
					     int32_t w, int32_t h) {
  wtt->img.col_offset = x;
  wtt->img.row_offset = y;
  g_return_if_fail(TIFFRGBAImageGet(&wtt->img, dest, w, h));

  // permute
  uint32_t *p = dest;
  uint32_t *end = dest + w * h;
  while (p < end) {
    uint32_t val = *p;
    *p++ = (val & 0xFF00FF00)
      | ((val << 16) & 0xFF0000)
      | ((val >> 16) & 0xFF);
  }
}

void _openslide_generic_tiff_tilereader_destroy(struct _openslide_tiff_tilereader *wtt) {
  g_slice_free(struct _openslide_tiff_tilereader, wtt);
}
