/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2012 Pathomation
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
 * MIRAX (mrxs) support
 *
 * quickhash comes from the slidedat file and the lowest resolution datafile
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-gdkpixbuf.h"
#include "openslide-decode-jpeg.h"
#include "openslide-decode-png.h"

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <zlib.h>

#include "openslide-hash.h"

static const char MRXS_EXT[] = ".mrxs";
static const char SLIDEDAT_INI[] = "Slidedat.ini";
static const int SLIDEDAT_MAX_SIZE = 1 << 20;

static const char GROUP_GENERAL[] = "GENERAL";
static const char KEY_SLIDE_VERSION[] = "SLIDE_VERSION";
static const char KEY_SLIDE_ID[] = "SLIDE_ID";
static const char KEY_IMAGENUMBER_X[] = "IMAGENUMBER_X";
static const char KEY_IMAGENUMBER_Y[] = "IMAGENUMBER_Y";
static const char KEY_OBJECTIVE_MAGNIFICATION[] = "OBJECTIVE_MAGNIFICATION";
static const char KEY_CAMERA_IMAGE_DIVISIONS_PER_SIDE[] = "CameraImageDivisionsPerSide";

static const char GROUP_HIERARCHICAL[] = "HIERARCHICAL";
static const char KEY_HIER_COUNT[] = "HIER_COUNT";
static const char KEY_NONHIER_COUNT[] = "NONHIER_COUNT";
static const char KEY_INDEXFILE[] = "INDEXFILE";
static const char INDEX_VERSION[] = "01.02";
static const char KEY_HIER_d_NAME[] = "HIER_%d_NAME";
static const char KEY_HIER_d_COUNT[] = "HIER_%d_COUNT";
static const char KEY_HIER_d_VAL_d_SECTION[] = "HIER_%d_VAL_%d_SECTION";
static const char KEY_NONHIER_d_NAME[] = "NONHIER_%d_NAME";
static const char KEY_NONHIER_d_COUNT[] = "NONHIER_%d_COUNT";
static const char KEY_NONHIER_d_VAL_d[] = "NONHIER_%d_VAL_%d";
static const char KEY_NONHIER_d_VAL_d_SECTION[] = "NONHIER_%d_VAL_%d_SECTION";
static const char KEY_MACRO_IMAGE_TYPE[] = "THUMBNAIL_IMAGE_TYPE";
static const char KEY_LABEL_IMAGE_TYPE[] = "BARCODE_IMAGE_TYPE";
static const char KEY_THUMBNAIL_IMAGE_TYPE[] = "PREVIEW_IMAGE_TYPE";
static const char VALUE_VIMSLIDE_POSITION_BUFFER[] = "VIMSLIDE_POSITION_BUFFER";
static const char VALUE_STITCHING_INTENSITY_LAYER[] = "StitchingIntensityLayer";
static const char VALUE_SCAN_DATA_LAYER[] = "Scan data layer";
static const char VALUE_SCAN_DATA_LAYER_MACRO[] = "ScanDataLayer_SlideThumbnail";
static const char VALUE_SCAN_DATA_LAYER_LABEL[] = "ScanDataLayer_SlideBarcode";
static const char VALUE_SCAN_DATA_LAYER_THUMBNAIL[] = "ScanDataLayer_SlidePreview";
static const char VALUE_SLIDE_ZOOM_LEVEL[] = "Slide zoom level";

static const int SLIDE_POSITION_RECORD_SIZE = 9;

static const char GROUP_DATAFILE[] = "DATAFILE";
static const char KEY_FILE_COUNT[] = "FILE_COUNT";
static const char KEY_d_FILE[] = "FILE_%d";

static const char KEY_OVERLAP_X[] = "OVERLAP_X";
static const char KEY_OVERLAP_Y[] = "OVERLAP_Y";
static const char KEY_MPP_X[] = "MICROMETER_PER_PIXEL_X";
static const char KEY_MPP_Y[] = "MICROMETER_PER_PIXEL_Y";
static const char KEY_IMAGE_FORMAT[] = "IMAGE_FORMAT";
static const char KEY_IMAGE_FILL_COLOR_BGR[] = "IMAGE_FILL_COLOR_BGR";
static const char KEY_DIGITIZER_WIDTH[] = "DIGITIZER_WIDTH";
static const char KEY_DIGITIZER_HEIGHT[] = "DIGITIZER_HEIGHT";
static const char KEY_IMAGE_CONCAT_FACTOR[] = "IMAGE_CONCAT_FACTOR";

#define READ_KEY_OR_FAIL(TARGET, KEYFILE, GROUP, KEY, TYPE)		\
  do {									\
    GError *tmp_err = NULL;						\
    TARGET = g_key_file_get_ ## TYPE(KEYFILE, GROUP, KEY, &tmp_err);	\
    if (tmp_err != NULL) {						\
      g_propagate_error(err, tmp_err);					\
      goto FAIL;							\
    }									\
  } while(0)

#define HAVE_GROUP_OR_FAIL(KEYFILE, GROUP)				\
  do {									\
    if (!g_key_file_has_group(slidedat, GROUP)) {			\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,		\
                  "Can't find %s group", GROUP);			\
      goto FAIL;							\
    }									\
  } while(0)

#define SUCCESSFUL_OR_FAIL(TMP_ERR)				\
  do {								\
    if (TMP_ERR) {						\
      g_propagate_error(err, TMP_ERR);				\
      goto FAIL;						\
    }								\
  } while(0)

#define POSITIVE_OR_FAIL(N)					\
  do {								\
    if (N <= 0) {						\
      g_set_error(err, OPENSLIDE_ERROR,				\
                  OPENSLIDE_ERROR_FAILED, #N " <= 0: %d", N);	\
      goto FAIL;						\
    }								\
  } while(0)

#define NON_NEGATIVE_OR_FAIL(N)					\
  do {								\
    if (N < 0) {						\
      g_set_error(err, OPENSLIDE_ERROR,				\
                  OPENSLIDE_ERROR_FAILED, #N " < 0: %d", N);	\
      goto FAIL;						\
    }								\
  } while(0)

enum image_format {
  FORMAT_UNKNOWN,
  FORMAT_JPEG,
  FORMAT_PNG,
  FORMAT_BMP,
};

struct slide_zoom_level_section {
  int concat_exponent;

  double overlap_x;
  double overlap_y;
  double mpp_x;
  double mpp_y;

  uint32_t fill_rgb;

  enum image_format image_format;
  int image_w;
  int image_h;
};

// see comments in _openslide_try_mirax()
struct slide_zoom_level_params {
  int image_concat;
  int tile_count_divisor;
  int tiles_per_image;
  int positions_per_tile;
  double tile_advance_x;
  double tile_advance_y;
};

struct image {
  int32_t fileno;
  int32_t start_in_file;
  int32_t length;
  int32_t imageno;   // used only for cache lookup
  int refcount;
};

struct tile {
  struct image *image;

  // location in the image
  double src_x;
  double src_y;
};

struct level {
  struct _openslide_level base;
  struct _openslide_grid *grid;

  enum image_format image_format;
  int32_t image_width;
  int32_t image_height;

  double tile_w;
  double tile_h;
};

struct mirax_ops_data {
  gchar **datafile_paths;
};

static void image_unref(struct image *image) {
  if (!--image->refcount) {
    g_slice_free(struct image, image);
  }
}

static void tile_free(gpointer data) {
  struct tile *tile = data;
  image_unref(tile->image);
  g_slice_free(struct tile, tile);
}

static uint32_t *read_image(openslide_t *osr,
                            struct image *image,
                            enum image_format format,
                            int w, int h,
                            GError **err) {
  struct mirax_ops_data *data = osr->data;
  bool result = false;

  uint32_t *dest = g_slice_alloc(w * h * 4);

  switch (format) {
  case FORMAT_JPEG:
    result = _openslide_jpeg_read(data->datafile_paths[image->fileno],
                                  image->start_in_file,
                                  dest, w, h,
                                  err);
    break;
  case FORMAT_PNG:
    result = _openslide_png_read(data->datafile_paths[image->fileno],
                                 image->start_in_file,
                                 dest, w, h,
                                 err);
    break;
  case FORMAT_BMP:
    result = _openslide_gdkpixbuf_read("bmp",
                                       data->datafile_paths[image->fileno],
                                       image->start_in_file, image->length,
                                       dest, w, h,
                                       err);
    break;
  default:
    g_assert_not_reached();
  }

  if (!result) {
    g_slice_free1(w * h * 4, dest);
    return NULL;
  }
  return dest;
}

static bool read_tile(openslide_t *osr,
                      cairo_t *cr,
                      struct _openslide_level *level,
                      int64_t tile_col G_GNUC_UNUSED,
                      int64_t tile_row G_GNUC_UNUSED,
                      void *data,
                      void *arg G_GNUC_UNUSED,
                      GError **err) {
  struct level *l = (struct level *) level;
  struct tile *tile = data;
  bool success = true;

  int iw = l->image_width;
  int ih = l->image_height;

  //g_debug("mirax read_tile: src: %g %g, dim: %d %d, tile dim: %g %g, region %g %g %g %g", tile->src_x, tile->src_y, l->image_width, l->image_height, l->tile_w, l->tile_h, x, y, w, h);

  // get the image data, possibly from cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            level,
                                            tile->image->imageno,
                                            0,
                                            &cache_entry);

  if (!tiledata) {
    tiledata = read_image(osr, tile->image, l->image_format, iw, ih, err);
    if (tiledata == NULL) {
      return false;
    }

    _openslide_cache_put(osr->cache,
                         level, tile->image->imageno, 0,
                         tiledata,
                         iw * ih * 4,
                         &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                 CAIRO_FORMAT_RGB24,
                                                                 iw, ih,
                                                                 iw * 4);

  // if we are drawing a subregion of the tile, we must do an additional copy,
  // because cairo lacks source clipping
  if ((l->image_width > l->tile_w) ||
      (l->image_height > l->tile_h)) {
    cairo_surface_t *surface2 = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                           ceil(l->tile_w),
                                                           ceil(l->tile_h));
    cairo_t *cr2 = cairo_create(surface2);
    cairo_set_source_surface(cr2, surface, -tile->src_x, -tile->src_y);

    // replace original image surface
    cairo_surface_destroy(surface);
    surface = surface2;

    cairo_rectangle(cr2, 0, 0,
                    ceil(l->tile_w),
                    ceil(l->tile_h));
    cairo_fill(cr2);
    success = _openslide_check_cairo_status(cr2, err);
    cairo_destroy(cr2);
  }

  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);

  return success;
}

static bool paint_region(openslide_t *osr G_GNUC_UNUSED, cairo_t *cr,
                         int64_t x, int64_t y,
                         struct _openslide_level *level,
                         int32_t w, int32_t h,
                         GError **err) {
  struct level *l = (struct level *) level;

  return _openslide_grid_paint_region(l->grid, cr, NULL,
                                      x / level->downsample,
                                      y / level->downsample,
                                      level, w, h,
                                      err);
}

static void destroy(openslide_t *osr) {
  struct mirax_ops_data *data = osr->data;

  // each level in turn
  for (int32_t i = 0; i < osr->level_count; i++) {
    struct level *l = (struct level *) osr->levels[i];
    _openslide_grid_destroy(l->grid);
    g_slice_free(struct level, l);
  }

  // the level array
  g_free(osr->levels);

  // the ops data
  g_strfreev(data->datafile_paths);
  g_slice_free(struct mirax_ops_data, data);
}

static const struct _openslide_ops mirax_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

static bool mirax_detect(const char *filename, struct _openslide_tifflike *tl,
                         GError **err) {
  // reject TIFFs
  if (tl) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Is a TIFF file");
    return false;
  }

  // verify filename
  if (!g_str_has_suffix(filename, MRXS_EXT)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not have %s extension", MRXS_EXT);
    return false;
  }

  // verify existence
  if (!g_file_test(filename, G_FILE_TEST_EXISTS)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "File does not exist");
    return false;
  }

  // verify slidedat exists
  char *dirname = g_strndup(filename, strlen(filename) - strlen(MRXS_EXT));
  char *slidedat_path = g_build_filename(dirname, SLIDEDAT_INI, NULL);
  bool ok = g_file_test(slidedat_path, G_FILE_TEST_EXISTS);
  g_free(slidedat_path);
  g_free(dirname);
  if (!ok) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "%s does not exist", SLIDEDAT_INI);
    return false;
  }

  return true;
}

static char *read_string_from_file(FILE *f, int len) {
  char *str = g_malloc(len + 1);
  str[len] = '\0';

  if (fread(str, len, 1, f) != 1) {
    g_free(str);
    return NULL;
  }
  return str;
}

static bool read_le_int32_from_file_with_result(FILE *f, int32_t *OUT) {
  if (fread(OUT, 4, 1, f) != 1) {
    return false;
  }

  *OUT = GINT32_FROM_LE(*OUT);
  //  g_debug("%d", i);

  return true;
}

static int32_t read_le_int32_from_file(FILE *f) {
  int32_t i;

  if (!read_le_int32_from_file_with_result(f, &i)) {
    // -1 means error
    i = -1;
  }

  return i;
}


static bool read_nonhier_record(FILE *f,
				int64_t nonhier_root_position,
				int datafile_count,
				char **datafile_paths,
				int recordno,
				char **path,
				int64_t *size, int64_t *position,
				GError **err) {
  g_return_val_if_fail(recordno >= 0, false);

  if (fseeko(f, nonhier_root_position, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot seek to nonhier root");
    return false;
  }

  int32_t ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read initial nonhier pointer");
    return false;
  }

  // seek to record pointer
  if (fseeko(f, ptr + 4 * recordno, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot seek to nonhier record pointer %d", recordno);
    return false;
  }

  // read pointer
  ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read nonhier record %d", recordno);
    return false;
  }

  // seek
  if (fseeko(f, ptr, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot seek to nonhier record %d", recordno);
    return false;
  }

  // read initial 0
  if (read_le_int32_from_file(f) != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Expected 0 value at beginning of data page");
    return false;
  }

  // read pointer
  ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read initial data page pointer");
    return false;
  }

  // seek to offset
  if (fseeko(f, ptr, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't seek to initial data page");
    return false;
  }

  // read pagesize == 1
  if (read_le_int32_from_file(f) != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Expected 1 value");
    return false;
  }

  // read 3 zeroes
  // the first zero is sometimes 1253, for reasons that are not clear
  // http://lists.andrew.cmu.edu/pipermail/openslide-users/2013-August/000634.html
  read_le_int32_from_file(f);
  if (read_le_int32_from_file(f) != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Expected second 0 value");
    return false;
  }
  if (read_le_int32_from_file(f) != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Expected third 0 value");
    return false;
  }

  // finally read offset, size, fileno
  *position = read_le_int32_from_file(f);
  if (*position == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read position");
    return false;
  }
  *size = read_le_int32_from_file(f);
  if (*size == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read size");
    return false;
  }
  int fileno = read_le_int32_from_file(f);
  if (fileno == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read fileno");
    return false;
  } else if (fileno < 0 || fileno >= datafile_count) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid fileno %d", fileno);
    return false;
  }
  *path = datafile_paths[fileno];

  return true;
}


static void insert_tile(struct level *l,
                        const struct slide_zoom_level_params *lp,
                        struct image *image,
                        double pos_x, double pos_y,
                        double src_x, double src_y,
                        int tile_x, int tile_y,
                        int zoom_level) {
  // increment image refcount
  image->refcount++;

  // generate tile
  struct tile *tile = g_slice_new0(struct tile);
  tile->image = image;
  tile->src_x = src_x;
  tile->src_y = src_y;

  // compute offset
  double offset_x = pos_x - (tile_x * lp->tile_advance_x);
  double offset_y = pos_y - (tile_y * lp->tile_advance_y);

  // we only issue tile size hints if:
  // - advances are integers (checked below)
  // - tiles do not overlap (checked below)
  // - no tile has a delta from the standard advance
  if (offset_x || offset_y) {
    // clear
    l->base.tile_w = 0;
    l->base.tile_h = 0;
  }

  // insert
  _openslide_grid_tilemap_add_tile(l->grid,
                                   tile_x, tile_y,
                                   offset_x, offset_y,
                                   l->tile_w, l->tile_h,
                                   tile);

  if (!true) {
    g_debug("zoom %d, tile %d %d, pos %.10g %.10g, offset %.10g %.10g",
	    zoom_level, tile_x, tile_y, pos_x, pos_y, offset_x, offset_y);

    g_debug(" src %.10g %.10g dim %.10g %.10g",
	    tile->src_x, tile->src_y, l->tile_w, l->tile_h);
  }
}

// given the coordinates of a tile, compute its level 0 pixel coordinates.
// return false if none of the camera positions within the tile are active.
static bool get_tile_position(int32_t *slide_positions,
                              GHashTable *active_positions,
                              const struct slide_zoom_level_params *slide_zoom_level_params,
                              struct level **levels,
                              int images_across,
                              int image_divisions,
                              int zoom_level, int xx, int yy,
                              int *pos0_x, int *pos0_y)
{
  const struct slide_zoom_level_params *lp = slide_zoom_level_params +
      zoom_level;

  const int image0_w = levels[0]->image_width;
  const int image0_h = levels[0]->image_height;

  // camera position coordinates
  int xp = xx / image_divisions;
  int yp = yy / image_divisions;
  int cp = yp * (images_across / image_divisions) + xp;
  //g_debug("xx %d, yy %d, xp %d, yp %d, cp %d, spp %d, sc %d, image0: %d %d tile: %g %g", xx, yy, xp, yp, cp, tiles_per_position, lp->tiles_per_image, image0_w, image0_h, l->tile_w, l->tile_h);

  *pos0_x = slide_positions[cp * 2] +
      image0_w * (xx - xp * image_divisions);
  *pos0_y = slide_positions[(cp * 2) + 1] +
      image0_h * (yy - yp * image_divisions);

  // ensure only active positions (those present at zoom level 0) are
  // processed at higher zoom levels
  if (zoom_level == 0) {
    // If the level 0 concat factor <= image_divisions, we can simply mark
    // active any position with a corresponding level 0 image.
    //
    // If the concat factor is larger, then active and inactive positions
    // can be merged into the same image, and we can no longer tell which
    // tiles can be skipped at higher zoom levels.  Sometimes such
    // positions have coordinates (0, 0) in the slide_positions map; we can
    // at least filter out these, and we must because such positions break
    // the tilemap grid's range search.  Assume that only position (0, 0)
    // can be at pixel (0, 0).
    if (slide_positions[cp * 2] == 0 && slide_positions[cp * 2 + 1] == 0 &&
        (xp != 0 || yp != 0)) {
      return false;
    }

    int *key = g_new(int, 1);
    *key = cp;
    g_hash_table_insert(active_positions, key, NULL);
    return true;

  } else {
    // make sure at least one of the positions within this tile is active
    for (int ypp = yp; ypp < yp + lp->positions_per_tile; ypp++) {
      for (int xpp = xp; xpp < xp + lp->positions_per_tile; xpp++) {
        int cpp = ypp * (images_across / image_divisions) + xpp;
        if (g_hash_table_lookup_extended(active_positions, &cpp, NULL, NULL)) {
          //g_debug("accept tile: level %d xp %d yp %d xpp %d ypp %d", zoom_level, xp, yp, xpp, ypp);
          return true;
        }
      }
    }

    //g_debug("skip tile: level %d positions %d xp %d yp %d", zoom_level, lp->positions_per_tile, xp, yp);
    return false;
  }
}

static bool process_hier_data_pages_from_indexfile(FILE *f,
						   int64_t seek_location,
						   int datafile_count,
						   char **datafile_paths,
						   int zoom_levels,
						   struct level **levels,
						   int images_across,
						   int images_down,
						   int image_divisions,
						   const struct slide_zoom_level_params *slide_zoom_level_params,
						   int32_t *slide_positions,
						   struct _openslide_hash *quickhash1,
						   GError **err) {
  int32_t image_number = 0;

  bool success = false;

  // used for storing which positions actually have data
  GHashTable *active_positions = g_hash_table_new_full(g_int_hash, g_int_equal,
						       g_free, NULL);

  for (int zoom_level = 0; zoom_level < zoom_levels; zoom_level++) {
    struct level *l = levels[zoom_level];
    const struct slide_zoom_level_params *lp = slide_zoom_level_params +
        zoom_level;
    int32_t ptr;

    //    g_debug("reading zoom_level %d", zoom_level);

    if (fseeko(f, seek_location, SEEK_SET) == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot seek to zoom level pointer %d", zoom_level);
      goto DONE;
    }

    ptr = read_le_int32_from_file(f);
    if (ptr == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read zoom level pointer");
      goto DONE;
    }
    if (fseeko(f, ptr, SEEK_SET) == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Cannot seek to start of data pages");
      goto DONE;
    }

    // read initial 0
    if (read_le_int32_from_file(f) != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Expected 0 value at beginning of data page");
      goto DONE;
    }

    // read pointer
    ptr = read_le_int32_from_file(f);
    if (ptr == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't read initial data page pointer");
      goto DONE;
    }

    // seek to offset
    if (fseeko(f, ptr, SEEK_SET) == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Can't seek to initial data page");
      goto DONE;
    }

    int32_t next_ptr;
    do {
      // read length
      int32_t page_len = read_le_int32_from_file(f);
      if (page_len == -1) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Can't read page length");
        goto DONE;
      }

      //    g_debug("page_len: %d", page_len);

      // read "next" pointer
      next_ptr = read_le_int32_from_file(f);
      if (next_ptr == -1) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Cannot read \"next\" pointer");
        goto DONE;
      }

      // read all the data into the list
      for (int i = 0; i < page_len; i++) {
	int32_t image_index = read_le_int32_from_file(f);
	int32_t offset = read_le_int32_from_file(f);
	int32_t length = read_le_int32_from_file(f);
	int32_t fileno = read_le_int32_from_file(f);

	if (image_index < 0) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "image_index < 0");
          goto DONE;
	}
	if (offset < 0) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "offset < 0");
          goto DONE;
	}
	if (length < 0) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "length < 0");
          goto DONE;
	}
	if (fileno < 0) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "fileno < 0");
          goto DONE;
	}

	// we have only encountered slides with exactly power-of-two scale
	// factors, and there appears to be no clear way to specify otherwise,
	// so require it
	int32_t x = image_index % images_across;
	int32_t y = image_index / images_across;

	if (y >= images_down) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "y (%d) outside of bounds for zoom level (%d)",
                      y, zoom_level);
          goto DONE;
	}

	if (x % lp->image_concat) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "x (%d) not correct multiple for zoom level (%d)",
                      x, zoom_level);
          goto DONE;
	}
	if (y % lp->image_concat) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "y (%d) not correct multiple for zoom level (%d)",
                      y, zoom_level);
          goto DONE;
	}

	// save filename
	if (fileno >= datafile_count) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                      "Invalid fileno");
          goto DONE;
	}

	// hash in the lowest-res images
	if (zoom_level == zoom_levels - 1) {
	  if (!_openslide_hash_file_part(quickhash1, datafile_paths[fileno],
	                                 offset, length, err, NULL)) {
            g_prefix_error(err, "Can't hash images: ");
            goto DONE;
	  }
	}

	// populate the image structure
	struct image *image = g_slice_new0(struct image);
	image->fileno = fileno;
	image->start_in_file = offset;
	image->length = length;
	image->imageno = image_number++;
	image->refcount = 1;

	/*
	g_debug("image_concat: %d, tiles_per_image: %d",
		lp->image_concat, lp->tiles_per_image);
	g_debug("found %d %d from file", x, y);
	*/


	// start processing 1 image into tiles_per_image^2 tiles
	for (int yi = 0; yi < lp->tiles_per_image; yi++) {
	  int yy = y + (yi * image_divisions);
	  if (yy >= images_down) {
	    break;
	  }

	  for (int xi = 0; xi < lp->tiles_per_image; xi++) {
	    int xx = x + (xi * image_divisions);
	    if (xx >= images_across) {
	      break;
	    }

	    // xx and yy are the image coordinates in level0 space

	    // position in level 0
            int pos0_x;
            int pos0_y;
            if (!get_tile_position(slide_positions,
                                   active_positions,
                                   slide_zoom_level_params,
                                   levels,
                                   images_across,
                                   image_divisions,
                                   zoom_level,
                                   xx, yy,
                                   &pos0_x, &pos0_y)) {
              // no such position
              continue;
            }

	    // position in this level
	    const double pos_x = ((double) pos0_x) / lp->image_concat;
	    const double pos_y = ((double) pos0_y) / lp->image_concat;

	    //g_debug("pos0: %d %d, pos: %g %g", pos0_x, pos0_y, pos_x, pos_y);

	    insert_tile(l, lp,
                        image,
                        pos_x, pos_y,
                        l->tile_w * xi, l->tile_h * yi,
                        x / lp->tile_count_divisor + xi,
                        y / lp->tile_count_divisor + yi,
                        zoom_level);
	  }
	}

	// drop initial reference, possibly free
	image_unref(image);
      }
    } while (next_ptr != 0);

    // advance for next zoom level
    seek_location += 4;
  }

  success = true;

 DONE:
  g_hash_table_unref(active_positions);

  return success;
}

static void *inflate_buffer(const void *src,
                            int64_t src_len,
                            int64_t dst_len,
                            GError **err) {
  void *dst = g_malloc(dst_len);
  z_stream strm = {
    .avail_in = src_len,
    .avail_out = dst_len,
    .next_in = (Bytef *) src,
    .next_out = (Bytef *) dst
  };

  int64_t error_code = -1;

  error_code = inflateInit(&strm);
  if (error_code != Z_OK) {
    goto ZLIB_ERROR;
  }
  error_code = inflate(&strm, Z_FINISH);
  if (error_code != Z_STREAM_END || (int64_t) strm.total_out != dst_len) {
    inflateEnd(&strm);
    goto ZLIB_ERROR;
  }
  error_code = inflateEnd(&strm);
  if (error_code != Z_OK) {
    goto ZLIB_ERROR;
  }

  return dst;

ZLIB_ERROR:
  if (error_code == Z_STREAM_END) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Short read while decompressing: %lu/%"PRId64,
                strm.total_out, dst_len);
  } else if (strm.msg) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Decompression failure: %s (%s)", zError(error_code), strm.msg);
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Decompression failure: %s", zError(error_code));
  }
  g_free(dst);
  return NULL;
}

static void *read_record_data(const char *path,
                              int64_t size, int64_t offset,
                              GError **err) {
  void *buffer = NULL;
  FILE *f = _openslide_fopen(path, "rb", err);
  if (!f) {
    return NULL;
  }

  if (fseeko(f, offset, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot seek data file");
    fclose(f);
    return NULL;
  }

  buffer = g_malloc(size);
  if (fread(buffer, sizeof(char), size, f) != (size_t) size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Error while reading data");
    g_free(buffer);
    fclose(f);
    return NULL;
  }

  fclose(f);
  return buffer;
}

static int32_t *read_slide_position_buffer(const void *buffer,
					   int64_t buffer_size,
					   int level_0_image_concat,
					   GError **err) {

  if (buffer_size % SLIDE_POSITION_RECORD_SIZE != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected buffer size");
    return NULL;
  }

  const char *p = buffer;
  int64_t count = buffer_size / SLIDE_POSITION_RECORD_SIZE;
  int32_t *result = g_new(int, count * 2);
  int32_t x;
  int32_t y;
  char zz;

  //  g_debug("slide positions count: %d", count);

  for (int64_t i = 0; i < count; i++) {
    zz = *p;  // flag byte

    if (zz & 0xfe) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Unexpected flag value (%d)", zz);
      g_free(result);
      return NULL;
    }

    p++;

    // x, y
    memcpy(&x, p, sizeof(x));
    x = GINT32_FROM_LE(x);
    p += sizeof(int32_t);

    memcpy(&y, p, sizeof(y));
    y = GINT32_FROM_LE(y);
    p += sizeof(int32_t);

    result[i * 2] = x * level_0_image_concat;
    result[(i * 2) + 1] = y * level_0_image_concat;
  }

  return result;
}

static enum image_format parse_image_format(const char *name, GError **err) {
  if (!strcmp(name, "JPEG")) {
    return FORMAT_JPEG;
  } else if (!strcmp(name, "PNG")) {
    return FORMAT_PNG;
  } else if (!strcmp(name, "BMP24")) {
    return FORMAT_BMP;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unrecognized image format: %s", name);
    return FORMAT_UNKNOWN;
  }
}

static bool add_associated_image(openslide_t *osr,
                                 FILE *indexfile,
                                 int64_t nonhier_root,
                                 int datafile_count,
                                 char **datafile_paths,
                                 const char *name,
                                 int recordno,
                                 GError **err) {
  char *path;
  int64_t size;
  int64_t offset;

  if (recordno == -1) {
    // no such image
    return true;
  }

  if (!read_nonhier_record(indexfile, nonhier_root,
                           datafile_count, datafile_paths, recordno,
                           &path, &size, &offset, err)) {
    g_prefix_error(err, "Cannot read %s associated image: ", name);
    return false;
  }

  return _openslide_jpeg_add_associated_image(osr, name, path, offset, err);
}


static bool process_indexfile(openslide_t *osr,
			      const char *uuid,
			      int datafile_count,
			      char **datafile_paths,
			      int vimslide_position_record,
			      int stitching_position_record,
			      int macro_record,
			      int label_record,
			      int thumbnail_record,
			      int zoom_levels,
			      int images_x,
			      int images_y,
			      double overlap_x,
			      double overlap_y,
			      int image_divisions,
			      const struct slide_zoom_level_params *slide_zoom_level_params,
			      FILE *indexfile,
			      struct level **levels,
			      struct _openslide_hash *quickhash1,
			      GError **err) {
  char *teststr = NULL;
  bool match;

  void *slide_position_buffer = NULL;
  int slide_position_record = -1;

  // init tmp parameters
  int32_t ptr = -1;

  const int npositions = (images_x / image_divisions) * (images_y / image_divisions);
  const int slide_position_buffer_size = SLIDE_POSITION_RECORD_SIZE * npositions;

  bool success = false;

  int32_t *slide_positions = NULL;

  rewind(indexfile);

  // save root positions
  const int64_t hier_root = strlen(INDEX_VERSION) + strlen(uuid);
  const int64_t nonhier_root = hier_root + 4;

  // verify version and uuid
  teststr = read_string_from_file(indexfile, strlen(INDEX_VERSION));
  match = (teststr != NULL) && (strcmp(teststr, INDEX_VERSION) == 0);
  g_free(teststr);
  if (!match) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Index.dat doesn't have expected version");
    goto DONE;
  }

  teststr = read_string_from_file(indexfile, strlen(uuid));
  match = (teststr != NULL) && (strcmp(teststr, uuid) == 0);
  g_free(teststr);
  if (!match) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Index.dat doesn't have a matching slide identifier");
    goto DONE;
  }

  // If we have individual slide positioning information as part of the
  // non-hier data, read the position information.
  if (vimslide_position_record != -1) {
    slide_position_record = vimslide_position_record;
  } else {
    slide_position_record = stitching_position_record;
  }

  if (slide_position_record != -1) {
    char *slide_position_path;
    int64_t slide_position_size;
    int64_t slide_position_offset;
    if (!read_nonhier_record(indexfile,
			     nonhier_root,
			     datafile_count,
			     datafile_paths,
			     slide_position_record,
			     &slide_position_path,
			     &slide_position_size,
			     &slide_position_offset,
			     err)) {
      g_prefix_error(err, "Cannot read slide position info: ");
      goto DONE;
    }

    slide_position_buffer = read_record_data(slide_position_path,
                                             slide_position_size,
                                             slide_position_offset,
                                             err);
    if (!slide_position_buffer) {
      g_prefix_error(err, "Cannot read slide position record: ");
      goto DONE;
    }

    if (slide_position_record == stitching_position_record) {
      // We need to decompress the buffer.
      // Length check happens in inflate_buffer
      void *decompressed = inflate_buffer(slide_position_buffer,
                                          slide_position_size,
                                          slide_position_buffer_size,
                                          err);

      g_free(slide_position_buffer); // free the compressed buffer

      if (decompressed) {
        slide_position_buffer = decompressed;
      } else {
        g_prefix_error(err, "Error decompressing position buffer: ");
        goto DONE;
      }
    } else if (slide_position_buffer_size != slide_position_size) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Slide position file not of the expected size");
      g_free(slide_position_buffer);
      goto DONE;
    }

    // read in the slide positions
    slide_positions = read_slide_position_buffer(slide_position_buffer,
					         slide_position_buffer_size,
					         slide_zoom_level_params[0].image_concat,
					         err);

    g_free(slide_position_buffer);

    if (!slide_positions) {
      goto DONE;
    }
  } else {
    // No position map available.  Fill in our own values based on the tile
    // size and nominal overlap.
    const int image0_w = levels[0]->image_width;
    const int image0_h = levels[0]->image_height;
    const int positions_x = images_x / image_divisions;

    slide_positions = g_new(int, npositions * 2);
    for (int i = 0; i < npositions; i++) {
      slide_positions[(i * 2)]     = (i % positions_x) *
                                     (image0_w * image_divisions - overlap_x);
      slide_positions[(i * 2) + 1] = (i / positions_x) *
                                     (image0_h * image_divisions - overlap_y);
    }
  }

  // read in the associated images
  if (!add_associated_image(osr,
                            indexfile,
                            nonhier_root,
                            datafile_count,
                            datafile_paths,
                            "macro",
                            macro_record,
                            err)) {
    goto DONE;
  }
  if (!add_associated_image(osr,
                            indexfile,
                            nonhier_root,
                            datafile_count,
                            datafile_paths,
                            "label",
                            label_record,
                            err)) {
    goto DONE;
  }
  if (!add_associated_image(osr,
                            indexfile,
                            nonhier_root,
                            datafile_count,
                            datafile_paths,
                            "thumbnail",
                            thumbnail_record,
                            err)) {
    goto DONE;
  }

  // read hierarchical sections
  if (fseeko(indexfile, hier_root, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot seek to hier sections root");
    goto DONE;
  }

  ptr = read_le_int32_from_file(indexfile);
  if (ptr == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't read initial pointer");
    goto DONE;
  }

  // read these pages in
  if (!process_hier_data_pages_from_indexfile(indexfile,
					      ptr,
					      datafile_count,
					      datafile_paths,
					      zoom_levels,
					      levels,
					      images_x,
					      images_y,
					      image_divisions,
					      slide_zoom_level_params,
					      slide_positions,
					      quickhash1,
					      err)) {
    goto DONE;
  }

  success = true;

 DONE:
  // deallocate
  g_free(slide_positions);

  return success;
}

static void add_properties(openslide_t *osr, GKeyFile *kf) {
  char **groups = g_key_file_get_groups(kf, NULL);
  if (groups == NULL) {
    return;
  }

  for (char **group = groups; *group != NULL; group++) {
    char **keys = g_key_file_get_keys(kf, *group, NULL, NULL);
    if (keys == NULL) {
      break;
    }

    for (char **key = keys; *key != NULL; key++) {
      char *value = g_key_file_get_value(kf, *group, *key, NULL);
      if (value) {
	g_hash_table_insert(osr->properties,
			    g_strdup_printf("mirax.%s.%s", *group, *key),
			    g_strdup(value));
	g_free(value);
      }
    }
    g_strfreev(keys);
  }
  g_strfreev(groups);
}

static int get_nonhier_name_offset_helper(GKeyFile *keyfile,
					  int nonhier_count,
					  const char *group,
					  const char *target_name,
					  int *name_count_out,
					  int *name_index_out,
					  GError **err) {
  GError *tmp_err = NULL;

  *name_count_out = 0;
  *name_index_out = 0;

  int offset = 0;
  for (int i = 0; i < nonhier_count; i++) {
    *name_index_out = i;

    // look at a key's value
    char *key = g_strdup_printf(KEY_NONHIER_d_NAME, i);
    char *value = g_key_file_get_value(keyfile, group, key, err);
    g_free(key);

    if (!value) {
      return -1;
    }

    // save count for this name
    key = g_strdup_printf(KEY_NONHIER_d_COUNT, i);
    int count = g_key_file_get_integer(keyfile, group, key, &tmp_err);
    g_free(key);
    if (!count) {
      if (tmp_err) {
        g_propagate_error(err, tmp_err);
      } else {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Nonhier val count is zero");
      }
      g_free(value);
      return -1;
    }
    *name_count_out = count;

    if (strcmp(target_name, value) == 0) {
      g_free(value);
      return offset;
    }
    g_free(value);

    // otherwise, increase offset
    offset += count;
  }

  return -1;
}


static int get_nonhier_name_offset(GKeyFile *keyfile,
				   int nonhier_count,
				   const char *group,
				   const char *target_name,
				   GError **err) {
  int d1, d2;
  return get_nonhier_name_offset_helper(keyfile,
					nonhier_count,
					group,
					target_name,
					&d1, &d2,
					err);
}

static int get_nonhier_val_offset(GKeyFile *keyfile,
				  int nonhier_count,
				  const char *group,
				  const char *target_name,
				  const char *target_value,
				  char **section_name,
				  GError **err) {
  int name_count;
  int name_index;
  int offset = get_nonhier_name_offset_helper(keyfile, nonhier_count,
					      group, target_name,
					      &name_count,
					      &name_index,
					      err);
  if (offset == -1) {
    return -1;
  }

  for (int i = 0; i < name_count; i++) {
    char *key = g_strdup_printf(KEY_NONHIER_d_VAL_d, name_index, i);
    char *value = g_key_file_get_value(keyfile, group, key, err);
    g_free(key);

    if (!value) {
      return -1;
    }

    if (strcmp(target_value, value) == 0) {
      g_free(value);

      if (section_name != NULL) {
        char *section_key = g_strdup_printf(KEY_NONHIER_d_VAL_d_SECTION,
                                            name_index, i);
        *section_name = g_key_file_get_value(keyfile, group, section_key, err);
        g_free(section_key);

        if (!*section_name) {
          return -1;
        }
      }

      return offset;
    }

    // otherwise, increase offset
    g_free(value);
    offset++;
  }

  return -1;
}

static int get_associated_image_nonhier_offset(GKeyFile *keyfile,
                                               int nonhier_count,
                                               const char *group,
                                               const char *target_name,
                                               const char *target_value,
                                               const char *target_format_key,
                                               GError **err) {
  char *section_name;
  int offset = get_nonhier_val_offset(keyfile,
                                      nonhier_count,
                                      group,
                                      target_name,
                                      target_value,
                                      &section_name,
                                      err);
  if (offset == -1) {
    return -1;
  }

  char *format = g_key_file_get_value(keyfile, section_name,
                                      target_format_key, err);
  g_free(section_name);
  if (format == NULL) {
    return -1;
  }

  // verify image format
  // we have only ever seen JPEG
  if (parse_image_format(format, NULL) != FORMAT_JPEG) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported associated image format: %s", format);
    g_free(format);
    return -1;
  }
  g_free(format);

  return offset;
}

static bool mirax_open(openslide_t *osr, const char *filename,
                       struct _openslide_tifflike *tl G_GNUC_UNUSED,
                       struct _openslide_hash *quickhash1, GError **err) {
  struct level **levels = NULL;

  char *dirname = NULL;

  GKeyFile *slidedat = NULL;
  GError *tmp_err = NULL;

  bool success = false;
  char *tmp = NULL;

  // info about this slide
  char *slide_version = NULL;
  char *slide_id = NULL;
  int images_x = 0;
  int images_y = 0;
  int image_divisions = 0;
  int objective_magnification = 0;

  char *index_filename = NULL;
  int zoom_levels = 0;
  int hier_count = 0;
  int nonhier_count = 0;
  int position_nonhier_vimslide_offset = -1;  // VIMSLIDE_POSITION_BUFFER
  int position_nonhier_stitching_offset = -1; // StitchingIntensityLayer
  int macro_nonhier_offset = -1;
  int label_nonhier_offset = -1;
  int thumbnail_nonhier_offset = -1;

  int slide_zoom_level_value = -1;
  char *key_slide_zoom_level_name = NULL;
  char *key_slide_zoom_level_count = NULL;
  char **slide_zoom_level_section_names = NULL;
  struct slide_zoom_level_section *slide_zoom_level_sections = NULL;
  struct slide_zoom_level_params *slide_zoom_level_params = NULL;

  int datafile_count = 0;
  char **datafile_paths = NULL;

  FILE *indexfile = NULL;

  int64_t base_w = 0;
  int64_t base_h = 0;

  int total_concat_exponent = 0;

  // get directory from filename
  dirname = g_strndup(filename, strlen(filename) - strlen(MRXS_EXT));

  // first, check slidedat
  tmp = g_build_filename(dirname, SLIDEDAT_INI, NULL);
  // hash the slidedat
  if (!_openslide_hash_file(quickhash1, tmp, err)) {
    goto FAIL;
  }

  slidedat = _openslide_read_key_file(tmp, SLIDEDAT_MAX_SIZE,
                                      G_KEY_FILE_NONE, err);
  if (!slidedat) {
    g_prefix_error(err, "Can't load Slidedat.ini file: ");
    goto FAIL;
  }
  g_free(tmp);
  tmp = NULL;

  // add properties
  add_properties(osr, slidedat);

  // load general stuff
  HAVE_GROUP_OR_FAIL(slidedat, GROUP_GENERAL);

  READ_KEY_OR_FAIL(slide_version, slidedat, GROUP_GENERAL,
                   KEY_SLIDE_VERSION, value);
  READ_KEY_OR_FAIL(slide_id, slidedat, GROUP_GENERAL,
                   KEY_SLIDE_ID, value);
  READ_KEY_OR_FAIL(images_x, slidedat, GROUP_GENERAL,
                   KEY_IMAGENUMBER_X, integer);
  READ_KEY_OR_FAIL(images_y, slidedat, GROUP_GENERAL,
                   KEY_IMAGENUMBER_Y, integer);
  READ_KEY_OR_FAIL(objective_magnification, slidedat, GROUP_GENERAL,
                   KEY_OBJECTIVE_MAGNIFICATION, integer);

  image_divisions = g_key_file_get_integer(slidedat, GROUP_GENERAL,
					   KEY_CAMERA_IMAGE_DIVISIONS_PER_SIDE,
					   &tmp_err);
  if (tmp_err != NULL) {
    image_divisions = 1;
    g_clear_error(&tmp_err);
  }

  // ensure positive values
  POSITIVE_OR_FAIL(images_x);
  POSITIVE_OR_FAIL(images_y);
  POSITIVE_OR_FAIL(image_divisions);

  // load hierarchical stuff
  HAVE_GROUP_OR_FAIL(slidedat, GROUP_HIERARCHICAL);

  READ_KEY_OR_FAIL(hier_count, slidedat, GROUP_HIERARCHICAL,
                   KEY_HIER_COUNT, integer);
  READ_KEY_OR_FAIL(nonhier_count, slidedat, GROUP_HIERARCHICAL,
                   KEY_NONHIER_COUNT, integer);

  POSITIVE_OR_FAIL(hier_count);
  NON_NEGATIVE_OR_FAIL(nonhier_count);

  // find key for slide zoom level
  for (int i = 0; i < hier_count; i++) {
    char *key = g_strdup_printf(KEY_HIER_d_NAME, i);
    char *value = g_key_file_get_value(slidedat, GROUP_HIERARCHICAL, key, err);
    g_free(key);

    if (!value) {
      goto FAIL;
    }

    if (strcmp(VALUE_SLIDE_ZOOM_LEVEL, value) == 0) {
      g_free(value);
      slide_zoom_level_value = i;
      key_slide_zoom_level_name = g_strdup_printf(KEY_HIER_d_NAME, i);
      key_slide_zoom_level_count = g_strdup_printf(KEY_HIER_d_COUNT, i);
      break;
    }
    g_free(value);
  }

  if (slide_zoom_level_value == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Can't find slide zoom level");
    goto FAIL;
  }

  // TODO allow slide_zoom_level_value to be at another hierarchy value
  if (slide_zoom_level_value != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Slide zoom level not HIER_0");
    goto FAIL;
  }

  READ_KEY_OR_FAIL(index_filename, slidedat, GROUP_HIERARCHICAL,
                   KEY_INDEXFILE, value);
  READ_KEY_OR_FAIL(zoom_levels, slidedat, GROUP_HIERARCHICAL,
                   key_slide_zoom_level_count, integer);
  POSITIVE_OR_FAIL(zoom_levels);


  slide_zoom_level_section_names = g_new0(char *, zoom_levels + 1);
  for (int i = 0; i < zoom_levels; i++) {
    tmp = g_strdup_printf(KEY_HIER_d_VAL_d_SECTION, slide_zoom_level_value, i);

    READ_KEY_OR_FAIL(slide_zoom_level_section_names[i], slidedat,
                     GROUP_HIERARCHICAL, tmp, value);

    g_free(tmp);
    tmp = NULL;
  }

  // load datafile stuff
  HAVE_GROUP_OR_FAIL(slidedat, GROUP_DATAFILE);

  READ_KEY_OR_FAIL(datafile_count, slidedat, GROUP_DATAFILE,
                   KEY_FILE_COUNT, integer);
  POSITIVE_OR_FAIL(datafile_count);

  datafile_paths = g_new0(char *, datafile_count + 1);
  for (int i = 0; i < datafile_count; i++) {
    tmp = g_strdup_printf(KEY_d_FILE, i);

    gchar *name;
    READ_KEY_OR_FAIL(name, slidedat, GROUP_DATAFILE, tmp, value);
    datafile_paths[i] = g_build_filename(dirname, name, NULL);
    g_free(name);

    g_free(tmp);
    tmp = NULL;
  }

  // load data from all slide_zoom_level_section_names sections
  slide_zoom_level_sections = g_new0(struct slide_zoom_level_section, zoom_levels);
  for (int i = 0; i < zoom_levels; i++) {
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;

    uint32_t bgr;

    char *group = slide_zoom_level_section_names[i];
    HAVE_GROUP_OR_FAIL(slidedat, group);

    READ_KEY_OR_FAIL(hs->concat_exponent, slidedat, group,
                     KEY_IMAGE_CONCAT_FACTOR, integer);
    READ_KEY_OR_FAIL(hs->overlap_x, slidedat, group, KEY_OVERLAP_X, double);
    READ_KEY_OR_FAIL(hs->overlap_y, slidedat, group, KEY_OVERLAP_Y, double);
    READ_KEY_OR_FAIL(hs->mpp_x, slidedat, group, KEY_MPP_X, double);
    READ_KEY_OR_FAIL(hs->mpp_y, slidedat, group, KEY_MPP_Y, double);
    READ_KEY_OR_FAIL(bgr, slidedat, group, KEY_IMAGE_FILL_COLOR_BGR, integer);
    READ_KEY_OR_FAIL(hs->image_w, slidedat, group, KEY_DIGITIZER_WIDTH,
                     integer);
    READ_KEY_OR_FAIL(hs->image_h, slidedat, group, KEY_DIGITIZER_HEIGHT,
                     integer);

    if (i == 0) {
      NON_NEGATIVE_OR_FAIL(hs->concat_exponent);
    } else {
      POSITIVE_OR_FAIL(hs->concat_exponent);
    }
    POSITIVE_OR_FAIL(hs->image_w);
    POSITIVE_OR_FAIL(hs->image_h);

    // convert fill color bgr into rgb
    hs->fill_rgb =
      ((bgr << 16) & 0x00FF0000) |
      (bgr & 0x0000FF00) |
      ((bgr >> 16) & 0x000000FF);

    // read image format
    READ_KEY_OR_FAIL(tmp, slidedat, group, KEY_IMAGE_FORMAT, value);
    hs->image_format = parse_image_format(tmp, err);
    g_free(tmp);
    tmp = NULL;
    if (hs->image_format == FORMAT_UNKNOWN) {
      goto FAIL;
    }
  }

  // load position stuff
  // find key for position, if present
  position_nonhier_vimslide_offset = get_nonhier_name_offset(slidedat,
                                                             nonhier_count,
                                                             GROUP_HIERARCHICAL,
                                                             VALUE_VIMSLIDE_POSITION_BUFFER,
                                                             &tmp_err);
  SUCCESSFUL_OR_FAIL(tmp_err);

  if (position_nonhier_vimslide_offset == -1) {
    position_nonhier_stitching_offset = get_nonhier_name_offset(slidedat,
							        nonhier_count,
							        GROUP_HIERARCHICAL,
							        VALUE_STITCHING_INTENSITY_LAYER,
							        &tmp_err);
    SUCCESSFUL_OR_FAIL(tmp_err);
  }

  // associated images
  macro_nonhier_offset = get_associated_image_nonhier_offset(slidedat,
                                                             nonhier_count,
                                                             GROUP_HIERARCHICAL,
                                                             VALUE_SCAN_DATA_LAYER,
                                                             VALUE_SCAN_DATA_LAYER_MACRO,
                                                             KEY_MACRO_IMAGE_TYPE,
                                                             &tmp_err);
  SUCCESSFUL_OR_FAIL(tmp_err);
  label_nonhier_offset = get_associated_image_nonhier_offset(slidedat,
                                                             nonhier_count,
                                                             GROUP_HIERARCHICAL,
                                                             VALUE_SCAN_DATA_LAYER,
                                                             VALUE_SCAN_DATA_LAYER_LABEL,
                                                             KEY_LABEL_IMAGE_TYPE,
                                                             &tmp_err);
  SUCCESSFUL_OR_FAIL(tmp_err);
  thumbnail_nonhier_offset = get_associated_image_nonhier_offset(slidedat,
                                                                 nonhier_count,
                                                                 GROUP_HIERARCHICAL,
                                                                 VALUE_SCAN_DATA_LAYER,
                                                                 VALUE_SCAN_DATA_LAYER_THUMBNAIL,
                                                                 KEY_THUMBNAIL_IMAGE_TYPE,
                                                                 &tmp_err);
  SUCCESSFUL_OR_FAIL(tmp_err);

  /*
  g_debug("dirname: %s", dirname);
  g_debug("slide_version: %s", slide_version);
  g_debug("slide_id: %s", slide_id);
  g_debug("images (%d,%d)", images_x, images_y);
  g_debug("index_filename: %s", index_filename);
  g_debug("zoom_levels: %d", zoom_levels);
  for (int i = 0; i < zoom_levels; i++) {
    g_debug(" section name %d: %s", i, slide_zoom_level_section_names[i]);
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;
    g_debug("  overlap_x: %g", hs->overlap_x);
    g_debug("  overlap_y: %g", hs->overlap_y);
    g_debug("  mpp_x: %g", hs->mpp_x);
    g_debug("  mpp_y: %g", hs->mpp_y);
    g_debug("  fill_rgb: %"PRIu32, hs->fill_rgb);
    g_debug("  image_w: %d", hs->image_w);
    g_debug("  image_h: %d", hs->image_h);
  }
  g_debug("datafile_count: %d", datafile_count);
  for (int i = 0; i < datafile_count; i++) {
    g_debug(" datafile path %d: %s", i, datafile_paths[i]);
  }
  g_debug("position_nonhier_vimslide_offset: %d", position_nonhier_vimslide_offset);
  g_debug("position_nonhier_stitching_offset: %d", position_nonhier_stitching_offset);
  */

  // read indexfile
  tmp = g_build_filename(dirname, index_filename, NULL);
  indexfile = _openslide_fopen(tmp, "rb", err);
  g_free(tmp);
  tmp = NULL;

  if (!indexfile) {
    goto FAIL;
  }

  // The camera on MIRAX takes a photo and records a position.
  // Then, the photo is split into image_divisions^2 image tiles.
  // So, if image_divisions=2, you'll get 4 images per photo.
  // If image_divisions=4, then 16 images per photo.
  //
  // The overlap is on the original photo, not the images.
  // What you'll get is position data for only a subset of images.
  // The images that come from a photo must be placed edge-to-edge.
  //
  // To generate levels, each image is downsampled by 2 and then
  // concatenated into a new image, 4 old images per new image (2x2).
  // Note that this is per-image, not per-photo. Repeat this several
  // times.
  //
  // The downsampling and concatenation is fine up until you start
  // concatenating images that come from different photos.
  // Then the positions don't line up and images must be split into
  // subtiles. This significantly complicates the code.

  // compute dimensions base_w and base_h in stupid but clear way
  base_w = 0;
  base_h = 0;

  for (int i = 0; i < images_x; i++) {
    if (((i % image_divisions) != (image_divisions - 1))
	|| (i == images_x - 1)) {
      // full size
      base_w += slide_zoom_level_sections[0].image_w;
    } else {
      // size minus overlap
      base_w += slide_zoom_level_sections[0].image_w - slide_zoom_level_sections[0].overlap_x;
    }
  }
  for (int i = 0; i < images_y; i++) {
    if (((i % image_divisions) != (image_divisions - 1))
	|| (i == images_y - 1)) {
      // full size
      base_h += slide_zoom_level_sections[0].image_h;
    } else {
      // size minus overlap
      base_h += slide_zoom_level_sections[0].image_h - slide_zoom_level_sections[0].overlap_y;
    }
  }


  // set up level dimensions and such
  levels = g_new0(struct level *, zoom_levels);
  slide_zoom_level_params = g_new(struct slide_zoom_level_params, zoom_levels);
  total_concat_exponent = 0;
  for (int i = 0; i < zoom_levels; i++) {
    struct level *l = g_slice_new0(struct level);
    levels[i] = l;
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;
    struct slide_zoom_level_params *lp = slide_zoom_level_params + i;

    // image_concat: number of images concatenated from the original in one
    //               dimension
    total_concat_exponent += hs->concat_exponent;
    if (total_concat_exponent > (int) sizeof(lp->image_concat) * 8 - 2) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "image_concat exponent too large: %d",
                  total_concat_exponent);
      goto FAIL;
    }
    lp->image_concat = 1 << total_concat_exponent;

    // positions_per_image: for this zoom, how many camera positions
    //                      are represented in an image?
    //                      this is constant for the first few levels,
    //                      depending on image_divisions
    const int positions_per_image = MAX(1, lp->image_concat / image_divisions);

    if (position_nonhier_vimslide_offset != -1
        || position_nonhier_stitching_offset != -1
        || slide_zoom_level_sections[0].overlap_x != 0
        || slide_zoom_level_sections[0].overlap_y != 0) {
      // tile_count_divisor: as we record levels, we would prefer to shrink the
      //                     number of tiles, but keep the tile size constant,
      //                     but this only works until we encounter images
      //                     with more than one source photo, in which case
      //                     the tile count bottoms out and we instead shrink
      //                     the advances
      lp->tile_count_divisor = MIN(lp->image_concat, image_divisions);

      // tiles_per_image: for this zoom, how many tiles in an image?
      //                  this is constant for the first few levels,
      //                  depending on image_divisions
      lp->tiles_per_image = positions_per_image;

      // positions_per_tile: for this zoom, how many camera positions
      //                     are represented in a tile?
      lp->positions_per_tile = 1;

    } else {
      // no position file and no overlaps, so we can skip subtile processing
      // for better performance

      lp->tile_count_divisor = lp->image_concat;
      lp->tiles_per_image = 1;
      lp->positions_per_tile = positions_per_image;
    }

    l->tile_w = (double) hs->image_w / lp->tiles_per_image;
    l->tile_h = (double) hs->image_h / lp->tiles_per_image;

    // images_per_position: for this zoom, how many images (in one dimension)
    //                      come from a single photo?
    const int images_per_position = MAX(1, image_divisions / lp->image_concat);

    // use a fraction of the overlap, so that our tile correction will flip between
    // positive and negative values typically (in case image_divisions=2)
    // this is because not every tile overlaps

    // overlaps are concatenated within images, so our tile size must
    // shrink, once we hit image_divisions
    lp->tile_advance_x = l->tile_w - ((double) hs->overlap_x /
        (double) images_per_position);
    lp->tile_advance_y = l->tile_h - ((double) hs->overlap_y /
        (double) images_per_position);

    l->base.w = base_w / lp->image_concat;  // image_concat is powers of 2
    l->base.h = base_h / lp->image_concat;
    l->image_format = hs->image_format;
    l->image_width = hs->image_w;  // raw image size
    l->image_height = hs->image_h;

    // initialize tile size hints if potentially valid (may be cleared later)
    if (((int64_t) lp->tile_advance_x) == lp->tile_advance_x &&
        ((int64_t) lp->tile_advance_y) == lp->tile_advance_y &&
        lp->tile_advance_x == l->tile_w &&
        lp->tile_advance_y == l->tile_h) {
      l->base.tile_w = lp->tile_advance_x;
      l->base.tile_h = lp->tile_advance_y;
    }

    // override downsample.  level 0 is defined to have a downsample of 1.0,
    // irrespective of its concat_exponent
    l->base.downsample = lp->image_concat /
                         slide_zoom_level_params[0].image_concat;

    // create grid
    l->grid = _openslide_grid_create_tilemap(osr,
                                             lp->tile_advance_x,
                                             lp->tile_advance_y,
                                             read_tile, tile_free);

    //g_debug("level %d tile advance %.10g %.10g, dim %"PRId64" %"PRId64", image size %d %d, tile %g %g, image_concat %d, tile_count_divisor %d, positions_per_tile %d", i, lp->tile_advance_x, lp->tile_advance_y, l->base.w, l->base.h, l->image_width, l->image_height, l->tile_w, l->tile_h, lp->image_concat, lp->tile_count_divisor, lp->positions_per_tile);
  }

  // load the position map and build up the tiles
  if (!process_indexfile(osr,
			 slide_id,
			 datafile_count, datafile_paths,
			 position_nonhier_vimslide_offset,
			 position_nonhier_stitching_offset,
			 macro_nonhier_offset,
			 label_nonhier_offset,
			 thumbnail_nonhier_offset,
			 zoom_levels,
			 images_x, images_y,
			 slide_zoom_level_sections[0].overlap_x,
			 slide_zoom_level_sections[0].overlap_y,
			 image_divisions,
			 slide_zoom_level_params,
			 indexfile,
			 levels,
			 quickhash1,
			 err)) {
    goto FAIL;
  }

  // set properties
  _openslide_set_bounds_props_from_grid(osr, levels[0]->grid);
  uint32_t fill = slide_zoom_level_sections[0].fill_rgb;
  _openslide_set_background_color_prop(osr,
                                       (fill >> 16) & 0xFF,
                                       (fill >> 8) & 0xFF,
                                       fill & 0xFF);
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER),
                      g_strdup_printf("%d", objective_magnification));
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_X),
                      _openslide_format_double(slide_zoom_level_sections[0].mpp_x));
  g_hash_table_insert(osr->properties,
                      g_strdup(OPENSLIDE_PROPERTY_NAME_MPP_Y),
                      _openslide_format_double(slide_zoom_level_sections[0].mpp_y));

  /*
  for (int i = 0; i < zoom_levels; i++) {
    struct level *l = levels[i];
    g_debug("level %d", i);
    g_debug(" size %"PRId64" %"PRId64, l->base.w, l->base.h);
    g_debug(" image size %d %d", l->image_width, l->image_height);
    g_debug(" tile advance %g %g", lp->tile_advance_x, lp->tile_advance_y);
  }
  */

  // if any level is missing tile size hints, we must invalidate all hints
  for (int i = 0; i < zoom_levels; i++) {
    if (!levels[i]->base.tile_w || !levels[i]->base.tile_h) {
      // invalidate
      for (i = 0; i < zoom_levels; i++) {
        levels[i]->base.tile_w = 0;
        levels[i]->base.tile_h = 0;
      }
      break;
    }
  }

  // populate the level_count
  osr->level_count = zoom_levels;

  // populate levels
  g_assert(osr->levels == NULL);
  osr->levels = (struct _openslide_level **) levels;
  levels = NULL;

  // set private data
  g_assert(osr->data == NULL);
  struct mirax_ops_data *data = g_slice_new0(struct mirax_ops_data);
  data->datafile_paths = datafile_paths;
  datafile_paths = NULL;
  osr->data = data;

  // set ops
  osr->ops = &mirax_ops;

  success = true;
  goto DONE;

 FAIL:
  if (levels != NULL) {
    for (int i = 0; i < zoom_levels; i++) {
      struct level *l = levels[i];
      if (l) {
        _openslide_grid_destroy(l->grid);
        g_slice_free(struct level, l);
      }
    }
    g_free(levels);
  }

  success = false;

 DONE:
  g_free(dirname);
  g_free(tmp);
  g_free(slide_version);
  g_free(slide_id);
  g_free(index_filename);
  g_strfreev(datafile_paths);
  g_strfreev(slide_zoom_level_section_names);
  g_free(slide_zoom_level_sections);
  g_free(slide_zoom_level_params);
  g_free(key_slide_zoom_level_name);
  g_free(key_slide_zoom_level_count);

  if (slidedat) {
    g_key_file_free(slidedat);
  }
  if (indexfile) {
    fclose(indexfile);
  }

  return success;
}

const struct _openslide_format _openslide_format_mirax = {
  .name = "mirax",
  .vendor = "mirax",
  .detect = mirax_detect,
  .open = mirax_open,
};
