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

#include "openslide-hash.h"

static const char MRXS_EXT[] = ".mrxs";
static const char SLIDEDAT_INI[] = "Slidedat.ini";

static const char GROUP_GENERAL[] = "GENERAL";
static const char KEY_SLIDE_VERSION[] = "SLIDE_VERSION";
static const char KEY_SLIDE_ID[] = "SLIDE_ID";
static const char KEY_IMAGENUMBER_X[] = "IMAGENUMBER_X";
static const char KEY_IMAGENUMBER_Y[] = "IMAGENUMBER_Y";
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
static const char VALUE_SCAN_DATA_LAYER[] = "Scan data layer";
static const char VALUE_SCAN_DATA_LAYER_MACRO[] = "ScanDataLayer_SlideThumbnail";
static const char VALUE_SCAN_DATA_LAYER_LABEL[] = "ScanDataLayer_SlideBarcode";
static const char VALUE_SCAN_DATA_LAYER_THUMBNAIL[] = "ScanDataLayer_SlidePreview";
static const char VALUE_SLIDE_ZOOM_LEVEL[] = "Slide zoom level";

static const char GROUP_NONHIERLAYER_d_SECTION[] = "NONHIERLAYER_%d_SECTION";
static const char KEY_VIMSLIDE_POSITION_DATA_FORMAT_VERSION[] =
  "VIMSLIDE_POSITION_DATA_FORMAT_VERSION";
static const int VALUE_VIMSLIDE_POSITION_DATA_FORMAT_VERSION = 257;

static const char GROUP_DATAFILE[] = "DATAFILE";
static const char KEY_FILE_COUNT[] = "FILE_COUNT";
static const char KEY_d_FILE[] = "FILE_%d";

static const char KEY_OVERLAP_X[] = "OVERLAP_X";
static const char KEY_OVERLAP_Y[] = "OVERLAP_Y";
static const char KEY_IMAGE_FORMAT[] = "IMAGE_FORMAT";
static const char KEY_IMAGE_FILL_COLOR_BGR[] = "IMAGE_FILL_COLOR_BGR";
static const char KEY_DIGITIZER_WIDTH[] = "DIGITIZER_WIDTH";
static const char KEY_DIGITIZER_HEIGHT[] = "DIGITIZER_HEIGHT";
static const char KEY_IMAGE_CONCAT_FACTOR[] = "IMAGE_CONCAT_FACTOR";

#define READ_KEY_OR_FAIL(TARGET, KEYFILE, GROUP, KEY, TYPE, FAIL_MSG)	\
  do {									\
    GError *err = NULL;							\
    TARGET = g_key_file_get_ ## TYPE(KEYFILE, GROUP, KEY, &err);	\
    if (err != NULL) {							\
      g_warning(FAIL_MSG); g_error_free(err); goto FAIL;		\
    }									\
  } while(0)

#define POSITIVE_OR_FAIL(N)					\
  do {								\
    if (N <= 0) {						\
      g_warning(#N " <= 0: %d", N); goto FAIL;			\
    }								\
  } while(0)

#define NON_NEGATIVE_OR_FAIL(N)					\
  do {								\
    if (N < 0) {						\
      g_warning(#N " < 0: %d", N); goto FAIL;			\
    }								\
  } while(0)

struct slide_zoom_level_section {
  int concat_exponent;

  double overlap_x;
  double overlap_y;

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
  char *str = (char *) g_malloc(len + 1);
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
				int recordno,
				int *fileno, int64_t *size, int64_t *position) {
  if (recordno == -1)
    return false;

  if (fseeko(f, nonhier_root_position, SEEK_SET) == -1) {
    g_warning("Cannot seek to nonhier root");
    return false;
  }

  int32_t ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_warning("Can't read initial nonhier pointer");
    return false;
  }

  // seek to record pointer
  if (fseeko(f, ptr + 4 * recordno, SEEK_SET) == -1) {
    g_warning("Cannot seek to nonhier record pointer %d", recordno);
    return false;
  }

  // read pointer
  ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_warning("Can't read nonhier record %d", recordno);
    return false;
  }

  // seek
  if (fseeko(f, ptr, SEEK_SET) == -1) {
    g_warning("Cannot seek to nonhier record %d", recordno);
    return false;
  }

  // read initial 0
  if (read_le_int32_from_file(f) != 0) {
    g_warning("Expected 0 value at beginning of data page");
    return false;
  }

  // read pointer
  ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_warning("Can't read initial data page pointer");
    return false;
  }

  // seek to offset
  if (fseeko(f, ptr, SEEK_SET) == -1) {
    g_warning("Can't seek to initial data page");
    return false;
  }

  // read pagesize == 1
  if (read_le_int32_from_file(f) != 1) {
    g_warning("Expected 1 value");
    return false;
  }

  // read 3 zeroes
  if (read_le_int32_from_file(f) != 0) {
    g_warning("Expected first 0 value");
    return false;
  }
  if (read_le_int32_from_file(f) != 0) {
    g_warning("Expected second 0 value");
    return false;
  }
  if (read_le_int32_from_file(f) != 0) {
    g_warning("Expected third 0 value");
    return false;
  }

  // finally read offset, size, fileno
  *position = read_le_int32_from_file(f);
  if (*position == -1) {
    g_warning("Can't read position");
    return false;
  }
  *size = read_le_int32_from_file(f);
  if (*size == -1) {
    g_warning("Can't read size");
    return false;
  }
  *fileno = read_le_int32_from_file(f);
  if (*fileno == -1) {
    g_warning("Can't read fileno");
    return false;
  }

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

// given the coordinates of a subtile, compute its layer 0 pixel coordinates.
// return false if none of the camera positions within the subtile are
// active.
static bool get_subtile_position(int32_t *tile_positions,
                                 GHashTable *active_positions,
                                 const struct slide_zoom_level_params *slide_zoom_level_params,
                                 struct _openslide_jpeg_layer **layers,
                                 int tiles_across,
                                 int image_divisions,
                                 int zoom_level, int xx, int yy,
                                 int *pos0_x, int *pos0_y)
{
  const struct slide_zoom_level_params *lp = slide_zoom_level_params +
      zoom_level;

  const int tile0_w = layers[0]->raw_tile_width;
  const int tile0_h = layers[0]->raw_tile_height;

  // camera position coordinates
  int xp = xx / image_divisions;
  int yp = yy / image_divisions;
  int tp = yp * (tiles_across / image_divisions) + xp;
  //g_debug("xx %d, yy %d, xp %d, yp %d, tp %d, spp %d, sc %d, tile0: %d %d subtile: %g %g", xx, yy, xp, yp, tp, subtiles_per_position, lp->subtiles_per_jpeg_tile, tile0_w, tile0_h, lp->subtile_w, lp->subtile_h);

  *pos0_x = tile_positions[tp * 2] +
      tile0_w * (xx - xp * image_divisions);
  *pos0_y = tile_positions[(tp * 2) + 1] +
      tile0_h * (yy - yp * image_divisions);

  if (zoom_level == 0) {
    // if the zoom level is 0, then mark this position as active
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
						   struct _openslide_jpeg_layer **layers,
						   int tiles_across,
						   int tiles_down,
						   int image_divisions,
						   const struct slide_zoom_level_params *slide_zoom_level_params,
						   int32_t *tile_positions,
						   GList **jpegs_list,
						   struct _openslide_hash *quickhash1) {
  int32_t jpeg_number = 0;

  bool success = false;

  // used for storing which positions actually have data
  GHashTable *active_positions = g_hash_table_new_full(g_int_hash, g_int_equal,
						       g_free, NULL);

  for (int zoom_level = 0; zoom_level < zoom_levels; zoom_level++) {
    struct _openslide_jpeg_layer *l = layers[zoom_level];
    const struct slide_zoom_level_params *lp = slide_zoom_level_params +
        zoom_level;
    int32_t ptr;

    //    g_debug("reading zoom_level %d", zoom_level);

    if (fseeko(f, seek_location, SEEK_SET) == -1) {
      g_warning("Cannot seek to zoom level pointer %d", zoom_level + 1);
      goto DONE;
    }

    ptr = read_le_int32_from_file(f);
    if (ptr == -1) {
      g_warning("Can't read zoom level pointer");
      goto DONE;
    }
    if (fseeko(f, ptr, SEEK_SET) == -1) {
      g_warning("Cannot seek to start of data pages");
      goto DONE;
    }

    // read initial 0
    if (read_le_int32_from_file(f) != 0) {
      g_warning("Expected 0 value at beginning of data page");
      goto DONE;
    }

    // read pointer
    ptr = read_le_int32_from_file(f);
    if (ptr == -1) {
      g_warning("Can't read initial data page pointer");
      goto DONE;
    }

    // seek to offset
    if (fseeko(f, ptr, SEEK_SET) == -1) {
      g_warning("Can't seek to initial data page");
      goto DONE;
    }

    int32_t next_ptr;
    do {
      // read length
      int32_t page_len = read_le_int32_from_file(f);
      if (page_len == -1) {
	g_warning("Can't read page length");
	goto DONE;
      }

      //    g_debug("page_len: %d", page_len);

      // read "next" pointer
      next_ptr = read_le_int32_from_file(f);
      if (next_ptr == -1) {
	g_warning("Cannot read \"next\" pointer");
	goto DONE;
      }

      // read all the data into the list
      for (int i = 0; i < page_len; i++) {
	int32_t tile_index = read_le_int32_from_file(f);
	int32_t offset = read_le_int32_from_file(f);
	int32_t length = read_le_int32_from_file(f);
	int32_t fileno = read_le_int32_from_file(f);

	if (tile_index < 0) {
	  g_warning("tile_index < 0");
	  goto DONE;
	}
	if (offset < 0) {
	  g_warning("offset < 0");
	  goto DONE;
	}
	if (length < 0) {
	  g_warning("length < 0");
	  goto DONE;
	}
	if (fileno < 0) {
	  g_warning("fileno < 0");
	  goto DONE;
	}

	// we have only encountered images with exactly power-of-two scale
	// factors, and there appears to be no clear way to specify otherwise,
	// so require it
	int32_t x = tile_index % tiles_across;
	int32_t y = tile_index / tiles_across;

	if (y >= tiles_down) {
	  g_warning("y (%d) outside of bounds for zoom level (%d)",
		    y, zoom_level);
	  goto DONE;
	}

	if (x % (1 << zoom_level)) {
	  g_warning("x (%d) not correct multiple for zoom level (%d)",
		    x, zoom_level);
	  goto DONE;
	}
	if (y % (1 << zoom_level)) {
	  g_warning("y (%d) not correct multiple for zoom level (%d)",
		    y, zoom_level);
	  goto DONE;
	}

	// save filename
	if (fileno >= datafile_count) {
	  g_warning("Invalid fileno");
	  goto DONE;
	}
	char *filename = g_build_filename(dirname, datafile_names[fileno], NULL);

	// hash in the lowest-res on-disk tiles
	if (zoom_level == zoom_levels - 1) {
	  if (!_openslide_hash_file_part(quickhash1, filename, offset, length)) {
	    g_free(filename);
	    g_warning("Can't hash tiles");
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

	    // position in layer 0
            int pos0_x;
            int pos0_y;
            if (!get_subtile_position(tile_positions,
                                      active_positions,
                                      slide_zoom_level_params,
                                      layers,
                                      tiles_across,
                                      image_divisions,
                                      zoom_level,
                                      xx, yy,
                                      &pos0_x, &pos0_y)) {
              // no such position
              continue;
            }

	    // position in this layer
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

static int32_t *read_slide_position_file(const char *dirname, const char *name,
					 int64_t size, int64_t offset,
					 int level_0_tile_concat) {
  char *tmp = g_build_filename(dirname, name, NULL);
  FILE *f = _openslide_fopen(tmp, "rb");
  g_free(tmp);

  if (!f) {
    g_warning("Cannot open slide position file");
    return NULL;
  }

  if (fseeko(f, offset, SEEK_SET) == -1) {
    g_warning("Cannot seek to offset");
    fclose(f);
    return NULL;
  }

  int count = size / 9;
  int32_t *result = g_new(int, count * 2);

  //  g_debug("tile positions count: %d", count);

  for (int i = 0; i < count; i++) {
    // read flag byte, then 2 numbers
    int zz = getc(f);

    int32_t x;
    int32_t y;
    bool x_ok = read_le_int32_from_file_with_result(f, &x);
    bool y_ok = read_le_int32_from_file_with_result(f, &y);

    if (zz == EOF || !x_ok || !y_ok || (zz & 0xfe)) {
      g_warning("Error while reading slide position file (%d)", zz);
      fclose(f);
      g_free(result);
      return NULL;
    }

    result[i * 2] = x * level_0_tile_concat;
    result[(i * 2) + 1] = y * level_0_tile_concat;
  }

  fclose(f);
  return result;
}

static bool add_associated_image(const char *dirname,
				 const char *filename,
				 int64_t offset,
				 GHashTable *ht,
				 const char *name) {
  char *tmp = g_build_filename(dirname, filename, NULL);

  bool result = _openslide_add_jpeg_associated_image(ht, name, tmp, offset);

  g_free(tmp);
  return result;
}



static bool process_indexfile(const char *uuid,
			      const char *dirname,
			      int datafile_count,
			      char **datafile_names,
			      int slide_position_record,
			      int macro_record,
			      int label_record,
			      int thumbnail_record,
			      GHashTable *associated_images,
			      int zoom_levels,
			      int tiles_x,
			      int tiles_y,
			      int image_divisions,
			      const struct slide_zoom_level_params *slide_zoom_level_params,
			      FILE *indexfile,
			      struct _openslide_jpeg_layer **layers,
			      int *file_count_out,
			      struct _openslide_jpeg_file ***files_out,
			      struct _openslide_hash *quickhash1) {
  // init out parameters
  *file_count_out = 0;
  *files_out = NULL;

  char *teststr = NULL;
  bool match;

  // init tmp parameters
  int32_t ptr = -1;

  const int ntiles = (tiles_x / image_divisions) * (tiles_y / image_divisions);

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
    g_warning("Index.dat doesn't have expected version");
    goto DONE;
  }

  teststr = read_string_from_file(indexfile, strlen(uuid));
  match = (teststr != NULL) && (strcmp(teststr, uuid) == 0);
  g_free(teststr);
  if (!match) {
    g_warning("Index.dat doesn't have a matching slide identifier");
    goto DONE;
  }

  // If we have individual tile positioning information as part of the
  // non-hier data, read the position information.
  if (slide_position_record != -1) {
    int slide_position_fileno;
    int64_t slide_position_size;
    int64_t slide_position_offset;
    if (!read_nonhier_record(indexfile,
			     nonhier_root,
			     slide_position_record,
			     &slide_position_fileno,
			     &slide_position_size,
			     &slide_position_offset)) {
      g_warning("Cannot read slide position info");
      goto DONE;
    }
    //  g_debug("slide position: fileno %d size %" G_GINT64_FORMAT " offset %" G_GINT64_FORMAT, slide_position_fileno, slide_position_size, slide_position_offset);

    if (slide_position_size != (9 * ntiles)) {
      g_warning("Slide position file not of expected size");
      goto DONE;
    }

    // read in the slide positions
    slide_positions = read_slide_position_file(dirname,
					       datafile_names[slide_position_fileno],
					       slide_position_size,
					       slide_position_offset,
					       slide_zoom_level_params[0].tile_concat);
  } else {
    // no position map available and we know overlap is 0, fill in our own
    // values based on the known tile size.
    const int tile0_w = layers[0]->raw_tile_width;
    const int tile0_h = layers[0]->raw_tile_height;

    slide_positions = g_new(int, ntiles * 2);
    for (int i = 0; i < ntiles; i++) {
      slide_positions[(i * 2)]     = (i % tiles_x) * tile0_w;
      slide_positions[(i * 2) + 1] = (i / tiles_x) * tile0_h;
    }
  }
  if (!slide_positions) {
    g_warning("Cannot read slide positions");
    goto DONE;
  }


  // read in the associated images
  int tmp_fileno;
  int64_t tmp_size;
  int64_t tmp_offset;

  if (read_nonhier_record(indexfile,
			  nonhier_root,
			  macro_record,
			  &tmp_fileno,
			  &tmp_size,
			  &tmp_offset)) {
    if (!add_associated_image(dirname,
			      datafile_names[tmp_fileno],
			      tmp_offset,
			      associated_images,
			      "macro")) {
      g_warning("Cannot read macro associated image");
      goto DONE;
    }
  }
  if (read_nonhier_record(indexfile,
			  nonhier_root,
			  label_record,
			  &tmp_fileno,
			  &tmp_size,
			  &tmp_offset)) {
    if (!add_associated_image(dirname,
			      datafile_names[tmp_fileno],
			      tmp_offset,
			      associated_images,
			      "label")) {
      g_warning("Cannot read label associated image");
      goto DONE;
    }
  }
  if (read_nonhier_record(indexfile,
			  nonhier_root,
			  thumbnail_record,
			  &tmp_fileno,
			  &tmp_size,
			  &tmp_offset)) {
    if (!add_associated_image(dirname,
			      datafile_names[tmp_fileno],
			      tmp_offset,
			      associated_images,
			      "thumbnail")) {
      g_warning("Cannot read thumbnail associated image");
      goto DONE;
    }
  }

  // read hierarchical sections
  if (fseeko(indexfile, hier_root, SEEK_SET) == -1) {
    g_warning("Cannot seek to hier sections root");
    goto DONE;
  }

  ptr = read_le_int32_from_file(indexfile);
  if (ptr == -1) {
    g_warning("Can't read initial pointer");
    goto DONE;
  }

  // read these pages in
  if (!process_hier_data_pages_from_indexfile(indexfile,
					      ptr,
					      datafile_count,
					      datafile_names,
					      dirname,
					      zoom_levels,
					      layers,
					      tiles_x,
					      tiles_y,
					      image_divisions,
					      slide_zoom_level_params,
					      slide_positions,
					      &jpegs_list,
					      quickhash1)) {
    g_warning("Cannot read some data pages from indexfile");
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
    jpegs[cur_file++] = (struct _openslide_jpeg_file *) iter->data;
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
					  int *name_index_out) {
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
      g_warning("Can't read value for nonhier name");
      return -1;
    }

    // save count for this name
    key = g_strdup_printf(KEY_NONHIER_d_COUNT, i);
    int count = g_key_file_get_integer(keyfile, group,
				       key, NULL);
    g_free(key);
    if (!count) {
      g_warning("Can't read nonhier val count");
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
				   const char *target_name) {
  int d1, d2;
  return get_nonhier_name_offset_helper(keyfile,
					nonhier_count,
					group,
					target_name,
					&d1, &d2);
}

static int get_nonhier_val_offset(GKeyFile *keyfile,
				  int nonhier_count,
				  const char *group,
				  const char *target_name,
				  const char *target_value) {
  int name_count;
  int name_index;
  int offset = get_nonhier_name_offset_helper(keyfile, nonhier_count,
					      group, target_name,
					      &name_count,
					      &name_index);
  if (offset == -1) {
    return -1;
  }

  for (int i = 0; i < name_count; i++) {
    char *key = g_strdup_printf(KEY_NONHIER_d_VAL_d, name_index, i);
    char *value = g_key_file_get_value(keyfile, group,
				       key, NULL);
    g_free(key);

    if (!value) {
      g_warning("Can't read value for nonhier key");
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
			  struct _openslide_hash *quickhash1) {
  struct _openslide_jpeg_file **jpegs = NULL;
  int num_jpegs = 0;
  struct _openslide_jpeg_layer **layers = NULL;

  char *dirname = NULL;

  GKeyFile *slidedat = NULL;
  GError *err = NULL;

  bool success = false;
  char *tmp = NULL;

  // info about this slide
  char *slide_version = NULL;
  char *slide_id = NULL;
  int tiles_x = 0;
  int tiles_y = 0;
  int image_divisions = 0;

  char *index_filename = NULL;
  int zoom_levels = 0;
  int hier_count = 0;
  int nonhier_count = 0;
  int position_nonhier_offset = -1;
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
    goto FAIL;
  }

  // get directory from filename
  dirname = g_strndup(filename, strlen(filename) - strlen(MRXS_EXT));

  // first, check slidedat
  tmp = g_build_filename(dirname, SLIDEDAT_INI, NULL);
  // hash the slidedat
  if (!_openslide_hash_file(quickhash1, tmp)) {
    g_warning("Can't hash Slidedat file");
    goto FAIL;
  }

  slidedat = g_key_file_new();
  if (!_openslide_read_key_file(slidedat, tmp, G_KEY_FILE_NONE, NULL)) {
    g_warning("Can't load Slidedat.ini file");
    goto FAIL;
  }
  g_free(tmp);
  tmp = NULL;

  // add properties
  if (osr) {
    add_properties(osr->properties, slidedat);
  }

  // load general stuff
  if (!g_key_file_has_group(slidedat, GROUP_GENERAL)) {
    g_warning("Can't find %s group", GROUP_GENERAL);
    goto FAIL;
  }

  READ_KEY_OR_FAIL(slide_version, slidedat, GROUP_GENERAL,
		   KEY_SLIDE_VERSION, value, "Can't read slide version");
  READ_KEY_OR_FAIL(slide_id, slidedat, GROUP_GENERAL,
		   KEY_SLIDE_ID, value, "Can't read slide id");
  READ_KEY_OR_FAIL(tiles_x, slidedat, GROUP_GENERAL,
		   KEY_IMAGENUMBER_X, integer, "Can't read tiles across");
  READ_KEY_OR_FAIL(tiles_y, slidedat, GROUP_GENERAL,
		   KEY_IMAGENUMBER_Y, integer, "Can't read tiles down");

  image_divisions = g_key_file_get_integer(slidedat, GROUP_GENERAL,
					   KEY_CAMERA_IMAGE_DIVISIONS_PER_SIDE,
					   &err);
  if (err != NULL) {
    image_divisions = 1;
    g_error_free(err);
    err = NULL;
  }

  // ensure positive values
  POSITIVE_OR_FAIL(tiles_x);
  POSITIVE_OR_FAIL(tiles_y);
  POSITIVE_OR_FAIL(image_divisions);

  // load hierarchical stuff
  if (!g_key_file_has_group(slidedat, GROUP_HIERARCHICAL)) {
    g_warning("Can't find %s group", GROUP_HIERARCHICAL);
    goto FAIL;
  }

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
      g_warning("Can't read value for hier name");
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
    g_warning("Can't find slide zoom level");
    goto FAIL;
  }

  // TODO allow slide_zoom_level_value to be at another hierarchy value
  if (slide_zoom_level_value != 0) {
    g_warning("Slide zoom level not HIER_0");
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
  if (!g_key_file_has_group(slidedat, GROUP_DATAFILE)) {
    g_warning("Can't find %s group", GROUP_DATAFILE);
    goto FAIL;
  }

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
    if (!g_key_file_has_group(slidedat, group)) {
      g_warning("Can't find %s group", group);
      goto FAIL;
    }

    READ_KEY_OR_FAIL(hs->concat_exponent, slidedat, group,
		     KEY_IMAGE_CONCAT_FACTOR,
		     integer, "Can't read image concat exponent");
    READ_KEY_OR_FAIL(hs->overlap_x, slidedat, group, KEY_OVERLAP_X,
		     double, "Can't read overlap X");
    READ_KEY_OR_FAIL(hs->overlap_y, slidedat, group, KEY_OVERLAP_Y,
		     double, "Can't read overlap Y");
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
      g_warning("Level %d not JPEG", i);
      goto FAIL;
    }
    g_free(tmp);
    tmp = NULL;
  }


  // load position stuff
  // find key for position
  position_nonhier_offset = get_nonhier_name_offset(slidedat,
						    nonhier_count,
						    GROUP_HIERARCHICAL,
						    VALUE_VIMSLIDE_POSITION_BUFFER);
  // When the position map is missing we calculate it ourselves based on the
  // known tile width and height but do not take tile overlap into account.
  if ((slide_zoom_level_sections[0].overlap_x ||
       slide_zoom_level_sections[0].overlap_y)
      && position_nonhier_offset == -1) {
    g_warning("Can't figure out where the position file is");
    goto FAIL;
  }

  // associated images
  macro_nonhier_offset = get_nonhier_val_offset(slidedat,
						nonhier_count,
						GROUP_HIERARCHICAL,
						VALUE_SCAN_DATA_LAYER,
						VALUE_SCAN_DATA_LAYER_MACRO);
  label_nonhier_offset = get_nonhier_val_offset(slidedat,
						nonhier_count,
						GROUP_HIERARCHICAL,
						VALUE_SCAN_DATA_LAYER,
						VALUE_SCAN_DATA_LAYER_LABEL);
  thumbnail_nonhier_offset = get_nonhier_val_offset(slidedat,
						    nonhier_count,
						    GROUP_HIERARCHICAL,
						    VALUE_SCAN_DATA_LAYER,
						    VALUE_SCAN_DATA_LAYER_THUMBNAIL);

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
    g_debug("  fill_rgb: %" PRIu32, hs->fill_rgb);
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
  indexfile = _openslide_fopen(tmp, "rb");
  g_free(tmp);
  tmp = NULL;

  if (!indexfile) {
    g_warning("Cannot open index file");
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


  // set up layer dimensions and such
  layers = g_new(struct _openslide_jpeg_layer *, zoom_levels);
  slide_zoom_level_params = g_new(struct slide_zoom_level_params, zoom_levels);
  total_concat_exponent = 0;
  for (int i = 0; i < zoom_levels; i++) {
    // one jpeg layer per zoom level
    struct _openslide_jpeg_layer *l = g_slice_new0(struct _openslide_jpeg_layer);
    layers[i] = l;
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

    if (position_nonhier_offset != -1) {
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
    l->layer_w = base_w / lp->tile_concat;  // tile_concat is powers of 2
    l->layer_h = base_h / lp->tile_concat;
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

    // override downsample.  layer 0 is defined to have a downsample of 1.0,
    // irrespective of its concat_exponent
    l->downsample = lp->tile_concat / slide_zoom_level_params[0].tile_concat;

    //g_debug("layer %d tile advance %.10g %.10g, dim %" G_GINT64_FORMAT " %" G_GINT64_FORMAT ", tiles %d %d, rawtile %d %d, subtile %g %g, tile_concat %d, tile_count_divisor %d, positions_per_subtile %d", i, l->tile_advance_x, l->tile_advance_y, l->layer_w, l->layer_h, l->tiles_across, l->tiles_down, l->raw_tile_width, l->raw_tile_height, lp->subtile_w, lp->subtile_h, lp->tile_concat, lp->tile_count_divisor, lp->positions_per_subtile);
  }

  // load the position map and build up the tiles, using subtiles
  if (osr) {
    associated_images = osr->associated_images;
  }
  if (!process_indexfile(slide_id,
			 dirname,
			 datafile_count, datafile_names,
			 position_nonhier_offset,
			 macro_nonhier_offset,
			 label_nonhier_offset,
			 thumbnail_nonhier_offset,
			 associated_images,
			 zoom_levels,
			 tiles_x, tiles_y,
			 image_divisions,
			 slide_zoom_level_params,
			 indexfile,
			 layers,
			 &num_jpegs, &jpegs,
			 quickhash1)) {
    goto FAIL;
  }

  if (osr) {
    uint32_t fill = slide_zoom_level_sections[0].fill_rgb;
    _openslide_set_background_color_property(osr->properties,
					     (fill >> 16) & 0xFF,
					     (fill >> 8) & 0xFF,
					     fill & 0xFF);
  }

  _openslide_add_jpeg_ops(osr, num_jpegs, jpegs, zoom_levels, layers);

  success = true;
  goto DONE;

 FAIL:
  if (layers != NULL) {
    for (int i = 0; i < zoom_levels; i++) {
      struct _openslide_jpeg_layer *l = layers[i];
      g_hash_table_unref(l->tiles);
      g_slice_free(struct _openslide_jpeg_layer, l);
    }
    g_free(layers);
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
