/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
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

/*
 * Generic TIFF support
 *
 * quickhash comes from _openslide_tiff_init_properties_and_hash
 *
 */

#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <errno.h>

struct generic_tiff_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

static void destroy_data(struct generic_tiff_ops_data *data,
                         struct level **levels, int32_t level_count) {
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct generic_tiff_ops_data, data);

  for (int32_t i = 0; i < level_count; i++) {
    _openslide_grid_destroy(levels[i]->grid);
    g_slice_free(struct level, levels[i]);
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  struct generic_tiff_ops_data *data = osr->data;
  struct level **levels = (struct level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}

static void read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      struct _openslide_grid *grid,
                      int64_t tile_col, int64_t tile_row,
                      void *arg) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            tile_col, tile_row, grid,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    _openslide_tiff_read_tile(osr, tiffl, tiff, tiledata, tile_col, tile_row);

    // clip, if necessary
    _openslide_tiff_clip_tile(osr, tiffl, tiledata, tile_col, tile_row);

    // put it in the cache
    _openslide_cache_put(osr->cache, tile_col, tile_row, grid,
                         tiledata, tw * th * 4,
                         &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 tw, th,
                                                                 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  //_openslide_grid_label_tile(grid, cr, tile_col, tile_row);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);
}

static void paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h) {
  struct generic_tiff_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  TIFF *tiff = _openslide_tiffcache_get(data->tc);
  if (tiff) {
    if (TIFFSetDirectory(tiff, l->tiffl.dir)) {
      _openslide_grid_paint_region(l->grid, cr, tiff,
                                   x / l->base.downsample,
                                   y / l->base.downsample,
                                   level, w, h);
    } else {
      _openslide_set_error(osr, "Cannot set TIFF directory");
    }
  } else {
    _openslide_set_error(osr, "Cannot open TIFF file");
  }
  _openslide_tiffcache_put(data->tc, tiff);
}

static const struct _openslide_ops generic_tiff_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct level *la = a;
  const struct level *lb = b;

  if (la->tiffl.image_w > lb->tiffl.image_w) {
    return -1;
  } else if (la->tiffl.image_w == lb->tiffl.image_w) {
    return 0;
  } else {
    return 1;
  }
}

bool _openslide_try_generic_tiff(openslide_t *osr,
				 struct _openslide_tiffcache *tc, TIFF *tiff,
				 struct _openslide_hash *quickhash1,
				 GError **err) {
  GList *level_list = NULL;

  if (!TIFFIsTiled(tiff)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "TIFF is not tiled");
    goto FAIL;
  }

  // accumulate tiled levels
  int32_t level_count = 0;
  do {
    // confirm that this directory is tiled
    if (!TIFFIsTiled(tiff)) {
      continue;
    }

    // confirm it is either the first image, or reduced-resolution
    if (TIFFCurrentDirectory(tiff) != 0) {
      uint32_t subfiletype;
      if (!TIFFGetField(tiff, TIFFTAG_SUBFILETYPE, &subfiletype)) {
        continue;
      }

      if (!(subfiletype & FILETYPE_REDUCEDIMAGE)) {
        continue;
      }
    }

    // verify that we can read this compression (hard fail if not)
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read compression scheme");
      goto FAIL;
    };
    if (!TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Unsupported TIFF compression: %u", compression);
      goto FAIL;
    }

    // create level
    struct level *l = g_slice_new0(struct level);
    struct _openslide_tiff_level *tiffl = &l->tiffl;
    if (!_openslide_tiff_level_init(tiff,
                                    TIFFCurrentDirectory(tiff),
                                    (struct _openslide_level *) l,
                                    tiffl,
                                    err)) {
      g_slice_free(struct level, l);
      goto FAIL;
    }
    l->grid = _openslide_grid_create_simple(osr,
                                            tiffl->tiles_across,
                                            tiffl->tiles_down,
                                            tiffl->tile_w,
                                            tiffl->tile_h,
                                            read_tile);

    // push into list
    level_list = g_list_prepend(level_list, l);
    level_count++;
  } while (TIFFReadDirectory(tiff));

  // sort tiled levels
  level_list = g_list_sort(level_list, width_compare);

  // copy levels in, while deleting the list
  struct level **levels = g_new(struct level *, level_count);
  for (int i = 0; i < level_count; i++) {
    levels[i] = level_list->data;
    level_list = g_list_delete_link(level_list, level_list);
  }

  g_assert(level_list == NULL);

  // allocate private data
  struct generic_tiff_ops_data *data =
    g_slice_new0(struct generic_tiff_ops_data);

  if (osr == NULL) {
    // free now and return
    _openslide_tiffcache_put(tc, tiff);
    data->tc = tc;
    destroy_data(data, levels, level_count);
    return true;
  }

  // set hash and properties
  if (!_openslide_tiff_init_properties_and_hash(osr, tiff, quickhash1,
                                                levels[level_count - 1]->tiffl.dir,
                                                0,
                                                err)) {
    destroy_data(data, levels, level_count);
    return false;
  }
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
                      g_strdup("generic-tiff"));

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &generic_tiff_ops;

  // put TIFF handle and assume tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  return true;

 FAIL:
  // free the level list
  for (GList *i = level_list; i != NULL; i = g_list_delete_link(i, i)) {
    struct level *l = i->data;
    _openslide_grid_destroy(l->grid);
    g_slice_free(struct level, l);
  }

  return false;
}
