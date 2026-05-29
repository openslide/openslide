/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2026 Benjamin Gilbert
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
 * Huron (tif) support.  Very similar to Aperio SVS.
 *
 * quickhash comes from _openslide_tifflike_init_properties_and_hash
 *
 */

#include "openslide-private.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <tiffio.h>

static const char HURON_MAKE[] = "Huron";

struct huron_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

static void destroy_level(struct level *l) {
  _openslide_grid_destroy(l->grid);
  g_free(l);
}
OPENSLIDE_DEFINE_G_DESTROY_NOTIFY_WRAPPER(destroy_level)

static void destroy(openslide_t *osr) {
  for (int32_t i = 0; i < osr->level_count; i++) {
    destroy_level((struct level *) osr->levels[i]);
  }
  g_free(osr->levels);

  struct huron_ops_data *data = osr->data;
  _openslide_tiffcache_destroy(data->tc);
  g_free(data);
}

static bool read_tile(openslide_t *osr,
		      cairo_t *cr,
		      struct _openslide_level *level,
		      int64_t tile_col, int64_t tile_row,
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
    g_autofree uint32_t *buf = g_new(uint32_t, tw * th);
    if (!_openslide_tiff_read_tile(&l->tiffl, tiff, buf,
                                   tile_col, tile_row, err)) {
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, buf,
                                   tile_col, tile_row,
                                   err)) {
      return false;
    }

    // put it in the cache
    tiledata = g_steal_pointer(&buf);
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
  struct huron_ops_data *data = osr->data;
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

static const struct _openslide_ops huron_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool huron_detect(const char *filename G_GNUC_UNUSED,
                         struct _openslide_tifflike *tl, GError **err) {
  // ensure we have a TIFF
  if (!tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a TIFF file");
    return false;
  }

  // ensure TIFF is tiled
  if (!_openslide_tifflike_is_tiled(tl, 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFF is not tiled");
    return false;
  }

  // check ImageMake
  const char *tagval = _openslide_tifflike_get_buffer(tl, 0, TIFFTAG_MAKE, err);
  if (!tagval) {
    return false;
  }
  if (!g_str_has_prefix(tagval, HURON_MAKE)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not a Huron slide");
    return false;
  }

  return true;
}

static bool add_properties(openslide_t *osr, TIFF *tiff, GError **err) {
  char *image_desc;
  if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
    _openslide_tiff_error(err, tiff, "Couldn't read ImageDescription field");
    return false;
  }
  g_auto(GStrv) props = g_strsplit(image_desc, "\n", -1);
  for (char **p = props; *p != NULL; p++) {
    g_auto(GStrv) pair = g_strsplit(*p, "=", 2);
    if (pair[0] && pair[1]) {
      g_strstrip(pair[0]);
      g_strstrip(pair[1]);
      g_hash_table_insert(osr->properties,
                          g_strdup_printf("huron.%s", pair[0]),
                          g_steal_pointer(&pair[1]));
    }
  }
  return true;
}

static bool huron_open(openslide_t *osr,
                       const char *filename,
                       struct _openslide_tifflike *tl,
                       struct _openslide_hash *quickhash1, GError **err) {
  // open TIFF
  g_autoptr(_openslide_tiffcache) tc = _openslide_tiffcache_create(filename);
  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
  if (!ct.tiff) {
    return false;
  }

  g_autoptr(GPtrArray) level_array =
    g_ptr_array_new_with_free_func(OPENSLIDE_G_DESTROY_NOTIFY_WRAPPER(destroy_level));
  do {
    // check depth
    uint32_t depth;
    if (TIFFGetField(ct.tiff, TIFFTAG_IMAGEDEPTH, &depth) &&
        depth != 1) {
      // we can't handle depth != 1
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot handle ImageDepth=%d", depth);
      return false;
    }

    // check compression
    uint16_t compression;
    if (!TIFFGetField(ct.tiff, TIFFTAG_COMPRESSION, &compression)) {
      _openslide_tiff_error(err, ct.tiff, "Can't read compression scheme");
      return false;
    }
    if (!TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unsupported TIFF compression: %u", compression);
      return false;
    }

    tdir_t dir = TIFFCurrentDirectory(ct.tiff);
    if (TIFFIsTiled(ct.tiff)) {
      //g_debug("tiled directory: %d", dir);
      struct level *l = g_new0(struct level, 1);
      struct _openslide_tiff_level *tiffl = &l->tiffl;
      g_ptr_array_add(level_array, l);

      if (!_openslide_tiff_level_init(ct.tiff,
                                      dir,
                                      (struct _openslide_level *) l,
                                      tiffl,
                                      err)) {
        return false;
      }

      l->grid = _openslide_grid_create_simple(osr,
                                              tiffl->tiles_across,
                                              tiffl->tiles_down,
                                              tiffl->tile_w,
                                              tiffl->tile_h,
                                              read_tile);

    } else {
      // associated image
      const char *name = NULL;
      if (dir == 1) {
        name = "thumbnail";
      } else {
        uint32_t subfile;
        if (TIFFGetField(ct.tiff, TIFFTAG_SUBFILETYPE, &subfile)) {
          switch (subfile) {
          case 1:
            name = "label";
            break;
          case 9:
            name = "macro";
            break;
          }
        }
      }
      if (name &&
          !_openslide_tiff_add_associated_image(osr, name, tc, dir, NULL,
                                                err)) {
	return false;
      }
      //g_debug("associated image: %d", dir);
    }
  } while (TIFFReadDirectory(ct.tiff));

  // read properties
  if (!_openslide_tiff_set_dir(ct.tiff, 0, err)) {
    return false;
  }
  if (!add_properties(osr, ct.tiff, err)) {
    return false;
  }
  _openslide_tifflike_set_resolution_props(osr, tl, 0);

  // set hash and TIFF properties
  struct level *top_level = level_array->pdata[level_array->len - 1];
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    top_level->tiffl.dir,
                                                    0,
                                                    err)) {
    return false;
  }
  // keep the ImageDescription out of the properties
  g_hash_table_remove(osr->properties, OPENSLIDE_PROPERTY_NAME_COMMENT);
  g_hash_table_remove(osr->properties, "tiff.ImageDescription");

  // allocate private data
  struct huron_ops_data *data = g_new0(struct huron_ops_data, 1);
  data->tc = g_steal_pointer(&tc);

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->level_count = level_array->len;
  osr->levels = (struct _openslide_level **)
    g_ptr_array_free(g_steal_pointer(&level_array), false);
  osr->data = data;
  osr->ops = &huron_ops;

  return true;
}

const struct _openslide_format _openslide_format_huron = {
  .name = "huron",
  .vendor = "huron",
  .detect = huron_detect,
  .open = huron_open,
};
