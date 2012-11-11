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
 * MIRAX (mrxs) support
 *
 * quickhash comes from the slidedat file and the lowest resolution datafile
 *
 */

#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <jpeglib.h>
#include <zlib.h>

#include "openslide-hash.h"

static const char MRXS_EXT[] = ".mrxs";
static const char SLIDEDAT_INI[] = "Slidedat.ini";

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
static const char VALUE_VIMSLIDE_POSITION_BUFFER[] = "VIMSLIDE_POSITION_BUFFER";
static const char VALUE_STITCHING_INTENSITY_LAYER[] = "StitchingIntensityLayer";
static const char VALUE_SCAN_DATA_LAYER[] = "Scan data layer";
static const char VALUE_SCAN_DATA_LAYER_MACRO[] = "ScanDataLayer_SlideThumbnail";
static const char VALUE_SCAN_DATA_LAYER_LABEL[] = "ScanDataLayer_SlideBarcode";
static const char VALUE_SCAN_DATA_LAYER_THUMBNAIL[] = "ScanDataLayer_SlidePreview";
static const char VALUE_SLIDE_ZOOM_LEVEL[] = "Slide zoom level";

static const char GROUP_NONHIERLAYER_d_SECTION[] = "NONHIERLAYER_%d_SECTION";
static const char KEY_VIMSLIDE_POSITION_DATA_FORMAT_VERSION[] =
  "VIMSLIDE_POSITION_DATA_FORMAT_VERSION";
static const int VALUE_VIMSLIDE_POSITION_DATA_FORMAT_VERSION = 257;
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

#define READ_KEY_OR_FAIL(TARGET, KEYFILE, GROUP, KEY, TYPE, FAIL_MSG)	\
  do {									\
    GError *tmp_err = NULL;						\
    TARGET = g_key_file_get_ ## TYPE(KEYFILE, GROUP, KEY, &tmp_err);	\
    if (tmp_err != NULL) {						\
      g_clear_error(&tmp_err);						\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,	\
                  FAIL_MSG);						\
      goto FAIL;							\
    }									\
  } while(0)

#define HAVE_GROUP_OR_FAIL(KEYFILE, GROUP)				\
  do {									\
    if (!g_key_file_has_group(slidedat, GROUP)) {			\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,	\
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
                  OPENSLIDE_ERROR_BAD_DATA, #N " <= 0: %d", N);	\
      goto FAIL;						\
    }								\
  } while(0)

#define NON_NEGATIVE_OR_FAIL(N)					\
  do {								\
    if (N < 0) {						\
      g_set_error(err, OPENSLIDE_ERROR,				\
                  OPENSLIDE_ERROR_BAD_DATA, #N " < 0: %d", N);	\
      goto FAIL;						\
    }								\
  } while(0)
  

struct slide_zoom_level_section {
  int concat_exponent;

  double overlap_x;
  double overlap_y;
  double mpp_x;
  double mpp_y;

  uint32_t fill_rgb;

  int tile_w;
  int tile_h;
};

// see comments in _openslide_try_mirax()
struct slide_zoom_level_params {
  int tile_concat;
  int tile_count_divisor;
  int subtiles_per_jpeg_tile;
  int positions_per_subtile;
  double subtile_w;
  double subtile_h;
};

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
				const char *dirname,
				int datafile_count,
				char **datafile_names,
				int recordno,
				char **path,  // must g_free()
				int64_t *size, int64_t *position,
				GError **err) {
  g_return_val_if_fail(recordno >= 0, false);

  if (fseeko(f, nonhier_root_position, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot seek to nonhier root");
    return false;
  }

  int32_t ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read initial nonhier pointer");
    return false;
  }

  // seek to record pointer
  if (fseeko(f, ptr + 4 * recordno, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot seek to nonhier record pointer %d", recordno);
    return false;
  }

  // read pointer
  ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read nonhier record %d", recordno);
    return false;
  }

  // seek
  if (fseeko(f, ptr, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot seek to nonhier record %d", recordno);
    return false;
  }

  // read initial 0
  if (read_le_int32_from_file(f) != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Expected 0 value at beginning of data page");
    return false;
  }

  // read pointer
  ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read initial data page pointer");
    return false;
  }

  // seek to offset
  if (fseeko(f, ptr, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't seek to initial data page");
    return false;
  }

  // read pagesize == 1
  if (read_le_int32_from_file(f) != 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Expected 1 value");
    return false;
  }

  // read 3 zeroes
  if (read_le_int32_from_file(f) != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Expected first 0 value");
    return false;
  }
  if (read_le_int32_from_file(f) != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Expected second 0 value");
    return false;
  }
  if (read_le_int32_from_file(f) != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Expected third 0 value");
    return false;
  }

  // finally read offset, size, fileno
  *position = read_le_int32_from_file(f);
  if (*position == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read position");
    return false;
  }
  *size = read_le_int32_from_file(f);
  if (*size == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read size");
    return false;
  }
  int fileno = read_le_int32_from_file(f);
  if (fileno == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read fileno");
    return false;
  } else if (fileno < 0 || fileno >= datafile_count) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid fileno %d", fileno);
    return false;
  }
  *path = g_build_filename(dirname, datafile_names[fileno], NULL);

  return true;
}


static void insert_subtile(GHashTable *tiles, int32_t jpeg_number,
			   double pos_x, double pos_y,
			   double src_x, double src_y,
			   double tw, double th,
			   double tile_advance_x, double tile_advance_y,
			   int tile_x, int tile_y,
			   int tiles_across,
			   int zoom_level) {
  // generate tile
  struct _openslide_jpeg_tile *tile = g_slice_new0(struct _openslide_jpeg_tile);
  tile->fileno = jpeg_number;
  tile->tileno = 0;
  tile->src_x = src_x;
  tile->src_y = src_y;
  tile->w = tw;
  tile->h = th;

  // compute offset
  tile->dest_offset_x = pos_x - (tile_x * tile_advance_x);
  tile->dest_offset_y = pos_y - (tile_y * tile_advance_y);

  // insert
  int64_t *key = g_slice_new(int64_t);
  *key = (tile_y * tiles_across) + tile_x;
  g_hash_table_insert(tiles, key, tile);

  if (!true) {
    g_debug("zoom %d, tile %d %d, pos %.10g %.10g, offset %.10g %.10g",
	    zoom_level, tile_x, tile_y, pos_x, pos_y, tile->dest_offset_x, tile->dest_offset_y);

    g_debug(" src %.10g %.10g dim %.10g %.10g key %" G_GINT64_FORMAT,
	    tile->src_x, tile->src_y, tile->w, tile->h, *key);
  }
}

// given the coordinates of a subtile, compute its level 0 pixel coordinates.
// return false if none of the camera positions within the subtile are
// active.
static bool get_subtile_position(int32_t *tile_positions,
                                 GHashTable *active_positions,
                                 const struct slide_zoom_level_params *slide_zoom_level_params,
                                 struct _openslide_jpeg_level **levels,
                                 int tiles_across,
                                 int image_divisions,
                                 int zoom_level, int xx, int yy,
                                 int *pos0_x, int *pos0_y)
{
  const struct slide_zoom_level_params *lp = slide_zoom_level_params +
      zoom_level;

  const int tile0_w = levels[0]->raw_tile_width;
  const int tile0_h = levels[0]->raw_tile_height;

  // camera position coordinates
  int xp = xx / image_divisions;
  int yp = yy / image_divisions;
  int tp = yp * (tiles_across / image_divisions) + xp;
  //g_debug("xx %d, yy %d, xp %d, yp %d, tp %d, spp %d, sc %d, tile0: %d %d subtile: %g %g", xx, yy, xp, yp, tp, subtiles_per_position, lp->subtiles_per_jpeg_tile, tile0_w, tile0_h, lp->subtile_w, lp->subtile_h);

  *pos0_x = tile_positions[tp * 2] +
      tile0_w * (xx - xp * image_divisions);
  *pos0_y = tile_positions[(tp * 2) + 1] +
      tile0_h * (yy - yp * image_divisions);

  // ensure only active positions (those present at zoom level 0) are
  // processed at higher zoom levels
  if (zoom_level == 0) {
    // If the level 0 concat factor <= image_divisions, we can simply mark
    // active any position with a corresponding level 0 tile.
    //
    // If the concat factor is larger, then active and inactive positions
    // can be merged into the same tile, and we can no longer tell which
    // subtiles can be skipped at higher zoom levels.  Sometimes such
    // positions have coordinates (0, 0) in the tile_positions map; we can
    // at least filter out these, and we must because such positions break
    // the JPEG backend's range search.  Assume that only position (0, 0)
    // can be at pixel (0, 0).
    if (tile_positions[tp * 2] == 0 && tile_positions[tp * 2 + 1] == 0 &&
        (xp != 0 || yp != 0)) {
      return false;
    }

    int *key = g_new(int, 1);
    *key = tp;
    g_hash_table_insert(active_positions, key, NULL);
    return true;

  } else {
    // make sure at least one of the positions within this subtile is active
    for (int ypp = yp; ypp < yp + lp->positions_per_subtile; ypp++) {
      for (int xpp = xp; xpp < xp + lp->positions_per_subtile; xpp++) {
        int tpp = ypp * (tiles_across / image_divisions) + xpp;
        if (g_hash_table_lookup_extended(active_positions, &tpp, NULL, NULL)) {
          //g_debug("accept tile: level %d xp %d yp %d xpp %d ypp %d", zoom_level, xp, yp, xpp, ypp);
          return true;
        }
      }
    }

    //g_debug("skip tile: level %d positions %d xp %d yp %d", zoom_level, lp->positions_per_subtile, xp, yp);
    return false;
  }
}

static bool process_hier_data_pages_from_indexfile(FILE *f,
						   int64_t seek_location,
						   int datafile_count,
						   char **datafile_names,
						   const char *dirname,
						   int zoom_levels,
						   struct _openslide_jpeg_level **levels,
						   int tiles_across,
						   int tiles_down,
						   int image_divisions,
						   const struct slide_zoom_level_params *slide_zoom_level_params,
						   int32_t *tile_positions,
						   GList **jpegs_list,
						   struct _openslide_hash *quickhash1,
						   GError **err) {
  int32_t jpeg_number = 0;

  bool success = false;

  // used for storing which positions actually have data
  GHashTable *active_positions = g_hash_table_new_full(g_int_hash, g_int_equal,
						       g_free, NULL);

  for (int zoom_level = 0; zoom_level < zoom_levels; zoom_level++) {
    struct _openslide_jpeg_level *l = levels[zoom_level];
    const struct slide_zoom_level_params *lp = slide_zoom_level_params +
        zoom_level;
    int32_t ptr;

    //    g_debug("reading zoom_level %d", zoom_level);

    if (fseeko(f, seek_location, SEEK_SET) == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot seek to zoom level pointer %d", zoom_level);
      goto DONE;
    }

    ptr = read_le_int32_from_file(f);
    if (ptr == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read zoom level pointer");
      goto DONE;
    }
    if (fseeko(f, ptr, SEEK_SET) == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot seek to start of data pages");
      goto DONE;
    }

    // read initial 0
    if (read_le_int32_from_file(f) != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Expected 0 value at beginning of data page");
      goto DONE;
    }

    // read pointer
    ptr = read_le_int32_from_file(f);
    if (ptr == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read initial data page pointer");
      goto DONE;
    }

    // seek to offset
    if (fseeko(f, ptr, SEEK_SET) == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't seek to initial data page");
      goto DONE;
    }

    int32_t next_ptr;
    do {
      // read length
      int32_t page_len = read_le_int32_from_file(f);
      if (page_len == -1) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Can't read page length");
        goto DONE;
      }

      //    g_debug("page_len: %d", page_len);

      // read "next" pointer
      next_ptr = read_le_int32_from_file(f);
      if (next_ptr == -1) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Cannot read \"next\" pointer");
        goto DONE;
      }

      // read all the data into the list
      for (int i = 0; i < page_len; i++) {
	int32_t tile_index = read_le_int32_from_file(f);
	int32_t offset = read_le_int32_from_file(f);
	int32_t length = read_le_int32_from_file(f);
	int32_t fileno = read_le_int32_from_file(f);

	if (tile_index < 0) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "tile_index < 0");
          goto DONE;
	}
	if (offset < 0) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "offset < 0");
          goto DONE;
	}
	if (length < 0) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "length < 0");
          goto DONE;
	}
	if (fileno < 0) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "fileno < 0");
          goto DONE;
	}

	// we have only encountered images with exactly power-of-two scale
	// factors, and there appears to be no clear way to specify otherwise,
	// so require it
	int32_t x = tile_index % tiles_across;
	int32_t y = tile_index / tiles_across;

	if (y >= tiles_down) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "y (%d) outside of bounds for zoom level (%d)",
                      y, zoom_level);
          goto DONE;
	}

	if (x % (1 << zoom_level)) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "x (%d) not correct multiple for zoom level (%d)",
                      x, zoom_level);
          goto DONE;
	}
	if (y % (1 << zoom_level)) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "y (%d) not correct multiple for zoom level (%d)",
                      y, zoom_level);
          goto DONE;
	}

	// save filename
	if (fileno >= datafile_count) {
          g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                      "Invalid fileno");
          goto DONE;
	}
	char *filename = g_build_filename(dirname, datafile_names[fileno], NULL);

	// hash in the lowest-res on-disk tiles
	if (zoom_level == zoom_levels - 1) {
	  if (!_openslide_hash_file_part(quickhash1, filename, offset, length, err)) {
            g_free(filename);
            g_prefix_error(err, "Can't hash tiles: ");
            goto DONE;
	  }
	}

	// populate the file structure
	struct _openslide_jpeg_file *jpeg = g_slice_new0(struct _openslide_jpeg_file);
	jpeg->filename = filename;
	jpeg->start_in_file = offset;
	jpeg->end_in_file = jpeg->start_in_file + length;
	jpeg->tw = l->raw_tile_width;
	jpeg->th = l->raw_tile_height;
	jpeg->w = l->raw_tile_width;
	jpeg->h = l->raw_tile_height;

	*jpegs_list = g_list_prepend(*jpegs_list, jpeg);

	/*
	g_debug("tile_concat: %d, subtiles_per_jpeg_tile: %d",
		lp->tile_concat, lp->subtiles_per_jpeg_tile);
	g_debug("found %d %d from file", x, y);
	*/


	// start processing 1 JPEG tile into subtiles_per_jpeg_tile^2 subtiles
	for (int yi = 0; yi < lp->subtiles_per_jpeg_tile; yi++) {
	  int yy = y + (yi * image_divisions);
	  if (yy >= tiles_down) {
	    break;
	  }

	  for (int xi = 0; xi < lp->subtiles_per_jpeg_tile; xi++) {
	    int xx = x + (xi * image_divisions);
	    if (xx >= tiles_across) {
	      break;
	    }

	    // xx and yy are the tile coordinates in level0 space

	    // position in level 0
            int pos0_x;
            int pos0_y;
            if (!get_subtile_position(tile_positions,
                                      active_positions,
                                      slide_zoom_level_params,
                                      levels,
                                      tiles_across,
                                      image_divisions,
                                      zoom_level,
                                      xx, yy,
                                      &pos0_x, &pos0_y)) {
              // no such position
              continue;
            }

	    // position in this level
	    const double pos_x = ((double) pos0_x) / lp->tile_concat;
	    const double pos_y = ((double) pos0_y) / lp->tile_concat;

	    //g_debug("pos0: %d %d, pos: %g %g", pos0_x, pos0_y, pos_x, pos_y);

	    insert_subtile(l->tiles, jpeg_number,
			   pos_x, pos_y,
			   lp->subtile_w * xi, lp->subtile_h * yi,
			   lp->subtile_w, lp->subtile_h,
			   l->tile_advance_x, l->tile_advance_y,
			   x / lp->tile_count_divisor + xi,
			   y / lp->tile_count_divisor + yi,
			   tiles_across / lp->tile_count_divisor,
			   zoom_level);
	  }
	}
	jpeg_number++;
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

static int inflate_buffer(const void *src, int srcLen, void *dst, int dstLen,
                          GError **err) {
  z_stream strm = {0};
  strm.total_in = strm.avail_in  = srcLen;
  strm.total_out = strm.avail_out = dstLen;
  strm.next_in = (Bytef *) src;
  strm.next_out = (Bytef *) dst;

  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;

  int error_code = -1;
  int ret = -1;

  // 15 window bits, and the +32 tells zlib to to detect if using gzip or zlib
  error_code = inflateInit2(&strm, (15 + 32));
  if (error_code == Z_OK) {
    error_code = inflate(&strm, Z_FINISH);
    if (error_code == Z_STREAM_END) {
      ret = strm.total_out;
    }
    else {
      goto ERROR;
    }
  } else {
      goto ERROR;
  }

  inflateEnd(&strm);
  return ret;
  
 ERROR:
  if (Z_BUF_ERROR == error_code) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Decompressed slide position buffer not of expected size");
  }
  else if (Z_MEM_ERROR == error_code) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Not enough memory to decompress");
  }
  else if (Z_DATA_ERROR == error_code) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Unrecognizable or corrupt compressed stream");
  }
  inflateEnd(&strm);
  return error_code;
}

static int read_slide_position_file(const char *path,
					 int64_t size, int64_t offset, char **buffer,
					 GError **err) {
  int buffer_size = size;
  FILE *f = _openslide_fopen(path, "rb", err);
  if (!f) {
    g_prefix_error(err, "Cannot open slide position file: ");
    return 0;
  }

  if (fseeko(f, offset, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot seek slide position file");
    fclose(f);
    return 0;
  }
  
  *buffer = g_malloc(size);
  if (fread(*buffer, sizeof(char), size, f) != size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Error while reading slide position buffer");

    g_free(*buffer);
    *buffer = NULL;

    fclose(f);
    return 0;
  }
  
  fclose(f);
  return buffer_size;
}

static int32_t *read_slide_position_buffer(const char *buffer,
					 int buffer_size,
					 int level_0_tile_concat,
					 GError **err) {
					 
  if (buffer_size % SLIDE_POSITION_RECORD_SIZE != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Unexpected buffer size");
    return 0;
  }

  const char *p = buffer;
  int count = buffer_size / SLIDE_POSITION_RECORD_SIZE;
  int32_t *result = g_new(int, count * 2);
  int32_t x;
  int32_t y;

  //  g_debug("tile positions count: %d", count);

  for (int i = 0; i < count; i++) {
    p++;  // skip flag byte
    
    // then read two integers
    x = *((int32_t *)p);
    p += sizeof(int32_t);
    y = *((int32_t *)p);
    p += sizeof(int32_t);
    
    result[i * 2] = x * level_0_tile_concat;
    result[(i * 2) + 1] = y * level_0_tile_concat;
  }

  return result;
}

static bool add_associated_image(const char *dirname,
                                 GHashTable *ht,
                                 FILE *indexfile,
                                 int64_t nonhier_root,
                                 int datafile_count,
                                 char **datafile_names,
                                 const char *name,
                                 int recordno,
                                 GError **err) {
  char *path;
  int64_t size;
  int64_t offset;
  bool result = false;

  if (recordno == -1) {
    // no such image
    return true;
  }

  if (read_nonhier_record(indexfile, nonhier_root, dirname,
                          datafile_count, datafile_names, recordno,
                          &path, &size, &offset, err)) {
    result = _openslide_add_jpeg_associated_image(ht, name, path, offset, err);
    g_free(path);
  }

  if (!result) {
    g_prefix_error(err, "Cannot read %s associated image: ", name);
  }
  return result;
}


static bool process_indexfile(const char *uuid,
			      const char *dirname,
			      int datafile_count,
			      char **datafile_names,
			      int slide_position_record,
			      int stitching_intensity_record,
			      int macro_record,
			      int label_record,
			      int thumbnail_record,
			      GHashTable *associated_images,
			      int zoom_levels,
			      int tiles_x,
			      int tiles_y,
			      double overlap_x,
			      double overlap_y,
			      int image_divisions,
			      const struct slide_zoom_level_params *slide_zoom_level_params,
			      FILE *indexfile,
			      struct _openslide_jpeg_level **levels,
			      int *file_count_out,
			      struct _openslide_jpeg_file ***files_out,
			      struct _openslide_hash *quickhash1,
			      GError **err) {
  // init out parameters
  *file_count_out = 0;
  *files_out = NULL;

  char *teststr = NULL;
  bool match;
  
  char *tile_position_buffer = NULL;
  int read_buffer_size = 0;
  int tile_position_record = -1;

  // init tmp parameters
  int32_t ptr = -1;

  const int ntiles = (tiles_x / image_divisions) * (tiles_y / image_divisions);
  const int tile_position_buffer_size = SLIDE_POSITION_RECORD_SIZE * ntiles;

  struct _openslide_jpeg_file **jpegs = NULL;
  bool success = false;

  int32_t *slide_positions = NULL;
  GList *jpegs_list = NULL;

  rewind(indexfile);

  // save root positions
  const int64_t hier_root = strlen(INDEX_VERSION) + strlen(uuid);
  const int64_t nonhier_root = hier_root + 4;

  // verify version and uuid
  teststr = read_string_from_file(indexfile, strlen(INDEX_VERSION));
  match = (teststr != NULL) && (strcmp(teststr, INDEX_VERSION) == 0);
  g_free(teststr);
  if (!match) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Index.dat doesn't have expected version");
    goto DONE;
  }

  teststr = read_string_from_file(indexfile, strlen(uuid));
  match = (teststr != NULL) && (strcmp(teststr, uuid) == 0);
  g_free(teststr);
  if (!match) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Index.dat doesn't have a matching slide identifier");
    goto DONE;
  }
  
  // If we have individual tile positioning information as part of the
  // non-hier data, read the position information.
  if (slide_position_record != -1) {
    tile_position_record = slide_position_record;
  } else {
    tile_position_record = stitching_intensity_record;
  }

  if (tile_position_record != -1) {
    char *slide_position_path;
    int64_t slide_position_size;
    int64_t slide_position_offset;
    if (!read_nonhier_record(indexfile,
			     nonhier_root,
			     dirname,
			     datafile_count,
			     datafile_names,
			     tile_position_record,
			     &slide_position_path,
			     &slide_position_size,
			     &slide_position_offset,
			     err)) {
      g_prefix_error(err, "Cannot read slide position info: ");
      goto DONE;
    }
    
    read_buffer_size = read_slide_position_file(slide_position_path,
					       slide_position_size, 
					       slide_position_offset,
					       &tile_position_buffer, 
					       err);
    g_free(slide_position_path);
    
    if (!tile_position_buffer) {
      goto DONE;
    }
    
    if (tile_position_record == stitching_intensity_record) {
      //MRXS 2.2 we need to decompress the buffer
      char *decompressed = g_malloc(tile_position_buffer_size);
      int decompress_result = inflate_buffer(tile_position_buffer, read_buffer_size, 
                                            decompressed, tile_position_buffer_size,
                                            err);
      
      g_free(tile_position_buffer); // free the compressed buffer

      if (decompress_result == tile_position_buffer_size) {
        tile_position_buffer = decompressed;
        read_buffer_size = tile_position_buffer_size;
      } else {
        g_free(decompressed);
        goto DONE;
      }
    }

    if (tile_position_buffer_size != read_buffer_size) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Slide position file not of expected size");

      g_free(tile_position_buffer);

      goto DONE;
    }

    // read in the slide positions
    slide_positions = read_slide_position_buffer(tile_position_buffer,
					       tile_position_buffer_size,
					       slide_zoom_level_params[0].tile_concat,
					       err);

    g_free(tile_position_buffer);

    if (!slide_positions) {
      goto DONE;
    }
  } else {
    // No position map available.  Fill in our own values based on the tile
    // size and nominal overlap.
    const int tile0_w = levels[0]->raw_tile_width;
    const int tile0_h = levels[0]->raw_tile_height;
    const int positions_x = tiles_x / image_divisions;

    slide_positions = g_new(int, ntiles * 2);
    for (int i = 0; i < ntiles; i++) {
      slide_positions[(i * 2)]     = (i % positions_x) *
                                     (tile0_w * image_divisions - overlap_x);
      slide_positions[(i * 2) + 1] = (i / positions_x) *
                                     (tile0_h * image_divisions - overlap_y);
    }
  }

  // read in the associated images
  if (!add_associated_image(dirname,
                            associated_images,
                            indexfile,
                            nonhier_root,
                            datafile_count,
                            datafile_names,
                            "macro",
                            macro_record,
                            err)) {
    goto DONE;
  }
  if (!add_associated_image(dirname,
                            associated_images,
                            indexfile,
                            nonhier_root,
                            datafile_count,
                            datafile_names,
                            "label",
                            label_record,
                            err)) {
    goto DONE;
  }
  if (!add_associated_image(dirname,
                            associated_images,
                            indexfile,
                            nonhier_root,
                            datafile_count,
                            datafile_names,
                            "thumbnail",
                            thumbnail_record,
                            err)) {
    goto DONE;
  }

  // read hierarchical sections
  if (fseeko(indexfile, hier_root, SEEK_SET) == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot seek to hier sections root");
    goto DONE;
  }

  ptr = read_le_int32_from_file(indexfile);
  if (ptr == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read initial pointer");
    goto DONE;
  }

  // read these pages in
  if (!process_hier_data_pages_from_indexfile(indexfile,
					      ptr,
					      datafile_count,
					      datafile_names,
					      dirname,
					      zoom_levels,
					      levels,
					      tiles_x,
					      tiles_y,
					      image_divisions,
					      slide_zoom_level_params,
					      slide_positions,
					      &jpegs_list,
					      quickhash1,
					      err)) {
    goto DONE;
  }

  success = true;

 DONE:
  // reverse list
  jpegs_list = g_list_reverse(jpegs_list);

  // copy file structures
  int jpeg_count = g_list_length(jpegs_list);
  jpegs = g_new(struct _openslide_jpeg_file *, jpeg_count);

  int cur_file = 0;
  for (GList *iter = jpegs_list; iter != NULL; iter = iter->next) {
    jpegs[cur_file++] = iter->data;
  }
  g_assert(cur_file == jpeg_count);

  // deallocate
  g_free(slide_positions);
  g_list_free(jpegs_list);

  if (success) {
    *file_count_out = jpeg_count;
    *files_out = jpegs;
  } else {
    for (int i = 0; i < jpeg_count; i++) {
      struct _openslide_jpeg_file *jpeg = jpegs[i];
      g_free(jpeg->filename);
      g_slice_free(struct _openslide_jpeg_file, jpeg);
    }
    g_free(jpegs);
  }

  return success;
}

static void add_properties(GHashTable *ht, GKeyFile *kf) {
  g_hash_table_insert(ht,
		      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
		      g_strdup("mirax"));

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
	g_hash_table_insert(ht,
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
  *name_count_out = 0;
  *name_index_out = 0;

  int offset = 0;
  for (int i = 0; i < nonhier_count; i++) {
    *name_index_out = i;

    // look at a key's value
    char *key = g_strdup_printf(KEY_NONHIER_d_NAME, i);
    char *value = g_key_file_get_value(keyfile, group,
				       key, NULL);
    g_free(key);

    if (!value) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read value for nonhier name");
      return -1;
    }

    // save count for this name
    key = g_strdup_printf(KEY_NONHIER_d_COUNT, i);
    int count = g_key_file_get_integer(keyfile, group,
				       key, NULL);
    g_free(key);
    if (!count) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read nonhier val count");
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
    char *value = g_key_file_get_value(keyfile, group,
				       key, NULL);
    g_free(key);

    if (!value) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read value for nonhier key");
      return -1;
    }

    if (strcmp(target_value, value) == 0) {
      g_free(value);
      return offset;
    }

    // otherwise, increase offset
    g_free(value);
    offset++;
  }

  return -1;
}

bool _openslide_try_mirax(openslide_t *osr, const char *filename,
			  struct _openslide_hash *quickhash1,
			  GError **err) {
  struct _openslide_jpeg_file **jpegs = NULL;
  int num_jpegs = 0;
  struct _openslide_jpeg_level **levels = NULL;

  char *dirname = NULL;

  GKeyFile *slidedat = NULL;
  GError *tmp_err = NULL;

  bool success = false;
  char *tmp = NULL;

  // info about this slide
  char *slide_version = NULL;
  char *slide_id = NULL;
  int tiles_x = 0;
  int tiles_y = 0;
  int image_divisions = 0;
  int objective_magnification = 0;

  char *index_filename = NULL;
  int zoom_levels = 0;
  int hier_count = 0;
  int nonhier_count = 0;
  int position_nonhier_offset = -1;
  int position_nonhier_stitching_offset = -1;  // used for MRXS 2.2
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
  char **datafile_names = NULL;

  FILE *indexfile = NULL;

  GHashTable *associated_images = NULL;

  int64_t base_w = 0;
  int64_t base_h = 0;

  int total_concat_exponent = 0;

  // start reading

  // verify filename
  if (!g_str_has_suffix(filename, MRXS_EXT) ||
      !g_file_test(filename, G_FILE_TEST_EXISTS)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a MIRAX slide");
    goto FAIL;
  }

  // get directory from filename
  dirname = g_strndup(filename, strlen(filename) - strlen(MRXS_EXT));

  // first, check slidedat
  tmp = g_build_filename(dirname, SLIDEDAT_INI, NULL);
  // hash the slidedat
  if (!_openslide_hash_file(quickhash1, tmp, err)) {
    goto FAIL;
  }

  slidedat = g_key_file_new();
  if (!_openslide_read_key_file(slidedat, tmp, G_KEY_FILE_NONE, err)) {
    g_prefix_error(err, "Can't load Slidedat.ini file: ");
    goto FAIL;
  }
  g_free(tmp);
  tmp = NULL;

  // add properties
  if (osr) {
    add_properties(osr->properties, slidedat);
  }

  // load general stuff
  HAVE_GROUP_OR_FAIL(slidedat, GROUP_GENERAL);

  READ_KEY_OR_FAIL(slide_version, slidedat, GROUP_GENERAL,
		   KEY_SLIDE_VERSION, value, "Can't read slide version");
  READ_KEY_OR_FAIL(slide_id, slidedat, GROUP_GENERAL,
		   KEY_SLIDE_ID, value, "Can't read slide id");
  READ_KEY_OR_FAIL(tiles_x, slidedat, GROUP_GENERAL,
		   KEY_IMAGENUMBER_X, integer, "Can't read tiles across");
  READ_KEY_OR_FAIL(tiles_y, slidedat, GROUP_GENERAL,
		   KEY_IMAGENUMBER_Y, integer, "Can't read tiles down");
  READ_KEY_OR_FAIL(objective_magnification, slidedat, GROUP_GENERAL,
		   KEY_OBJECTIVE_MAGNIFICATION, integer,
		   "Can't read objective magnification");

  image_divisions = g_key_file_get_integer(slidedat, GROUP_GENERAL,
					   KEY_CAMERA_IMAGE_DIVISIONS_PER_SIDE,
					   &tmp_err);
  if (tmp_err != NULL) {
    image_divisions = 1;
    g_clear_error(&tmp_err);
  }

  // ensure positive values
  POSITIVE_OR_FAIL(tiles_x);
  POSITIVE_OR_FAIL(tiles_y);
  POSITIVE_OR_FAIL(image_divisions);

  // load hierarchical stuff
  HAVE_GROUP_OR_FAIL(slidedat, GROUP_HIERARCHICAL);

  READ_KEY_OR_FAIL(hier_count, slidedat, GROUP_HIERARCHICAL,
		   KEY_HIER_COUNT, integer, "Can't read hier count");
  READ_KEY_OR_FAIL(nonhier_count, slidedat, GROUP_HIERARCHICAL,
		   KEY_NONHIER_COUNT, integer, "Can't read nonhier count");

  POSITIVE_OR_FAIL(hier_count);
  NON_NEGATIVE_OR_FAIL(nonhier_count);

  // find key for slide zoom level
  for (int i = 0; i < hier_count; i++) {
    char *key = g_strdup_printf(KEY_HIER_d_NAME, i);
    char *value = g_key_file_get_value(slidedat, GROUP_HIERARCHICAL,
				       key, NULL);
    g_free(key);

    if (!value) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read value for hier name");
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
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't find slide zoom level");
    goto FAIL;
  }

  // TODO allow slide_zoom_level_value to be at another hierarchy value
  if (slide_zoom_level_value != 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Slide zoom level not HIER_0");
    goto FAIL;
  }

  READ_KEY_OR_FAIL(index_filename, slidedat, GROUP_HIERARCHICAL,
		   KEY_INDEXFILE, value, "Can't read index filename");
  READ_KEY_OR_FAIL(zoom_levels, slidedat, GROUP_HIERARCHICAL,
		   key_slide_zoom_level_count, integer, "Can't read zoom levels");
  POSITIVE_OR_FAIL(zoom_levels);


  slide_zoom_level_section_names = g_new0(char *, zoom_levels + 1);
  for (int i = 0; i < zoom_levels; i++) {
    tmp = g_strdup_printf(KEY_HIER_d_VAL_d_SECTION, slide_zoom_level_value, i);

    READ_KEY_OR_FAIL(slide_zoom_level_section_names[i], slidedat, GROUP_HIERARCHICAL,
		     tmp, value, "Can't read section name");

    g_free(tmp);
    tmp = NULL;
  }

  // load datafile stuff
  HAVE_GROUP_OR_FAIL(slidedat, GROUP_DATAFILE);

  READ_KEY_OR_FAIL(datafile_count, slidedat, GROUP_DATAFILE,
		   KEY_FILE_COUNT, integer, "Can't read datafile count");
  POSITIVE_OR_FAIL(datafile_count);

  datafile_names = g_new0(char *, datafile_count + 1);
  for (int i = 0; i < datafile_count; i++) {
    tmp = g_strdup_printf(KEY_d_FILE, i);

    READ_KEY_OR_FAIL(datafile_names[i], slidedat, GROUP_DATAFILE,
		     tmp, value, "Can't read datafile name");

    g_free(tmp);
    tmp = NULL;
  }

  // load data from all slide_zoom_level_section_names sections
  slide_zoom_level_sections = g_new0(struct slide_zoom_level_section, zoom_levels);
  for (int i = 0; i < zoom_levels; i++) {
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;

    int bgr;

    char *group = slide_zoom_level_section_names[i];
    HAVE_GROUP_OR_FAIL(slidedat, group);

    READ_KEY_OR_FAIL(hs->concat_exponent, slidedat, group,
		     KEY_IMAGE_CONCAT_FACTOR,
		     integer, "Can't read image concat exponent");
    READ_KEY_OR_FAIL(hs->overlap_x, slidedat, group, KEY_OVERLAP_X,
		     double, "Can't read overlap X");
    READ_KEY_OR_FAIL(hs->overlap_y, slidedat, group, KEY_OVERLAP_Y,
		     double, "Can't read overlap Y");
    READ_KEY_OR_FAIL(hs->mpp_x, slidedat, group, KEY_MPP_X,
		     double, "Can't read micrometers/pixel X");
    READ_KEY_OR_FAIL(hs->mpp_y, slidedat, group, KEY_MPP_Y,
		     double, "Can't read micrometers/pixel Y");
    READ_KEY_OR_FAIL(bgr, slidedat, group, KEY_IMAGE_FILL_COLOR_BGR,
		     integer, "Can't read image fill color");
    READ_KEY_OR_FAIL(hs->tile_w, slidedat, group, KEY_DIGITIZER_WIDTH,
		     integer, "Can't read tile width");
    READ_KEY_OR_FAIL(hs->tile_h, slidedat, group, KEY_DIGITIZER_HEIGHT,
		     integer, "Can't read tile height");

    if (i == 0) {
      NON_NEGATIVE_OR_FAIL(hs->concat_exponent);
    } else {
      POSITIVE_OR_FAIL(hs->concat_exponent);
    }
    POSITIVE_OR_FAIL(hs->tile_w);
    POSITIVE_OR_FAIL(hs->tile_h);

    // convert fill color bgr into rgb
    hs->fill_rgb =
      ((bgr << 16) & 0x00FF0000) |
      (bgr & 0x0000FF00) |
      ((bgr >> 16) & 0x000000FF);

    // verify we are JPEG
    READ_KEY_OR_FAIL(tmp, slidedat, group, KEY_IMAGE_FORMAT,
		     value, "Can't read image format");
    if (strcmp(tmp, "JPEG") != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Level %d not JPEG", i);
      goto FAIL;
    }
    g_free(tmp);
    tmp = NULL;
  }


  // load position stuff
  // find key for position, if present
  position_nonhier_offset = get_nonhier_name_offset(slidedat,
						    nonhier_count,
						    GROUP_HIERARCHICAL,
						    VALUE_VIMSLIDE_POSITION_BUFFER,
						    &tmp_err);
  SUCCESSFUL_OR_FAIL(tmp_err);

  if (position_nonhier_offset == -1) {
    position_nonhier_stitching_offset = get_nonhier_name_offset(slidedat,
						      nonhier_count,
						      GROUP_HIERARCHICAL,
						      VALUE_STITCHING_INTENSITY_LAYER,
						      &tmp_err);
    SUCCESSFUL_OR_FAIL(tmp_err);
  }

  // associated images
  macro_nonhier_offset = get_nonhier_val_offset(slidedat,
						nonhier_count,
						GROUP_HIERARCHICAL,
						VALUE_SCAN_DATA_LAYER,
						VALUE_SCAN_DATA_LAYER_MACRO,
						&tmp_err);
  SUCCESSFUL_OR_FAIL(tmp_err);
  label_nonhier_offset = get_nonhier_val_offset(slidedat,
						nonhier_count,
						GROUP_HIERARCHICAL,
						VALUE_SCAN_DATA_LAYER,
						VALUE_SCAN_DATA_LAYER_LABEL,
						&tmp_err);
  SUCCESSFUL_OR_FAIL(tmp_err);
  thumbnail_nonhier_offset = get_nonhier_val_offset(slidedat,
						    nonhier_count,
						    GROUP_HIERARCHICAL,
						    VALUE_SCAN_DATA_LAYER,
						    VALUE_SCAN_DATA_LAYER_THUMBNAIL,
						    &tmp_err);
  SUCCESSFUL_OR_FAIL(tmp_err);

  /*
  g_debug("dirname: %s", dirname);
  g_debug("slide_version: %s", slide_version);
  g_debug("slide_id: %s", slide_id);
  g_debug("tiles (%d,%d)", tiles_x, tiles_y);
  g_debug("index_filename: %s", index_filename);
  g_debug("zoom_levels: %d", zoom_levels);
  for (int i = 0; i < zoom_levels; i++) {
    g_debug(" section name %d: %s", i, slide_zoom_level_section_names[i]);
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;
    g_debug("  overlap_x: %g", hs->overlap_x);
    g_debug("  overlap_y: %g", hs->overlap_y);
    g_debug("  mpp_x: %g", hs->mpp_x);
    g_debug("  mpp_y: %g", hs->mpp_y);
    g_debug("  fill_rgb: %" G_GUINT32_FORMAT, hs->fill_rgb);
    g_debug("  tile_w: %d", hs->tile_w);
    g_debug("  tile_h: %d", hs->tile_h);
  }
  g_debug("datafile_count: %d", datafile_count);
  for (int i = 0; i < datafile_count; i++) {
    g_debug(" datafile name %d: %s", i, datafile_names[i]);
  }
  g_debug("position_nonhier_offset: %d", position_nonhier_offset);
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
  // Then, the photo is split into image_divisions^2 JPEG tiles.
  // So, if image_division=2, you'll get 4 JPEG tiles per photo.
  // If image_division=4, then 16 JPEG tiles per photo.
  //
  // The overlap is on the original photo, not the JPEG tiles.
  // What you'll get is position data for only a subset
  // of JPEG tiles.
  // The JPEG tiles that come from a photo must be placed edge-to-edge.
  //
  // To generate levels, each JPEG tile is downsampled by 2 and then
  // concatenated into a new JPEG tile, 4 old tiles per new JPEG tile (2x2).
  // Note that this is per-tile, not per-photo. Repeat this several
  // times.
  //
  // The downsampling and concatenation is fine up until you start
  // concatenating JPEG tiles that come from different photos.
  // Then the positions don't line up and JPEG tiles must be split into
  // subtiles. This significantly complicates the code.

  // compute dimensions base_w and base_h in stupid but clear way
  base_w = 0;
  base_h = 0;

  for (int i = 0; i < tiles_x; i++) {
    if (((i % image_divisions) != (image_divisions - 1))
	|| (i == tiles_x - 1)) {
      // full size
      base_w += slide_zoom_level_sections[0].tile_w;
    } else {
      // size minus overlap
      base_w += slide_zoom_level_sections[0].tile_w - slide_zoom_level_sections[0].overlap_x;
    }
  }
  for (int i = 0; i < tiles_y; i++) {
    if (((i % image_divisions) != (image_divisions - 1))
	|| (i == tiles_y - 1)) {
      // full size
      base_h += slide_zoom_level_sections[0].tile_h;
    } else {
      // size minus overlap
      base_h += slide_zoom_level_sections[0].tile_h - slide_zoom_level_sections[0].overlap_y;
    }
  }


  // set up level dimensions and such
  levels = g_new(struct _openslide_jpeg_level *, zoom_levels);
  slide_zoom_level_params = g_new(struct slide_zoom_level_params, zoom_levels);
  total_concat_exponent = 0;
  for (int i = 0; i < zoom_levels; i++) {
    // one jpeg level per zoom level
    struct _openslide_jpeg_level *l = g_slice_new0(struct _openslide_jpeg_level);
    levels[i] = l;
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;
    struct slide_zoom_level_params *lp = slide_zoom_level_params + i;

    // tile_concat: number of tiles concatenated from the original in one dimension
    total_concat_exponent += hs->concat_exponent;
    lp->tile_concat = 1 << total_concat_exponent;

    // positions_per_jpeg_tile: for this zoom, how many camera positions
    //                          are represented in a JPEG tile?
    //                          this is constant for the first few levels,
    //                          depending on image_divisions
    const int positions_per_jpeg_tile = MAX(1, lp->tile_concat / image_divisions);

    if (position_nonhier_offset != -1
        || position_nonhier_stitching_offset != -1
        || slide_zoom_level_sections[0].overlap_x != 0
        || slide_zoom_level_sections[0].overlap_y != 0) {
      // tile_count_divisor: as we record levels, we would prefer to shrink the
      //                     number of tiles, but keep the tile size constant,
      //                     but this only works until we encounter JPEG tiles
      //                     with more than one source photo, in which case
      //                     the tile count bottoms out and we instead shrink
      //                     the advances
      lp->tile_count_divisor = MIN(lp->tile_concat, image_divisions);

      // subtiles_per_jpeg_tile: for this zoom, how many subtiles in a JPEG tile?
      //                         this is constant for the first few levels,
      //                         depending on image_divisions
      lp->subtiles_per_jpeg_tile = positions_per_jpeg_tile;

      // positions_per_subtile: for this zoom, how many camera positions
      //                        are represented in a subtile?
      lp->positions_per_subtile = 1;

    } else {
      // no position file and no overlaps, so we can skip subtile processing
      // for better performance

      lp->tile_count_divisor = lp->tile_concat;
      lp->subtiles_per_jpeg_tile = 1;
      lp->positions_per_subtile = positions_per_jpeg_tile;
    }

    lp->subtile_w = (double) hs->tile_w / lp->subtiles_per_jpeg_tile;
    lp->subtile_h = (double) hs->tile_h / lp->subtiles_per_jpeg_tile;

    l->tiles = _openslide_jpeg_create_tiles_table();
    l->level_w = base_w / lp->tile_concat;  // tile_concat is powers of 2
    l->level_h = base_h / lp->tile_concat;
    l->tiles_across = tiles_x / lp->tile_count_divisor;
    l->tiles_down = tiles_y / lp->tile_count_divisor;
    l->raw_tile_width = hs->tile_w;  // raw JPEG size
    l->raw_tile_height = hs->tile_h;

    // subtiles_per_position: for this zoom, how many subtiles (in one dimension)
    //                        come from a single photo?
    const int subtiles_per_position = MAX(1, image_divisions / lp->tile_concat);

    // use a fraction of the overlap, so that our tile correction will flip between
    // positive and negative values typically (in case image_divisions=2)
    // this is because not every tile overlaps

    // overlaps are concatenated within physical tiles, so our virtual tile
    // size must shrink, once we hit image_divisions
    l->tile_advance_x = lp->subtile_w - ((double) hs->overlap_x /
        (double) subtiles_per_position);
    l->tile_advance_y = lp->subtile_h - ((double) hs->overlap_y /
        (double) subtiles_per_position);

    // override downsample.  level 0 is defined to have a downsample of 1.0,
    // irrespective of its concat_exponent
    l->downsample = lp->tile_concat / slide_zoom_level_params[0].tile_concat;

    //g_debug("level %d tile advance %.10g %.10g, dim %" G_GINT64_FORMAT " %" G_GINT64_FORMAT ", tiles %d %d, rawtile %d %d, subtile %g %g, tile_concat %d, tile_count_divisor %d, positions_per_subtile %d", i, l->tile_advance_x, l->tile_advance_y, l->level_w, l->level_h, l->tiles_across, l->tiles_down, l->raw_tile_width, l->raw_tile_height, lp->subtile_w, lp->subtile_h, lp->tile_concat, lp->tile_count_divisor, lp->positions_per_subtile);
  }

  // load the position map and build up the tiles, using subtiles
  if (osr) {
    associated_images = osr->associated_images;
  }
  
  if (!process_indexfile(slide_id,
			 dirname,
			 datafile_count, datafile_names,
			 position_nonhier_offset,
			 position_nonhier_stitching_offset,
			 macro_nonhier_offset,
			 label_nonhier_offset,
			 thumbnail_nonhier_offset,
			 associated_images,
			 zoom_levels,
			 tiles_x, tiles_y,
			 slide_zoom_level_sections[0].overlap_x,
			 slide_zoom_level_sections[0].overlap_y,
			 image_divisions,
			 slide_zoom_level_params,
			 indexfile,
			 levels,
			 &num_jpegs, &jpegs,
			 quickhash1,
			 err)) {
    goto FAIL;
  }

  if (osr) {
    uint32_t fill = slide_zoom_level_sections[0].fill_rgb;
    _openslide_set_background_color_prop(osr->properties,
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
  }

  _openslide_add_jpeg_ops(osr, num_jpegs, jpegs, zoom_levels, levels);

  success = true;
  goto DONE;

 FAIL:
  if (levels != NULL) {
    for (int i = 0; i < zoom_levels; i++) {
      struct _openslide_jpeg_level *l = levels[i];
      g_hash_table_unref(l->tiles);
      g_slice_free(struct _openslide_jpeg_level, l);
    }
    g_free(levels);
  }
  g_free(jpegs);

  success = false;

 DONE:
  g_free(dirname);
  g_free(tmp);
  g_free(slide_version);
  g_free(slide_id);
  g_free(index_filename);
  g_strfreev(datafile_names);
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
