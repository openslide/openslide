/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
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
 * Trestle (tif) support
 *
 * quickhash comes from _openslide_tiff_init_properties_and_hash
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-cache.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <errno.h>

static const char TRESTLE_SOFTWARE[] = "MedScan";
static const char OVERLAPS_XY[] = "OverlapsXY=";
static const char BACKGROUND_COLOR[] = "Background Color=";

struct trestle_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  tdir_t dir;

  int32_t overlap_x;
  int32_t overlap_y;
};

#define SET_DIR_OR_FAIL(osr, tiff, i)				\
  if (!TIFFSetDirectory(tiff, i)) {				\
    _openslide_set_error(osr, "Cannot set TIFF directory");	\
    return;							\
  }

#define GET_FIELD_OR_FAIL(osr, tiff, tag, result)		\
  if (!TIFFGetField(tiff, tag, &tmp)) {				\
    _openslide_set_error(osr, "Cannot get required TIFF tag: %d", tag);	\
    return;							\
  }								\
  result = tmp;

static void read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      struct _openslide_grid *grid,
                      int64_t tile_x, int64_t tile_y,
                      void *arg);

static void destroy_data(struct trestle_ops_data *data,
                         struct level **levels, int32_t level_count) {
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct trestle_ops_data, data);

  for (int32_t i = 0; i < level_count; i++) {
    _openslide_grid_destroy(levels[i]->grid);
    g_slice_free(struct level, levels[i]);
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  struct trestle_ops_data *data = osr->data;
  struct level **levels = (struct level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}


static void set_dimensions(openslide_t *osr, TIFF *tiff,
                           struct level *l, bool geometry) {
  uint32_t tmp;

  // set the directory
  SET_DIR_OR_FAIL(osr, tiff, l->dir)

  // figure out tile size
  int64_t tw, th;
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_TILEWIDTH, tw)
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_TILELENGTH, th)

  // get image size
  int64_t iw, ih;
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_IMAGEWIDTH, iw)
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_IMAGELENGTH, ih)

  // safe now, start writing
  if (geometry) {
    l->base.tile_w = tw;
    l->base.tile_h = th;
  }

  // num tiles in each dimension
  int64_t tiles_across = (iw / tw) + !!(iw % tw);   // integer ceiling
  int64_t tiles_down = (ih / th) + !!(ih % th);

  // subtract out the overlaps (there are tiles-1 overlaps in each dimension)
  l->base.w = iw;
  l->base.h = ih;
  if (iw >= tw) {
    l->base.w -= (tiles_across - 1) * l->overlap_x;
  }
  if (ih >= th) {
    l->base.h -= (tiles_down - 1) * l->overlap_y;
  }

  // set up grid
  l->grid = _openslide_grid_create_simple(osr,
                                          tiles_across, tiles_down,
                                          tw - l->overlap_x,
                                          th - l->overlap_y,
                                          read_tile);
}

static void read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      struct _openslide_grid *grid,
                      int64_t tile_x, int64_t tile_y,
                      void *arg) {
  struct level *l = (struct level *) level;
  TIFF *tiff = arg;
  uint32_t tmp;

  // set the directory
  SET_DIR_OR_FAIL(osr, tiff, l->dir)

  // tile size
  int64_t tw, th;
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_TILEWIDTH, tw)
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_TILELENGTH, th)

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache, tile_x, tile_y, grid,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    _openslide_tiff_read_tile(osr, tiff, tiledata, tile_x, tile_y);

    // clip, if necessary
    _openslide_tiff_clip_tile(osr, tiff, tiledata, tile_x, tile_y);

    // put it in the cache
    _openslide_cache_put(osr->cache, tile_x, tile_y, grid,
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

  //_openslide_grid_label_tile(grid, cr, tile_x, tile_y);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);
}

static void paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h) {
  struct trestle_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  TIFF *tiff = _openslide_tiffcache_get(data->tc);
  if (tiff) {
    if (TIFFSetDirectory(tiff, l->dir)) {
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


static const struct _openslide_ops _openslide_tiff_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static void add_trestle_ops(openslide_t *osr,
                            TIFF *tiff,
                            int32_t overlap_count,
                            int32_t *overlaps,
                            int32_t level_count,
                            int32_t *directories,
                            struct _openslide_hash *quickhash1) {
  // allocate private data
  struct trestle_ops_data *data = g_slice_new0(struct trestle_ops_data);

  GError *tmp_err = NULL;

  // create levels
  struct level **levels = g_new(struct level *, level_count);
  for (int32_t i = 0; i < level_count; i++) {
    struct level *l = g_slice_new0(struct level);
    l->dir = directories[i];
    if (i < overlap_count) {
      l->overlap_x = overlaps[2 * i];
      l->overlap_y = overlaps[2 * i + 1];
    }
    levels[i] = l;
  }
  g_free(directories);
  g_free(overlaps);

  if (osr == NULL) {
    // free now and return
    TIFFClose(tiff);
    destroy_data(data, levels, level_count);
    return;
  }

  // if any level has overlaps, reporting tile advances would mislead the
  // application
  bool report_geometry = true;
  for (int32_t i = 0; i < level_count; i++) {
    if (levels[i]->overlap_x || levels[i]->overlap_y) {
      report_geometry = false;
      break;
    }
  }

  // set dimensions
  for (int32_t i = 0; i < level_count; i++) {
    set_dimensions(osr, tiff, levels[i], report_geometry);
  }

  // generate hash of the smallest level
  if (!_openslide_tiff_init_properties_and_hash(osr, tiff, quickhash1,
                                                levels[level_count - 1]->dir,
                                                0,
                                                &tmp_err)) {
    _openslide_set_error_from_gerror(osr, tmp_err);
    g_clear_error(&tmp_err);
  }

  // store tiff-specific data into osr
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);

  // general osr data
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &_openslide_tiff_ops;

  // create TIFF cache from handle
  data->tc = _openslide_tiffcache_create(tiff);
}

static void add_properties(GHashTable *ht, char **tags) {
  g_hash_table_insert(ht,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
                      g_strdup("trestle"));

  for (char **tag = tags; *tag != NULL; tag++) {
    char **pair = g_strsplit(*tag, "=", 2);
    if (pair) {
      char *name = g_strstrip(pair[0]);
      if (name) {
        char *value = g_strstrip(pair[1]);

        g_hash_table_insert(ht,
                            g_strdup_printf("trestle.%s", name),
                            g_strdup(value));
      }
    }
    g_strfreev(pair);
  }

  _openslide_duplicate_int_prop(ht, "trestle.Objective Power",
                                OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
}

static void parse_trestle_image_description(openslide_t *osr,
                                            const char *description,
                                            int32_t *overlap_count_OUT,
                                            int32_t **overlaps_OUT) {
  char **first_pass = g_strsplit(description, ";", -1);

  int32_t overlap_count = 0;
  int32_t *overlaps = NULL;

  if (osr) {
    add_properties(osr->properties, first_pass);
  }

  for (char **cur_str = first_pass; *cur_str != NULL; cur_str++) {
    //g_debug(" XX: %s", *cur_str);
    if (g_str_has_prefix(*cur_str, OVERLAPS_XY)) {
      // found it
      char **second_pass = g_strsplit(*cur_str, " ", -1);

      overlap_count = g_strv_length(second_pass) - 1; // skip fieldname
      overlaps = g_new(int32_t, overlap_count);

      int i = 0;
      // skip fieldname
      for (char **cur_str2 = second_pass + 1; *cur_str2 != NULL; cur_str2++) {
        overlaps[i] = g_ascii_strtoull(*cur_str2, NULL, 10);
        i++;
      }

      g_strfreev(second_pass);
    } else if (g_str_has_prefix(*cur_str, BACKGROUND_COLOR)) {
      // found background color
      errno = 0;
      uint64_t bg = g_ascii_strtoull((*cur_str) + strlen(BACKGROUND_COLOR), NULL, 16);
      if (bg || !errno) {
        if (osr) {
          _openslide_set_background_color_prop(osr->properties,
                                               (bg >> 16) & 0xFF,
                                               (bg >> 8) & 0xFF,
                                               bg & 0xFF);
        }
      }
    }
  }
  g_strfreev(first_pass);

  *overlap_count_OUT = overlap_count;
  *overlaps_OUT = overlaps;
}

static char *get_associated_path(TIFF *tiff, const char *extension) {
  char *base_path = g_strdup(TIFFFileName(tiff));

  // strip file extension, if present
  char *dot = g_strrstr(base_path, ".");
  if (dot != NULL) {
    *dot = 0;
  }

  char *path = g_strdup_printf("%s%s", base_path, extension);
  g_free(base_path);
  return path;
}

static void add_associated_jpeg(openslide_t *osr, TIFF *tiff,
                                const char *extension,
                                const char *name) {
  char *path = get_associated_path(tiff, extension);
  _openslide_add_jpeg_associated_image(osr->associated_images,
                                       name, path, 0, NULL);
  g_free(path);
}

bool _openslide_try_trestle(openslide_t *osr, TIFF *tiff,
                            struct _openslide_hash *quickhash1,
                            GError **err) {
  int32_t overlap_count = 0;
  int32_t *overlaps = NULL;
  int32_t level_count = 0;
  int32_t *levels = NULL;

  if (!TIFFIsTiled(tiff)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "TIFF is not tiled");
    goto FAIL;
  }

  char *tagval;
  int tiff_result;
  tiff_result = TIFFGetField(tiff, TIFFTAG_SOFTWARE, &tagval);
  if (!tiff_result ||
      (strncmp(TRESTLE_SOFTWARE, tagval, strlen(TRESTLE_SOFTWARE)) != 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a Trestle slide");
    goto FAIL;
  }

  // parse
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (!tiff_result) {
    // no description, not trestle
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a Trestle slide");
    goto FAIL;
  }
  parse_trestle_image_description(osr, tagval, &overlap_count, &overlaps);

  // count and validate levels
  do {
    if (!TIFFIsTiled(tiff)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                  "TIFF level is not tiled");
      goto FAIL;
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

    // level ok
    level_count++;
  } while (TIFFReadDirectory(tiff));
  levels = g_new(int32_t, level_count);

  // directories are linear
  for (int32_t i = 0; i < level_count; i++) {
    levels[i] = i;
  }

  // add associated images
  if (osr) {
    add_associated_jpeg(osr, tiff, ".Full", "macro");
  }

  // all set, load up the TIFF-specific ops
  add_trestle_ops(osr, tiff,
                  overlap_count / 2, overlaps,
                  level_count, levels,
                  quickhash1);

  // copy the TIFF resolution props to the standard MPP properties
  // this is a totally non-standard use of these TIFF tags
  if (osr) {
    _openslide_duplicate_double_prop(osr->properties, "tiff.XResolution",
                                     OPENSLIDE_PROPERTY_NAME_MPP_X);
    _openslide_duplicate_double_prop(osr->properties, "tiff.YResolution",
                                     OPENSLIDE_PROPERTY_NAME_MPP_Y);
  }

  return true;

 FAIL:
  g_free(levels);
  g_free(overlaps);
  return false;
}
