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
 * Trestle (tif) support
 *
 * quickhash comes from _openslide_tifflike_init_properties_and_hash
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"
#include "openslide-decode-jpeg.h"

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
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_slice_free(struct level, l);
}

static void destroy(openslide_t *osr) {
  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *) osr->levels[i]);
  }
  g_free(osr->levels);

  struct trestle_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct trestle_ops_data, data);
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col, int64_t tile_row,
                      void *tile G_GNUC_UNUSED,
                      void *arg,
                      GError **err) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  g_autoptr(_openslide_cache_entry) cache_entry = NULL;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    g_auto(_openslide_slice) box = _openslide_slice_alloc(tw * th * 4);
    if (!_openslide_tiff_read_tile(tiffl, tiff,
                                   box.p, tile_col, tile_row,
                                   err)) {
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, box.p,
                                   tile_col, tile_row,
                                   err)) {
      return false;
    }

    // put it in the cache
    tiledata = _openslide_slice_steal(&box);
    _openslide_cache_put(osr->cache, level, tile_col, tile_row,
                         tiledata, tw * th * 4,
                         &cache_entry);
  }

  // draw it
  g_autoptr(cairo_surface_t) surface =
    cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                        CAIRO_FORMAT_ARGB32,
                                        tw, th, tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  return true;
}

static bool paint_region(openslide_t *osr, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct trestle_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(data->tc, err);
  if (ct.tiff == NULL) {
    return false;
  }

  return _openslide_grid_paint_region(l->grid, cr, ct.tiff,
                                      x / l->base.downsample,
                                      y / l->base.downsample,
                                      level, w, h,
                                      err);
}

static const struct _openslide_ops trestle_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool trestle_detect(const char *filename G_GNUC_UNUSED,
                           struct _openslide_tifflike *tl,
                           GError **err) {
  // ensure we have a TIFF
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // check Software field
  const char *software = _openslide_tifflike_get_buffer(tl, 0,
                                                        TIFFTAG_SOFTWARE, err);
  if (!software) {
    return false;
  }
  if (!g_str_has_prefix(software, TRESTLE_SOFTWARE)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a Trestle slide");
    return false;
  }

  // check for ImageDescription field
  if (!_openslide_tifflike_get_buffer(tl, 0, TIFFTAG_IMAGEDESCRIPTION, err)) {
    return false;
  }

  // ensure all levels are tiled
  int64_t dirs = _openslide_tifflike_get_directory_count(tl);
  for (int64_t i = 0; i < dirs; i++) {
    if (!_openslide_tifflike_is_tiled(tl, i)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "TIFF level %"PRId64" is not tiled", i);
      return false;
    }
  }

  return true;
}

static void add_properties(openslide_t *osr, char **tags) {
  for (char **tag = tags; *tag != NULL; tag++) {
    g_auto(GStrv) pair = g_strsplit(*tag, "=", 2);
    if (pair) {
      char *name = g_strstrip(pair[0]);
      if (name) {
        char *value = g_strstrip(pair[1]);

        g_hash_table_insert(osr->properties,
                            g_strdup_printf("trestle.%s", name),
                            g_strdup(value));
      }
    }
  }

  _openslide_duplicate_int_prop(osr, "trestle.Objective Power",
                                OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
}

static void parse_trestle_image_description(openslide_t *osr,
                                            const char *description,
                                            int32_t *overlap_count_OUT,
                                            int32_t **overlaps_OUT) {
  g_auto(GStrv) first_pass = g_strsplit(description, ";", -1);
  add_properties(osr, first_pass);

  int32_t overlap_count = 0;
  g_autofree int32_t *overlaps = NULL;
  for (char **cur_str = first_pass; *cur_str != NULL; cur_str++) {
    //g_debug(" XX: %s", *cur_str);
    if (g_str_has_prefix(*cur_str, OVERLAPS_XY)) {
      // found it
      g_auto(GStrv) second_pass = g_strsplit(*cur_str, " ", -1);

      overlap_count = g_strv_length(second_pass) - 1; // skip fieldname
      overlaps = g_new(int32_t, overlap_count);

      int i = 0;
      // skip fieldname
      for (char **cur_str2 = second_pass + 1; *cur_str2 != NULL; cur_str2++) {
        overlaps[i] = g_ascii_strtoull(*cur_str2, NULL, 10);
        i++;
      }
    } else if (g_str_has_prefix(*cur_str, BACKGROUND_COLOR)) {
      // found background color
      errno = 0;
      uint64_t bg = g_ascii_strtoull((*cur_str) + strlen(BACKGROUND_COLOR), NULL, 16);
      if (bg || !errno) {
        _openslide_set_background_color_prop(osr,
                                             (bg >> 16) & 0xFF,
                                             (bg >> 8) & 0xFF,
                                             bg & 0xFF);
      }
    }
  }

  *overlap_count_OUT = overlap_count / 2;
  *overlaps_OUT = g_steal_pointer(&overlaps);
}

static char *get_associated_path(TIFF *tiff, const char *extension) {
  g_autofree char *base_path = g_strdup(TIFFFileName(tiff));

  // strip file extension, if present
  char *dot = g_strrstr(base_path, ".");
  if (dot != NULL) {
    *dot = 0;
  }

  return g_strdup_printf("%s%s", base_path, extension);
}

static void add_associated_jpeg(openslide_t *osr, TIFF *tiff,
                                const char *extension,
                                const char *name) {
  g_autofree char *path = get_associated_path(tiff, extension);
  _openslide_jpeg_add_associated_image(osr, name, path, 0, NULL);
}

static bool trestle_open(openslide_t *osr, const char *filename,
                         struct _openslide_tifflike *tl,
                         struct _openslide_hash *quickhash1, GError **err) {
  // open TIFF
  g_autoptr(_openslide_tiffcache) tc = _openslide_tiffcache_create(filename);
  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
  if (!ct.tiff) {
    return false;
  }

  // parse ImageDescription
  char *image_desc;
  int32_t overlap_count = 0;
  g_autofree int32_t *overlaps = NULL;
  if (!TIFFGetField(ct.tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read ImageDescription");
    return false;
  }
  parse_trestle_image_description(osr, image_desc, &overlap_count, &overlaps);

  // create levels
  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_with_free_func((GDestroyNotify) destroy_level);
  bool report_geometry = true;
  do {
    // verify that we can read this compression (hard fail if not)
    uint16_t compression;
    if (!TIFFGetField(ct.tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read compression scheme");
      return false;
    };
    if (!TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unsupported TIFF compression: %u", compression);
      return false;
    }

    struct level *l = g_slice_new0(struct level);
    struct _openslide_tiff_level *tiffl = &l->tiffl;
    g_ptr_array_add(level_array, l);

    // directories are linear
    tdir_t dir = TIFFCurrentDirectory(ct.tiff);
    if (!_openslide_tiff_level_init(ct.tiff, dir,
                                    (struct _openslide_level *) l, tiffl,
                                    err)) {
      return false;
    }

    // get overlaps
    int32_t overlap_x = 0;
    int32_t overlap_y = 0;
    if (dir < overlap_count) {
      overlap_x = overlaps[2 * dir];
      overlap_y = overlaps[2 * dir + 1];
      // if any level has overlaps, reporting tile advances would mislead the
      // application
      if (overlap_x || overlap_y) {
        report_geometry = false;
      }
    }

    // subtract out the overlaps (there are tiles-1 overlaps in each dimension)
    if (tiffl->image_w >= tiffl->tile_w) {
      l->base.w -= (tiffl->tiles_across - 1) * overlap_x;
    }
    if (tiffl->image_h >= tiffl->tile_h) {
      l->base.h -= (tiffl->tiles_down - 1) * overlap_y;
    }

    // create grid
    l->grid = _openslide_grid_create_tilemap(osr,
                                             tiffl->tile_w - overlap_x,
                                             tiffl->tile_h - overlap_y,
                                             read_tile, NULL);

    // add tiles
    for (int64_t y = 0; y < tiffl->tiles_down; y++) {
      for (int64_t x = 0; x < tiffl->tiles_across; x++) {
        _openslide_grid_tilemap_add_tile(l->grid,
                                         x, y,
                                         0, 0,
                                         tiffl->tile_w, tiffl->tile_h,
                                         NULL);
      }
    }
  } while (TIFFReadDirectory(ct.tiff));

  // clear tile size hints if necessary
  if (!report_geometry) {
    for (guint i = 0; i < level_array->len; i++) {
      struct level *l = level_array->pdata[i];
      l->base.tile_w = 0;
      l->base.tile_h = 0;
    }
  }

  // set hash and properties
  struct level *top_level = level_array->pdata[level_array->len - 1];
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    top_level->tiffl.dir,
                                                    0,
                                                    err)) {
    return false;
  }

  // create ops data
  struct trestle_ops_data *data = g_slice_new0(struct trestle_ops_data);
  data->tc = g_steal_pointer(&tc);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = level_array->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &trestle_ops;

  // copy the TIFF resolution props to the standard MPP properties
  // this is a totally non-standard use of these TIFF tags
  _openslide_duplicate_double_prop(osr, "tiff.XResolution",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr, "tiff.YResolution",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);

  // add associated images
  add_associated_jpeg(osr, ct.tiff, ".Full", "macro");

  return true;
}

const struct _openslide_format _openslide_format_trestle = {
  .name = "trestle",
  .vendor = "trestle",
  .detect = trestle_detect,
  .open = trestle_open,
};
