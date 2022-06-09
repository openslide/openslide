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
 * Aperio (svs, tif) support
 *
 * quickhash comes from _openslide_tifflike_init_properties_and_hash
 *
 */


#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-jp2k.h"
#include "openslide-decode-tiff.h"
#include "openslide-decode-tifflike.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <tiffio.h>

static const char APERIO_DESCRIPTION[] = "Aperio";

#define APERIO_COMPRESSION_JP2K_YCBCR 33003
#define APERIO_COMPRESSION_JP2K_RGB   33005

struct aperio_ops_data {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
  struct level *prev;
  GHashTable *missing_tiles;
  uint16_t compression;
};

static void destroy_data(struct aperio_ops_data *data,
                         struct level **levels, int32_t level_count) {
  if (data) {
    _openslide_tiffcache_destroy(data->tc);
    g_slice_free(struct aperio_ops_data, data);
  }

  if (levels) {
    for (int32_t i = 0; i < level_count; i++) {
      if (levels[i]) {
        if (levels[i]->missing_tiles) {
          g_hash_table_destroy(levels[i]->missing_tiles);
        }
        _openslide_grid_destroy(levels[i]->grid);
        g_slice_free(struct level, levels[i]);
      }
    }
    g_free(levels);
  }
}

static void destroy(openslide_t *osr) {
  struct aperio_ops_data *data = osr->data;
  struct level **levels = (struct level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}

static bool render_missing_tile(struct level *l,
                                TIFF *tiff,
                                uint32_t *dest,
                                int64_t tile_col, int64_t tile_row,
                                GError **err) {
  bool success = true;

  int64_t tw = l->tiffl.tile_w;
  int64_t th = l->tiffl.tile_h;

  // always fill with transparent (needed for SATURATE)
  memset(dest, 0, tw * th * 4);

  if (l->prev) {
    // recurse into previous level
    double relative_ds = l->prev->base.downsample / l->base.downsample;

    cairo_surface_t *surface =
      cairo_image_surface_create_for_data((unsigned char *) dest,
                                          CAIRO_FORMAT_ARGB32,
                                          tw, th, tw * 4);
    cairo_t *cr = cairo_create(surface);
    cairo_surface_destroy(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SATURATE);
    cairo_translate(cr, -1, -1);
    cairo_scale(cr, relative_ds, relative_ds);

    // For the usual case that we are on a tile boundary in the previous
    // level, extend the region by one pixel in each direction to ensure we
    // paint the surrounding tiles.  This reduces the visible seam that
    // would otherwise occur with non-integer downsamples.
    success = _openslide_grid_paint_region(l->prev->grid, cr, tiff,
                                           (tile_col * tw - 1) / relative_ds,
                                           (tile_row * th - 1) / relative_ds,
                                           (struct _openslide_level *) l->prev,
                                           ceil((tw + 2) / relative_ds),
                                           ceil((th + 2) / relative_ds),
                                           err);
    if (success) {
      success = _openslide_check_cairo_status(cr, err);
    }
    cairo_destroy(cr);
  }

  return success;
}

static bool decode_tile(struct level *l,
                        TIFF *tiff,
                        uint32_t *dest,
                        int64_t tile_col, int64_t tile_row,
                        GError **err) {
  struct _openslide_tiff_level *tiffl = &l->tiffl;

  // check for missing tile
  int64_t tile_no = tile_row * tiffl->tiles_across + tile_col;
  if (g_hash_table_lookup_extended(l->missing_tiles, &tile_no, NULL, NULL)) {
    //g_debug("missing tile in level %p: (%"PRId64", %"PRId64")", (void *) l, tile_col, tile_row);
    return render_missing_tile(l, tiff, dest,
                               tile_col, tile_row, err);
  }

  // select color space
  enum _openslide_jp2k_colorspace space;
  switch (l->compression) {
  case APERIO_COMPRESSION_JP2K_YCBCR:
    space = OPENSLIDE_JP2K_YCBCR;
    break;
  case APERIO_COMPRESSION_JP2K_RGB:
    space = OPENSLIDE_JP2K_RGB;
    break;
  default:
    // not for us? fallback
    return _openslide_tiff_read_tile(tiffl, tiff, dest,
                                     tile_col, tile_row,
                                     err);
  }

  // read raw tile
  void *buf;
  int32_t buflen;
  if (!_openslide_tiff_read_tile_data(tiffl, tiff,
                                      &buf, &buflen,
                                      tile_col, tile_row,
                                      err)) {
    return false;  // ok, haven't allocated anything yet
  }

  // decompress
  bool success = _openslide_jp2k_decode_buffer(dest,
                                               tiffl->tile_w, tiffl->tile_h,
                                               buf, buflen,
                                               space,
                                               err);

  // clean up
  g_free(buf);

  return success;
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
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level, tile_col, tile_row,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    if (!decode_tile(l, tiff, tiledata, tile_col, tile_row, err)) {
      g_slice_free1(tw * th * 4, tiledata);
      return false;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, tiledata,
                                   tile_col, tile_row,
                                   err)) {
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
  struct aperio_ops_data *data = osr->data;
  struct level *l = (struct level *) level;

  TIFF *tiff = _openslide_tiffcache_get(data->tc, err);
  if (tiff == NULL) {
    return false;
  }

  bool success = _openslide_grid_paint_region(l->grid, cr, tiff,
                                              x / l->base.downsample,
                                              y / l->base.downsample,
                                              level, w, h,
                                              err);
  _openslide_tiffcache_put(data->tc, tiff);

  return success;
}

static const struct _openslide_ops aperio_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool aperio_detect(const char *filename G_GNUC_UNUSED,
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

  // check ImageDescription
  const char *tagval = _openslide_tifflike_get_buffer(tl, 0,
                                                      TIFFTAG_IMAGEDESCRIPTION,
                                                      err);
  if (!tagval) {
    return false;
  }
  if (!g_str_has_prefix(tagval, APERIO_DESCRIPTION)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Not an Aperio slide");
    return false;
  }

  return true;
}

static void add_properties(openslide_t *osr, char **props) {
  if (*props == NULL) {
    return;
  }

  // ignore first property in Aperio
  for(char **p = props + 1; *p != NULL; p++) {
    char **pair = g_strsplit(*p, "=", 2);

    if (pair) {
      char *name = g_strstrip(pair[0]);
      if (name) {
	char *value = g_strstrip(pair[1]);

	g_hash_table_insert(osr->properties,
			    g_strdup_printf("aperio.%s", name),
			    g_strdup(value));
      }
    }
    g_strfreev(pair);
  }

  _openslide_duplicate_int_prop(osr, "aperio.AppMag",
                                OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_double_prop(osr, "aperio.MPP",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(osr, "aperio.MPP",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);
}

// add the image from the current TIFF directory
// returns false and sets GError if fatal error
// true does not necessarily imply an image was added
static bool add_associated_image(openslide_t *osr,
                                 const char *name_if_available,
                                 struct _openslide_tiffcache *tc,
                                 TIFF *tiff,
                                 GError **err) {
  char *name = NULL;
  if (name_if_available) {
    name = g_strdup(name_if_available);
  } else {
    char *val;

    // get name
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &val)) {
      return true;
    }

    // parse ImageDescription, after newline up to first whitespace -> gives name
    char **lines = g_strsplit_set(val, "\r\n", -1);
    if (!lines) {
      return true;
    }

    if (lines[0] && lines[1]) {
      char *line = lines[1];

      char **names = g_strsplit(line, " ", -1);
      if (names && names[0]) {
	name = g_strdup(names[0]);
      }
      g_strfreev(names);
    }

    g_strfreev(lines);
  }

  if (!name) {
    return true;
  }

  bool result = _openslide_tiff_add_associated_image(osr, name, tc,
                                                     TIFFCurrentDirectory(tiff),
                                                     err);
  g_free(name);
  return result;
}

static void propagate_missing_tile(void *key, void *value G_GNUC_UNUSED,
                                   void *data) {
  const int64_t *tile_no = key;
  struct level *next_l = data;
  struct level *l = next_l->prev;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  struct _openslide_tiff_level *next_tiffl = &next_l->tiffl;

  int64_t tile_col = *tile_no % tiffl->tiles_across;
  int64_t tile_row = *tile_no / tiffl->tiles_across;

  int64_t tile_concat_x = round((double) tiffl->tiles_across /
                                next_tiffl->tiles_across);
  int64_t tile_concat_y = round((double) tiffl->tiles_down /
                                next_tiffl->tiles_down);

  int64_t next_tile_col = tile_col / tile_concat_x;
  int64_t next_tile_row = tile_row / tile_concat_y;

  //g_debug("propagating %p (%"PRId64", %"PRId64") to %p (%"PRId64", %"PRId64")", (void *) l, tile_col, tile_row, (void *) next_l, next_tile_col, next_tile_row);

  int64_t *next_tile_no = g_new(int64_t, 1);
  *next_tile_no = next_tile_row * next_tiffl->tiles_across + next_tile_col;
  g_hash_table_insert(next_l->missing_tiles, next_tile_no, NULL);
}

static bool aperio_open(openslide_t *osr,
                        const char *filename,
                        struct _openslide_tifflike *tl,
                        struct _openslide_hash *quickhash1, GError **err) {
  struct aperio_ops_data *data = NULL;
  struct level **levels = NULL;
  int32_t level_count = 0;

  // open TIFF
  struct _openslide_tiffcache *tc = _openslide_tiffcache_create(filename);
  TIFF *tiff = _openslide_tiffcache_get(tc, err);
  if (!tiff) {
    goto FAIL;
  }

  /*
   * http://www.aperio.com/documents/api/Aperio_Digital_Slides_and_Third-party_data_interchange.pdf
   * page 14:
   *
   * The first image in an SVS file is always the baseline image (full
   * resolution). This image is always tiled, usually with a tile size
   * of 240 x 240 pixels. The second image is always a thumbnail,
   * typically with dimensions of about 1024 x 768 pixels. Unlike the
   * other slide images, the thumbnail image is always
   * stripped. Following the thumbnail there may be one or more
   * intermediate "pyramid" images. These are always compressed with
   * the same type of compression as the baseline image, and have a
   * tiled organization with the same tile size.
   *
   * Optionally at the end of an SVS file there may be a slide label
   * image, which is a low resolution picture taken of the slide’s
   * label, and/or a macro camera image, which is a low resolution
   * picture taken of the entire slide. The label and macro images are
   * always stripped.
   */

  do {
    // for aperio, the tiled directories are the ones we want
    if (TIFFIsTiled(tiff)) {
      level_count++;
    }

    // check depth
    uint32_t depth;
    if (TIFFGetField(tiff, TIFFTAG_IMAGEDEPTH, &depth) &&
        depth != 1) {
      // we can't handle depth != 1
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot handle ImageDepth=%d", depth);
      goto FAIL;
    }

    // check compression
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read compression scheme");
      goto FAIL;
    }
    if ((compression != APERIO_COMPRESSION_JP2K_YCBCR) &&
        (compression != APERIO_COMPRESSION_JP2K_RGB) &&
        !TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unsupported TIFF compression: %u", compression);
      goto FAIL;
    }
  } while (TIFFReadDirectory(tiff));

  // allocate private data
  data = g_slice_new0(struct aperio_ops_data);

  levels = g_new0(struct level *, level_count);
  int32_t i = 0;
  if (!_openslide_tiff_set_dir(tiff, 0, err)) {
    goto FAIL;
  }
  do {
    tdir_t dir = TIFFCurrentDirectory(tiff);
    if (TIFFIsTiled(tiff)) {
      //g_debug("tiled directory: %d", dir);
      struct level *l = g_slice_new0(struct level);
      struct _openslide_tiff_level *tiffl = &l->tiffl;
      if (i) {
        l->prev = levels[i - 1];
      }
      levels[i++] = l;

      if (!_openslide_tiff_level_init(tiff,
                                      dir,
                                      (struct _openslide_level *) l,
                                      tiffl,
                                      err)) {
        goto FAIL;
      }

      l->grid = _openslide_grid_create_simple(osr,
                                              tiffl->tiles_across,
                                              tiffl->tiles_down,
                                              tiffl->tile_w,
                                              tiffl->tile_h,
                                              read_tile);

      // get compression
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &l->compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Can't read compression scheme");
        goto FAIL;
      }

      // some Aperio slides have some zero-length tiles, apparently due to
      // an encoder bug
      toff_t *tile_sizes;
      if (!TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &tile_sizes)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Cannot get tile sizes");
        goto FAIL;
      }
      l->missing_tiles = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                               g_free, NULL);
      for (ttile_t tile_no = 0;
           tile_no < tiffl->tiles_across * tiffl->tiles_down; tile_no++) {
        if (tile_sizes[tile_no] == 0) {
          int64_t *p_tile_no = g_new(int64_t, 1);
          *p_tile_no = tile_no;
          g_hash_table_insert(l->missing_tiles, p_tile_no, NULL);
        }
      }
    } else {
      // associated image
      const char *name = (dir == 1) ? "thumbnail" : NULL;
      if (!add_associated_image(osr, name, tc, tiff, err)) {
	goto FAIL;
      }
      //g_debug("associated image: %d", dir);
    }
  } while (TIFFReadDirectory(tiff));

  // tiles concatenating a missing tile are sometimes corrupt, so we mark
  // them missing too
  for (i = 0; i < level_count - 1; i++) {
    g_hash_table_foreach(levels[i]->missing_tiles, propagate_missing_tile,
                         levels[i + 1]);
  }

  // read properties
  if (!_openslide_tiff_set_dir(tiff, 0, err)) {
    goto FAIL;
  }
  char *image_desc;
  if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &image_desc)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read ImageDescription field");
    goto FAIL;
  }
  char **props = g_strsplit(image_desc, "|", -1);
  add_properties(osr, props);
  g_strfreev(props);

  // set hash and properties
  if (!_openslide_tifflike_init_properties_and_hash(osr, tl, quickhash1,
                                                    levels[level_count - 1]->tiffl.dir,
                                                    0,
                                                    err)) {
    goto FAIL;
  }

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &aperio_ops;

  // put TIFF handle and store tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  return true;

FAIL:
  destroy_data(data, levels, level_count);
  _openslide_tiffcache_put(tc, tiff);
  _openslide_tiffcache_destroy(tc);
  return false;
}

const struct _openslide_format _openslide_format_aperio = {
  .name = "aperio",
  .vendor = "aperio",
  .detect = aperio_detect,
  .open = aperio_open,
};
