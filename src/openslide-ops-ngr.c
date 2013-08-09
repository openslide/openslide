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

#include <string.h>
#include <stdint.h>
#include <math.h>
#include <glib.h>
#include <cairo.h>

#define NGR_TILE_HEIGHT 64

struct ngr_level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  char *filename;

  int64_t start_in_file;

  int32_t column_width;
};

static void destroy_levels(struct ngr_level **levels, int count) {
  for (int i = 0; i < count; i++) {
    g_free(levels[i]->filename);
    _openslide_grid_destroy(levels[i]->grid);
    g_slice_free(struct ngr_level, levels[i]);
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  destroy_levels((struct ngr_level **) osr->levels, osr->level_count);
}


static void read_tile(openslide_t *osr,
		      cairo_t *cr,
		      struct _openslide_level *level,
		      struct _openslide_grid *grid,
		      int64_t tile_x, int64_t tile_y,
		      void *arg G_GNUC_UNUSED) {
  struct ngr_level *l = (struct ngr_level *) level;
  GError *tmp_err = NULL;

  int64_t tw = l->column_width;
  int64_t th = MIN(NGR_TILE_HEIGHT, l->base.h - tile_y * NGR_TILE_HEIGHT);
  int tilesize = tw * th * 4;
  struct _openslide_cache_entry *cache_entry;
  // look up tile in cache
  uint32_t *tiledata = _openslide_cache_get(osr->cache, tile_x, tile_y, level,
                                            &cache_entry);

  if (!tiledata) {
    // read the tile data
    FILE *f = _openslide_fopen(l->filename, "rb", &tmp_err);
    if (!f) {
      _openslide_set_error_from_gerror(osr, tmp_err);
      g_clear_error(&tmp_err);
      return;
    }

    // compute offset to read
    int64_t offset = l->start_in_file +
      (tile_y * NGR_TILE_HEIGHT * l->column_width * 6) +
      (tile_x * l->base.h * l->column_width * 6);
    //    g_debug("tile_x: %" G_GINT64_FORMAT ", "
    //	    "tile_y: %" G_GINT64_FORMAT ", "
    //	    "seeking to %" G_GINT64_FORMAT, tile_x, tile_y, offset);
    fseeko(f, offset, SEEK_SET);

    // alloc and read
    int buf_size = tw * th * 6;
    uint16_t *buf = g_slice_alloc(buf_size);

    if (fread(buf, buf_size, 1, f) != 1) {
      _openslide_set_error(osr, "Cannot read file %s", l->filename);
      fclose(f);
      g_slice_free1(buf_size, buf);
      return;
    }
    fclose(f);

    // got the data, now convert to 8-bit xRGB
    tiledata = g_slice_alloc(tilesize);
    for (int i = 0; i < tw * th; i++) {
      // scale down from 12 bits
      uint8_t r = GINT16_FROM_LE(buf[(i * 3)]) >> 4;
      uint8_t g = GINT16_FROM_LE(buf[(i * 3) + 1]) >> 4;
      uint8_t b = GINT16_FROM_LE(buf[(i * 3) + 2]) >> 4;

      tiledata[i] = (r << 16) | (g << 8) | b;
    }
    g_slice_free1(buf_size, buf);

    // put it in the cache
    _openslide_cache_put(osr->cache, tile_x, tile_y, level,
                         tiledata,
                         tilesize,
                         &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
								 CAIRO_FORMAT_RGB24,
								 tw, th,
								 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);
}

static void paint_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
			 int64_t x, int64_t y,
			 struct _openslide_level *level,
			 int32_t w, int32_t h) {
  struct ngr_level *l = (struct ngr_level *) level;

  _openslide_grid_paint_region(l->grid, cr, NULL,
                               x / level->downsample,
                               y / level->downsample,
                               level, w, h);
}


static const struct _openslide_ops _openslide_vmu_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};


void _openslide_add_ngr_ops(openslide_t *osr,
			    int32_t ngr_count,
			    struct _openslide_ngr **ngrs) {
  // transform ngrs to levels
  struct ngr_level **levels = g_new(struct ngr_level *, ngr_count);
  for (int32_t i = 0; i < ngr_count; i++) {
    struct _openslide_ngr *ngr = ngrs[i];
    struct ngr_level *l = g_slice_new0(struct ngr_level);
    l->base.w = ngr->w;
    l->base.h = ngr->h;
    l->base.tile_w = ngr->column_width;
    l->base.tile_h = NGR_TILE_HEIGHT;
    l->grid = _openslide_grid_create_simple(osr,
                                            ngr->w / ngr->column_width,
                                            (ngr->h + NGR_TILE_HEIGHT - 1) / NGR_TILE_HEIGHT,
                                            ngr->column_width,
                                            NGR_TILE_HEIGHT,
                                            read_tile);
    l->filename = ngr->filename;
    l->start_in_file = ngr->start_in_file;
    l->column_width = ngr->column_width;
    levels[i] = l;
    g_slice_free(struct _openslide_ngr, ngr);
  }
  g_free(ngrs);

  if (osr == NULL) {
    destroy_levels(levels, ngr_count);
    return;
  }

  // set levels
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;

  // general osr data
  osr->level_count = ngr_count;
  osr->ops = &_openslide_vmu_ops;
}
