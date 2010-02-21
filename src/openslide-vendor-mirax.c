/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include <jpeglib.h>

#include "openslide-private.h"
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

struct slide_zoom_level_section {
  double overlap_x;
  double overlap_y;

  uint32_t fill_rgb;

  int tile_w;
  int tile_h;
};

struct mirax_hier_page_entry {
  int32_t x;
  int32_t y;
  int32_t offset;
  int32_t length;
  int32_t fileno;
  int zoom_level;
};

struct mirax_nonhier_page_entry {
  int32_t offset;
  int32_t length;
  int32_t fileno;
};

static bool verify_string_from_file(FILE *f, const char *str) {
  bool result;
  int len = strlen(str);

  char *possible_str = g_malloc(len + 1);
  possible_str[len] = '\0';
  size_t size = fread(possible_str, len, 1, f);

  //  g_debug("\"%s\" == \"%s\" ?", str, possible_str);

  result = (size == 1) && (strcmp(str, possible_str) == 0);

  g_free(possible_str);
  return result;
}

static int32_t read_le_int32_from_file(FILE *f) {
  int32_t i;

  if (fread(&i, 4, 1, f) != 1) {
    return -1;
  }

  i = GINT32_FROM_LE(i);
  //  g_debug("%d", i);

  return i;
}


static bool read_nonhier_record(FILE *f,
				int64_t nonhier_root_position,
				int recordno,
				int *fileno, int64_t *size, int64_t *position) {
  if (fseeko(f, nonhier_root_position, SEEK_SET) == -1) {
    g_warning("Cannot seek to nonhier root");
    return false;
  }

  int32_t ptr = read_le_int32_from_file(f);
  if (ptr == -1) {
    g_warning("Can't read initial nonhier pointer");
    return false;
  }

  // jump to start of interesting data
  //  g_debug("seek %d", ptr);
  if (fseeko(f, ptr, SEEK_SET) == -1) {
    g_warning("Cannot seek to start of nonhier data");
    return false;
  }

  // seek to record pointer
  if (fseeko(f, recordno * 4, SEEK_CUR) == -1) {
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


static bool process_hier_data_pages_from_indexfile(FILE *f,
						   off_t seek_location,
						   int datafile_count,
						   char **datafile_names,
						   const char *dirname,
						   int zoom_levels,
						   struct _openslide_jpeg_layer **layers,
						   int tiles_across,
						   int tiles_down,
						   int tiles_per_position,
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
    int32_t ptr;

    //    g_debug("reading zoom_level %d", zoom_level);

    if (fseeko(f, seek_location, SEEK_SET) == -1) {
      g_warning("Cannot seek to zoom level pointer %d", zoom_level + 1);
      goto OUT;
    }

    ptr = read_le_int32_from_file(f);
    if (ptr == -1) {
      g_warning("Can't read zoom level pointer");
      goto OUT;
    }
    if (fseeko(f, ptr, SEEK_SET) == -1) {
      g_warning("Cannot seek to start of data pages");
      goto OUT;
    }

    // read initial 0
    if (read_le_int32_from_file(f) != 0) {
      g_warning("Expected 0 value at beginning of data page");
      goto OUT;
    }

    // read pointer
    ptr = read_le_int32_from_file(f);
    if (ptr == -1) {
      g_warning("Can't read initial data page pointer");
      goto OUT;
    }

    // seek to offset
    if (fseeko(f, ptr, SEEK_SET) == -1) {
      g_warning("Can't seek to initial data page");
      goto OUT;
    }

    int32_t next_ptr;
    do {
      // read length
      int32_t page_len = read_le_int32_from_file(f);
      if (page_len == -1) {
	g_warning("Can't read page length");
	goto OUT;
      }

      //    g_debug("page_len: %d", page_len);

      // read "next" pointer
      next_ptr = read_le_int32_from_file(f);
      if (next_ptr == -1) {
	g_warning("Cannot read \"next\" pointer");
	goto OUT;
      }

      // read all the data into the list
      for (int i = 0; i < page_len; i++) {
	int32_t tile_index = read_le_int32_from_file(f);
	int32_t offset = read_le_int32_from_file(f);
	int32_t length = read_le_int32_from_file(f);
	int32_t fileno = read_le_int32_from_file(f);

	if (tile_index < 0) {
	  g_warning("tile_index < 0");
	  goto OUT;
	}
	if (offset < 0) {
	  g_warning("offset < 0");
	  goto OUT;
	}
	if (length < 0) {
	  g_warning("length < 0");
	  goto OUT;
	}
	if (fileno < 0) {
	  g_warning("fileno < 0");
	  goto OUT;
	}

	// we have only encountered images with exactly power-of-two scale
	// factors, and there appears to be no clear way to specify otherwise,
	// so require it
	int32_t x = tile_index % tiles_across;
	int32_t y = tile_index / tiles_across;

	if (y >= tiles_down) {
	  g_warning("y (%d) outside of bounds for zoom level (%d)",
		    y, zoom_level);
	  goto OUT;
	}

	if (x % (1 << zoom_level)) {
	  g_warning("x (%d) not correct multiple for zoom level (%d)",
		    x, zoom_level);
	  goto OUT;
	}
	if (y % (1 << zoom_level)) {
	  g_warning("y (%d) not correct multiple for zoom level (%d)",
		    y, zoom_level);
	  goto OUT;
	}

	// save filename
	if (fileno >= datafile_count) {
	  g_warning("Invalid fileno");
	  goto OUT;
	}
	char *filename = g_build_filename(dirname, datafile_names[fileno], NULL);

	// hash in the lowest-res on-disk tiles
	if (zoom_level == zoom_levels - 1) {
	  _openslide_hash_file_part(quickhash1, filename, offset, length);
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


	// make 2^zoom_level * 2^zoom_level tiles for this jpeg
	int tile_count = 1 << zoom_level;
	for (int yi = 0; yi < tile_count; yi++) {
	  int yy = y + yi;
	  if (yy >= tiles_down) {
	    break;
	  }

	  for (int xi = 0; xi < tile_count; xi++) {
	    int xx = x + xi;
	    if (xx >= tiles_across) {
	      break;
	    }

	    // look up the tile position, stored in 24.8 fixed point
	    int xp = xx / tiles_per_position;
	    int yp = yy / tiles_per_position;
	    int tp = yp * (tiles_across / tiles_per_position) + xp;
	    double pos_x = ((double) tile_positions[tp * 2]) / 256.0;
	    double pos_y = ((double) tile_positions[(tp * 2) + 1]) / 256.0;

	    if (zoom_level == 0) {
	      // if the zoom level is 0, then mark this position as active
	      int *key = g_new(int, 1);
	      *key = tp;
	      g_hash_table_insert(active_positions, key, NULL);
	    } else {
	      // make sure we have an active position for this tile
	      if (!g_hash_table_lookup_extended(active_positions, &tp, NULL, NULL)) {
		continue;
	      }
	    }

	    // adjust
	    pos_x += jpeg->w * (xx - (xp * tiles_per_position));
	    pos_y += jpeg->h * (yy - (yp * tiles_per_position));

	    // scale down
	    pos_x /= (double) tile_count;
	    pos_y /= (double) tile_count;

	    // generate tile
	    struct _openslide_jpeg_tile *tile = g_slice_new0(struct _openslide_jpeg_tile);
	    tile->fileno = jpeg_number;
	    tile->tileno = 0;

	    double tw = (double) jpeg->w / (double) tile_count;
	    double th = (double) jpeg->h / (double) tile_count;

	    tile->src_x = tw * xi;
	    tile->src_y = th * yi;
	    tile->w = ceil(tw);  // XXX best compromise for now
	    tile->h = ceil(th);

	    // compute offset
	    tile->dest_offset_x = pos_x - (xx * l->tile_advance_x);
	    tile->dest_offset_y = pos_y - (yy * l->tile_advance_y);

	    /*
	    g_debug("tile %d %d, pos %.10g %.10g, offset %.10g %.10g",
		    xx, yy, pos_x, pos_y, tile->dest_offset_x, tile->dest_offset_y);

	    g_debug(" src %.10g %.10g dim %.10g %.10g",
		    tile->src_x, tile->src_y, tile->w, tile->h);
	    */

	    // insert
	    int64_t *key = g_slice_new(int64_t);
	    *key = (yy * tiles_across) + xx;
	    g_hash_table_insert(l->tiles, key, tile);
	  }
	}
	jpeg_number++;
      }
    } while (next_ptr != 0);

    // advance for next zoom level
    seek_location += 4;
  }

  success = true;

 OUT:
  g_hash_table_unref(active_positions);

  return success;
}

static int32_t *read_slide_position_file(const char *dirname, const char *name,
					 int64_t size, int64_t offset) {
  char *tmp = g_build_filename(dirname, name, NULL);
  FILE *f = fopen(tmp, "rb");
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
    // read 2 numbers, then a null
    int32_t x = read_le_int32_from_file(f);
    int32_t y = read_le_int32_from_file(f);

    uint8_t zz;
    int fread_result = fread(&zz, 1, 1, f);

    if ((x == -1) || (y == -1) || (zz != 0) || (fread_result != 1)) {
      g_warning("Error while reading slide position file");
      fclose(f);
      g_free(result);
      return NULL;
    }

    result[i * 2] = x;
    result[(i * 2) + 1] = y;
  }

  fclose(f);
  return result;
}

static void add_associated_image(const char *dirname,
				 const char *filename,
				 int64_t offset,
				 GHashTable *ht,
				 const char *name) {
  if (ht == NULL) {
    return;
  }

  char *tmp = g_build_filename(dirname, filename, NULL);
  FILE *f = fopen(tmp, "rb");
  g_free(tmp);

  if (!f) {
    g_warning("Cannot open associated image file");
    return;
  }

  if (fseeko(f, offset, SEEK_SET) == -1) {
    g_warning("Cannot seek to offset");
    fclose(f);
    return;
  }

  _openslide_add_jpeg_associated_image(ht, name, f);

  fclose(f);
}



static bool process_indexfile(const char *slideversion,
			      const char *uuid,
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
			      int tiles_per_position,
			      FILE *indexfile,
			      struct _openslide_jpeg_layer **layers,
			      int *file_count_out,
			      struct _openslide_jpeg_file ***files_out,
			      struct _openslide_hash *quickhash1) {
  // init out parameters
  *file_count_out = 0;
  *files_out = NULL;

  struct _openslide_jpeg_file **jpegs = NULL;
  bool success = false;

  int32_t *slide_positions = NULL;
  GList *jpegs_list = NULL;

  rewind(indexfile);

  // verify slideversion and uuid
  if (!(verify_string_from_file(indexfile, slideversion) &&
	verify_string_from_file(indexfile, uuid))) {
    g_warning("Indexfile doesn't start with expected values");
    goto OUT;
  }

  // save root positions
  int64_t hier_root = ftello(indexfile);
  int64_t nonhier_root = hier_root + 4;

  // read in the slide position info
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
    goto OUT;
  }
  //  g_debug("slide position: fileno %d size %" PRId64 " offset %" PRId64, slide_position_fileno, slide_position_size, slide_position_offset);

  if (slide_position_size != (9 * (tiles_x / tiles_per_position) * (tiles_y / tiles_per_position))) {
    g_warning("Slide position file not of expected size");
    goto OUT;
  }

  // read in the slide positions
  slide_positions = read_slide_position_file(dirname,
					     datafile_names[slide_position_fileno],
					     slide_position_size,
					     slide_position_offset);
  if (!slide_positions) {
    g_warning("Cannot read slide positions");
    goto OUT;
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
    add_associated_image(dirname,
			 datafile_names[tmp_fileno],
			 tmp_offset,
			 associated_images,
			 "macro");
  }
  if (read_nonhier_record(indexfile,
			  nonhier_root,
			  label_record,
			  &tmp_fileno,
			  &tmp_size,
			  &tmp_offset)) {
    add_associated_image(dirname,
			 datafile_names[tmp_fileno],
			 tmp_offset,
			 associated_images,
			 "label");
  }
  if (read_nonhier_record(indexfile,
			  nonhier_root,
			  thumbnail_record,
			  &tmp_fileno,
			  &tmp_size,
			  &tmp_offset)) {
    add_associated_image(dirname,
			 datafile_names[tmp_fileno],
			 tmp_offset,
			 associated_images,
			 "thumbnail");
  }

  // read hierarchical sections
  if (fseeko(indexfile, hier_root, SEEK_SET) == -1) {
    g_warning("Cannot seek to hier sections root");
    goto OUT;
  }

  int32_t ptr = read_le_int32_from_file(indexfile);
  if (ptr == -1) {
    g_warning("Can't read initial pointer");
    goto OUT;
  }

  // jump to start of interesting data
  //  g_debug("seek %d", ptr);
  if (fseeko(indexfile, ptr, SEEK_SET) == -1) {
    g_warning("Cannot seek to start of interesting data");
    goto OUT;
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
					      tiles_per_position,
					      slide_positions,
					      &jpegs_list,
					      quickhash1)) {
    g_warning("Cannot read some data pages from indexfile");
    goto OUT;
  }

  success = true;

 OUT:
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

  bool success = false;
  char *tmp = NULL;

  // info about this slide
  char *slide_version = NULL;
  char *slide_id = NULL;
  int tiles_x = 0;
  int tiles_y = 0;
  int tiles_per_position = 0;

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

  int datafile_count = 0;
  char **datafile_names = NULL;

  FILE *indexfile = NULL;

  // start reading

  // verify filename
  if (!g_str_has_suffix(filename, MRXS_EXT)) {
    goto FAIL;
  }

  // get directory from filename
  dirname = g_strndup(filename, strlen(filename) - strlen(MRXS_EXT));

  // first, check slidedat
  tmp = g_build_filename(dirname, SLIDEDAT_INI, NULL);
  _openslide_hash_file(quickhash1, tmp);  // hash the slidedat
  slidedat = g_key_file_new();
  if (!g_key_file_load_from_file(slidedat, tmp, G_KEY_FILE_NONE, NULL)) {
    g_warning("Can't load Slidedat file");
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
  READ_KEY_OR_FAIL(tiles_per_position, slidedat, GROUP_GENERAL,
		   KEY_CAMERA_IMAGE_DIVISIONS_PER_SIDE, integer,
		   "Can't read camera image divisions per side");

  // ensure positive values
  POSITIVE_OR_FAIL(tiles_x);
  POSITIVE_OR_FAIL(tiles_y);
  POSITIVE_OR_FAIL(tiles_per_position);

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
  POSITIVE_OR_FAIL(nonhier_count);

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

    // verify IMAGE_CONCAT_FACTOR == 1 for all but the first layer
    int ic_factor;
    READ_KEY_OR_FAIL(ic_factor, slidedat, group, KEY_IMAGE_CONCAT_FACTOR,
		     integer, "Can't read image concat factor");
    if ((i == 0) && (ic_factor != 0)) {
      g_warning("Level 0 has non-zero image concat factor: %d", ic_factor);
      goto FAIL;
    }
    if ((i != 0) && (ic_factor != 1)) {
      g_warning("Level %d has non-unity image concat factor: %d", i, ic_factor);
      goto FAIL;
    }
  }


  // load position stuff
  // find key for position
  position_nonhier_offset = get_nonhier_name_offset(slidedat,
						    nonhier_count,
						    GROUP_HIERARCHICAL,
						    VALUE_VIMSLIDE_POSITION_BUFFER);
  if (position_nonhier_offset == -1) {
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
  g_debug("position_nonheir_offset: %d", position_nonhier_offset);
  */

  // read indexfile
  tmp = g_build_filename(dirname, index_filename, NULL);
  indexfile = fopen(tmp, "rb");
  g_free(tmp);
  tmp = NULL;

  if (!indexfile) {
    g_warning("Cannot open index file");
    goto FAIL;
  }

  // compute dimensions in stupid but clear way
  int64_t base_w = 0;
  int64_t base_h = 0;

  for (int i = 0; i < tiles_x; i++) {
    if (((i % tiles_per_position) != (tiles_per_position - 1))
	|| (i == tiles_x - 1)) {
      // full size
      base_w += slide_zoom_level_sections[0].tile_w;
    } else {
      // size minus overlap
      base_w += slide_zoom_level_sections[0].tile_w - slide_zoom_level_sections[0].overlap_x;
    }
  }
  for (int i = 0; i < tiles_y; i++) {
    if (((i % tiles_per_position) != (tiles_per_position - 1))
	|| (i == tiles_y - 1)) {
      // full size
      base_h += slide_zoom_level_sections[0].tile_h;
    } else {
      // size minus overlap
      base_h += slide_zoom_level_sections[0].tile_h - slide_zoom_level_sections[0].overlap_y;
    }
  }


  // set up layers
  layers = g_new(struct _openslide_jpeg_layer *, zoom_levels);
  for (int i = 0; i < zoom_levels; i++) {
    struct _openslide_jpeg_layer *l = g_slice_new0(struct _openslide_jpeg_layer);
    layers[i] = l;
    struct slide_zoom_level_section *hs = slide_zoom_level_sections + i;

    int divisor = 1 << i;

    l->tiles = _openslide_jpeg_create_tiles_table();
    l->layer_w = base_w / divisor;
    l->layer_h = base_h / divisor;
    l->tiles_across = tiles_x;    // number of virtual tiles stay constant, they just shrink
    l->tiles_down = tiles_y;
    l->raw_tile_width = hs->tile_w;
    l->raw_tile_height = hs->tile_h;

    // use a fraction of the overlap, so that our tile correction will flip between
    // positive and negative values typically (in case tiles_per_position=2)
    // this is because not every tile overlaps

    // overlaps are concatenated within physical tiles, so our virtual tile
    // size must shrink
    l->tile_advance_x = (((double) hs->tile_w) / ((double) divisor)) -
      ((double) hs->overlap_x / (double) tiles_per_position);
    l->tile_advance_y = (((double) hs->tile_h) / ((double) divisor)) -
      ((double) hs->overlap_y / (double) tiles_per_position);

    //    g_debug("layer %d tile advance %.10g %.10g", i, l->tile_advance_x, l->tile_advance_y);
  }

  // load the position map and build up the tiles, using subtiles
  GHashTable *associated_images = NULL;
  if (osr) {
    associated_images = osr->associated_images;
  }
  if (!process_indexfile(slide_version, slide_id,
			 dirname,
			 datafile_count, datafile_names,
			 position_nonhier_offset,
			 macro_nonhier_offset,
			 label_nonhier_offset,
			 thumbnail_nonhier_offset,
			 associated_images,
			 zoom_levels,
			 tiles_x, tiles_y,
			 tiles_per_position,
			 indexfile,
			 layers,
			 &num_jpegs, &jpegs,
			 quickhash1)) {
    goto FAIL;
  }

  if (osr) {
    uint32_t fill = slide_zoom_level_sections[0].fill_rgb;
    osr->fill_color_r = ((fill >> 16) & 0xFF) / 255.0;
    osr->fill_color_g = ((fill >> 8) & 0xFF) / 255.0;
    osr->fill_color_b = (fill & 0xFF) / 255.0;
  }

  _openslide_add_jpeg_ops(osr, num_jpegs, jpegs, zoom_levels, layers);

  // override downsamples
  if (osr) {
    osr->downsamples = g_new(double, osr->layer_count);
    double downsample = 1.0;

    for (int32_t i = 0; i < osr->layer_count; i++) {
      osr->downsamples[i] = downsample;
      downsample *= 2.0;
    }
  }

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
