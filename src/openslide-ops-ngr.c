/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-cache.h"
#include "openslide-tilehelper.h"

#include <string.h>
#include <stdint.h>
#include <glib.h>
#include <cairo.h>


struct ngr_data {
  int32_t ngr_count;
  struct _openslide_ngr **ngrs;
};


static void destroy_ngrs(struct _openslide_ngr **ngrs, int count) {
  for (int i = 0; i < count; i++) {
    g_free(ngrs[i]->filename);
    g_slice_free(struct _openslide_ngr, ngrs[i]);
  }
  g_free(ngrs);
}

static void destroy(openslide_t *osr) {
  struct ngr_data *data = osr->data;

  destroy_ngrs(data->ngrs, data->ngr_count);

  g_slice_free(struct ngr_data, data);
}


static void get_dimensions(openslide_t *osr, int32_t level,
			   int64_t *w, int64_t *h) {

  struct ngr_data *data = osr->data;
  struct _openslide_ngr *ngr = data->ngrs[level];

  *w = ngr->w;
  *h = ngr->h;
}

static void read_tile(openslide_t *osr,
		      cairo_t *cr,
		      int32_t level,
		      int64_t tile_x, int64_t tile_y,
		      double translate_x, double translate_y,
		      struct _openslide_cache *cache) {
  struct ngr_data *data = osr->data;
  struct _openslide_ngr *ngr = data->ngrs[level];
  GError *tmp_err = NULL;

  // check if beyond boundary
  int num_columns = ngr->w / ngr->column_width;
  if ((tile_x >= num_columns) || (tile_y >= ngr->h)) {
    return;
  }

  int tilesize = ngr->column_width * 4;
  struct _openslide_cache_entry *cache_entry;
  // look up tile in cache
  uint32_t *tiledata = _openslide_cache_get(cache, tile_x, tile_y, level,
                                            &cache_entry);

  if (!tiledata) {
    // read the tile data
    FILE *f = _openslide_fopen(ngr->filename, "rb", &tmp_err);
    if (!f) {
      _openslide_set_error_from_gerror(osr, tmp_err);
      g_clear_error(&tmp_err);
      return;
    }

    // compute offset to read
    int64_t offset = ngr->start_in_file +
      (tile_y * ngr->column_width * 6) +
      (tile_x * ngr->h * ngr->column_width * 6);
    //    g_debug("tile_x: %" G_GINT64_FORMAT ", "
    //	    "tile_y: %" G_GINT64_FORMAT ", "
    //	    "seeking to %" G_GINT64_FORMAT, tile_x, tile_y, offset);
    fseeko(f, offset, SEEK_SET);

    // alloc and read
    int buf_size = ngr->column_width * 6;
    uint16_t *buf = g_slice_alloc(buf_size);

    if (fread(buf, buf_size, 1, f) != 1) {
      _openslide_set_error(osr, "Cannot read file %s", ngr->filename);
      fclose(f);
      g_slice_free1(buf_size, buf);
      return;
    }
    fclose(f);

    // got the data, now convert to 8-bit xRGB
    tiledata = g_slice_alloc(tilesize);
    for (int i = 0; i < ngr->column_width; i++) {
      // scale down from 12 bits
      uint8_t r = GINT16_FROM_LE(buf[(i * 3)]) >> 4;
      uint8_t g = GINT16_FROM_LE(buf[(i * 3) + 1]) >> 4;
      uint8_t b = GINT16_FROM_LE(buf[(i * 3) + 2]) >> 4;

      tiledata[i] = (r << 16) | (g << 8) | b;
    }
    g_slice_free1(buf_size, buf);

    // put it in the cache
    _openslide_cache_put(cache, tile_x, tile_y, level,
                         tiledata,
                         ngr->column_width * 1 * 4,
                         &cache_entry);
  }

  // draw it
  int64_t tw = ngr->column_width;
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
								 CAIRO_FORMAT_RGB24,
								 tw, 1,
								 tw * 4);
  cairo_matrix_t matrix;
  cairo_get_matrix(cr, &matrix);
  cairo_translate(cr, translate_x, translate_y);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);
  cairo_set_matrix(cr, &matrix);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);
}

static void paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 int32_t level, int32_t w, int32_t h) {
  struct ngr_data *data = osr->data;
  struct _openslide_ngr *ngr = data->ngrs[level];

  // compute coordinates
  double ds = openslide_get_level_downsample(osr, level);
  double ds_x = x / ds;
  double ds_y = y / ds;
  int64_t start_tile_x = ds_x / ngr->column_width;
  int64_t end_tile_x = ((ds_x + w) / ngr->column_width) + 1;
  int64_t start_tile_y = ds_y;
  int64_t end_tile_y = ds_y + h + 1;

  double offset_x = ds_x - (start_tile_x * ngr->column_width);

  _openslide_read_tiles(cr,
			level,
			start_tile_x, start_tile_y,
			end_tile_x, end_tile_y,
			offset_x, 0,
			ngr->column_width, 1,
			osr, osr->cache,
			read_tile);
}


static const struct _openslide_ops _openslide_vmu_ops = {
  .get_dimensions = get_dimensions,
  .paint_region = paint_region,
  .destroy = destroy,
};


void _openslide_add_ngr_ops(openslide_t *osr,
			    int32_t ngr_count,
			    struct _openslide_ngr **ngrs) {
  if (osr == NULL) {
    destroy_ngrs(ngrs, ngr_count);
    return;
  }

  // allocate private data
  struct ngr_data *data =
    g_slice_new(struct ngr_data);

  data->ngr_count = ngr_count;
  data->ngrs = ngrs;

  // store vmu-specific data into osr
  g_assert(osr->data == NULL);
  osr->data = data;

  // general osr data
  osr->level_count = ngr_count;
  osr->ops = &_openslide_vmu_ops;
}
