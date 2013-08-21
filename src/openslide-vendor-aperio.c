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
 * quickhash comes from _openslide_tiff_init_properties_and_hash
 *
 */


#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
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
  uint16_t compression;
};

static void destroy_data(struct aperio_ops_data *data,
                         struct level **levels, int32_t level_count) {
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct aperio_ops_data, data);

  for (int32_t i = 0; i < level_count; i++) {
    if (levels[i]) {
      _openslide_grid_destroy(levels[i]->grid);
      g_slice_free(struct level, levels[i]);
    }
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  struct aperio_ops_data *data = osr->data;
  struct level **levels = (struct level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}

static bool decode_tile(struct level *l,
                        TIFF *tiff,
                        uint32_t *dest,
                        int64_t tile_col, int64_t tile_row,
                        GError **err) {
  struct _openslide_tiff_level *tiffl = &l->tiffl;

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
  if (!buflen) {
    // a slide with zero-length tiles has been seen in the wild
    // fill with transparent
    memset(dest, 0, tiffl->tile_w * tiffl->tile_h * 4);
    return true;  // ok, haven't allocated anything yet
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

static void read_tile(openslide_t *osr,
		      cairo_t *cr,
		      struct _openslide_level *level,
		      struct _openslide_grid *grid,
		      int64_t tile_col, int64_t tile_row,
		      void *arg) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;
  GError *tmp_err = NULL;

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
    if (!decode_tile(l, tiff, tiledata, tile_col, tile_row, &tmp_err)) {
      _openslide_set_error_from_gerror(osr, tmp_err);
      g_clear_error(&tmp_err);
      g_slice_free1(tw * th * 4, tiledata);
      return;
    }

    // clip, if necessary
    if (!_openslide_tiff_clip_tile(tiffl, tiledata,
                                   tile_col, tile_row,
                                   &tmp_err)) {
      _openslide_set_error_from_gerror(osr, tmp_err);
      g_clear_error(&tmp_err);
      g_slice_free1(tw * th * 4, tiledata);
      return;
    }

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
  struct aperio_ops_data *data = osr->data;
  struct level *l = (struct level *) level;
  GError *tmp_err = NULL;

  TIFF *tiff = _openslide_tiffcache_get(data->tc, &tmp_err);
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
    _openslide_set_error_from_gerror(osr, tmp_err);
    g_clear_error(&tmp_err);
  }
  _openslide_tiffcache_put(data->tc, tiff);
}


static const struct _openslide_ops aperio_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static void add_properties(GHashTable *ht, char **props) {
  if (*props == NULL) {
    return;
  }

  g_hash_table_insert(ht,
		      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
		      g_strdup("aperio"));

  // ignore first property in Aperio
  for(char **p = props + 1; *p != NULL; p++) {
    char **pair = g_strsplit(*p, "=", 2);

    if (pair) {
      char *name = g_strstrip(pair[0]);
      if (name) {
	char *value = g_strstrip(pair[1]);

	g_hash_table_insert(ht,
			    g_strdup_printf("aperio.%s", name),
			    g_strdup(value));
      }
    }
    g_strfreev(pair);
  }

  _openslide_duplicate_int_prop(ht, "aperio.AppMag",
                                OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_double_prop(ht, "aperio.MPP",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(ht, "aperio.MPP",
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


bool _openslide_try_aperio(openslide_t *osr,
			   struct _openslide_tiffcache *tc, TIFF *tiff,
			   struct _openslide_hash *quickhash1,
			   GError **err) {
  if (!TIFFIsTiled(tiff)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "TIFF is not tiled");
    return false;
  }

  char *tagval;
  int tiff_result;
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (!tiff_result ||
      (strncmp(APERIO_DESCRIPTION, tagval, strlen(APERIO_DESCRIPTION)) != 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not an Aperio slide");
    return false;
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

  int32_t level_count = 0;
  do {
    // for aperio, the tiled directories are the ones we want
    if (TIFFIsTiled(tiff)) {
      level_count++;
    }

    // check depth
    uint32_t depth;
    tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDEPTH, &depth);
    if (tiff_result && depth != 1) {
      // we can't handle depth != 1
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot handle ImageDepth=%d", depth);
      return false;
    }

    // check compression
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read compression scheme");
      return false;
    }
    if ((compression != APERIO_COMPRESSION_JP2K_YCBCR) &&
        (compression != APERIO_COMPRESSION_JP2K_RGB) &&
        !TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Unsupported TIFF compression: %u", compression);
      return false;
    }
  } while (TIFFReadDirectory(tiff));

  // allocate private data
  struct aperio_ops_data *data = g_slice_new0(struct aperio_ops_data);

  struct level **levels = g_new0(struct level *, level_count);
  int32_t i = 0;
  TIFFSetDirectory(tiff, 0);
  do {
    tdir_t dir = TIFFCurrentDirectory(tiff);
    if (TIFFIsTiled(tiff)) {
      //g_debug("tiled directory: %d", dir);
      struct level *l = g_slice_new0(struct level);
      struct _openslide_tiff_level *tiffl = &l->tiffl;
      levels[i++] = l;

      if (!_openslide_tiff_level_init(tiff,
                                      dir,
                                      (struct _openslide_level *) l,
                                      tiffl,
                                      err)) {
        destroy_data(data, levels, level_count);
        return false;
      }

      l->grid = _openslide_grid_create_simple(osr,
                                              tiffl->tiles_across,
                                              tiffl->tiles_down,
                                              tiffl->tile_w,
                                              tiffl->tile_h,
                                              read_tile);

      // get compression
      if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &l->compression)) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Can't read compression scheme");
        destroy_data(data, levels, level_count);
        return false;
      }
    } else {
      // associated image
      const char *name = (dir == 1) ? "thumbnail" : NULL;
      if (!add_associated_image(osr, name, tc, tiff, err)) {
	g_prefix_error(err, "Can't read associated image: ");
	destroy_data(data, levels, level_count);
	return false;
      }
      //g_debug("associated image: %d", dir);
    }
  } while (TIFFReadDirectory(tiff));

  if (osr == NULL) {
    // free now and return
    _openslide_tiffcache_put(tc, tiff);
    data->tc = tc;
    destroy_data(data, levels, level_count);
    return true;
  }

  // read properties
  TIFFSetDirectory(tiff, 0);
  TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval); // XXX? should be safe, we just did it
  char **props = g_strsplit(tagval, "|", -1);
  add_properties(osr->properties, props);
  g_strfreev(props);

  // set hash and properties
  if (!_openslide_tiff_init_properties_and_hash(osr, tiff, quickhash1,
                                                levels[level_count - 1]->tiffl.dir,
                                                0,
                                                err)) {
    destroy_data(data, levels, level_count);
    return false;
  }

  // store osr data
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &aperio_ops;

  // put TIFF handle and assume tiffcache reference
  _openslide_tiffcache_put(tc, tiff);
  data->tc = tc;

  return true;
}
