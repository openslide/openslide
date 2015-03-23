/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2015 Mathieu Malaterre
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
 * DICOM support for VL Whole Slide Microscopy Image Storage (1.2.840.10008.5.1.4.1.1.77.1.6)
 *
 * quickhash comes from (0020,000d) Study Instance UID
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-dicom.h"
#include "openslide-decode-jpeg.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

struct level {
  struct _openslide_level base;
  struct _openslide_dicom_level dicoml;
  struct _openslide_grid *grid;
};

struct dicom_wsmis_ops_data {
  gchar **datafile_paths;
};

static void destroy(openslide_t *osr) {
  struct dicom_wsmis_ops_data *data = osr->data;
  g_slice_free(struct dicom_wsmis_ops_data, data);

  for (int32_t i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];
    _openslide_grid_destroy(l->grid);
    g_slice_free(struct level, l);
  }
  g_free(osr->levels);
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *arg,
                      GError **err) {
  struct level *l = (struct level *) level;
  struct _openslide_dicom_level *dicoml = &l->dicoml;
  struct dicom_wsmis_ops_data *data = arg;

  // tile size
  int64_t tw = dicoml->tile_w;
  int64_t th = dicoml->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    struct tile * tiles = dicoml->tiles;
    const int tidx = tile_col * dicoml->tiles_across + tile_row;
    if (!_openslide_jpeg_read(data->datafile_paths[dicoml->fileno],
                                  tiles[tidx].start_in_file,
                                  tiledata, tw, th,
                                  err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // clip, if necessary
    if (!_openslide_clip_tile(tiledata,
          dicoml->tile_w, dicoml->tile_h,
          dicoml->image_w - tile_col * dicoml->tile_w,
          dicoml->image_h - tile_row * dicoml->tile_h,
          err) ) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // put it in the cache
    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
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

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct dicom_wsmis_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  return _openslide_grid_paint_region(l->grid, cr, data,
                                      x / level->downsample,
                                      y / level->downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops dicom_wsmis_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool dicom_wsmis_detect(const char *filename,
                                struct _openslide_tifflike *tl,
                                GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // verify existence
  if (!g_file_test(filename, G_FILE_TEST_EXISTS)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not exist");
    return false;
  }

  // ensure DICOM is DICOMDIR instance
  if (!_openslide_dicom_is_dicomdir(filename, err)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "DICOM is not DICOMDIR");
    return false;
  }

  return true;
}

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct level *la = *(const struct level **) a;
  const struct level *lb = *(const struct level **) b;

  if (la->dicoml.image_w > lb->dicoml.image_w) {
    return -1;
  } else if (la->dicoml.image_w == lb->dicoml.image_w) {
    return 0;
  } else {
    return 1;
  }
}

static bool dicom_wsmis_open(openslide_t *osr, const char *filename,
                              struct _openslide_tifflike *tl G_GNUC_UNUSED,
                              struct _openslide_hash *quickhash1, GError **err) {
  char *dirname = NULL;
  // get directory from filename
  char *end = strrchr(filename, '/');
  if( end )
    dirname = g_strndup(filename, end - filename );
  else
    dirname = g_strndup(".", 1);

  struct _openslide_dicom * instance = _openslide_dicom_create(filename, err);

  char **datafile_paths = NULL;
  if(!_openslide_dicom_readindex(instance, dirname, &datafile_paths))
    {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
      "Could not read DICOMDIR");
    return false;
    }
  _openslide_dicom_destroy(instance);

  // accumulate tiled levels
  char ** fullpath = datafile_paths;
  GPtrArray *level_array = g_ptr_array_new();
  while( *fullpath )
    {
    // create level
    struct level *l = g_slice_new0(struct level);
    struct _openslide_dicom_level *dicoml = &l->dicoml;

    struct _openslide_dicom * instance = _openslide_dicom_create(*fullpath, err);
 
    if (!_openslide_dicom_level_init(instance,
                                    (struct _openslide_level *) l,
                                    dicoml,
                                    err)) {
      g_slice_free(struct level, l);
    }
    _openslide_dicom_destroy(instance);
    l->grid = _openslide_grid_create_simple(osr,
                                            dicoml->tiles_across,
                                            dicoml->tiles_down,
                                            dicoml->tile_w,
                                            dicoml->tile_h,
                                            read_tile);

    const int index = fullpath - datafile_paths;
    dicoml->fileno = index;
    // add to array
    if( !dicoml->is_icon )
      g_ptr_array_add(level_array, l);

    ++fullpath;
    }
  // sort tiled levels
  g_ptr_array_sort(level_array, width_compare);

  // unwrap level array
  int32_t level_count = level_array->len;
  struct level **levels =
    (struct level **) g_ptr_array_free(level_array, false);
  level_array = NULL;

  // Use Study Instance UID for hash:
  struct level *first = levels[0];
  struct _openslide_dicom_level *dicoml = &first->dicoml;
  _openslide_hash_string(quickhash1, dicoml->hash);

  // store osr data
  g_assert(osr->data == NULL);
  struct dicom_wsmis_ops_data *data = g_slice_new0(struct dicom_wsmis_ops_data);
  data->datafile_paths = datafile_paths;
  osr->data = data;

  // set ops
  osr->ops = &dicom_wsmis_ops;

  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;

  return true;
}

const struct _openslide_format _openslide_format_dicom_wsmis = {
  .name = "dicom-wsmis",
  .vendor = "dicom-wsmis",
  .detect = dicom_wsmis_detect,
  .open = dicom_wsmis_open,
};
