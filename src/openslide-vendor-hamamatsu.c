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
 * Hamamatsu (VMS, VMU) support
 *
 * quickhash comes from VMS/VMU file and map2 file
 *
 */


#include <config.h>

#include "openslide-private.h"
#include "openslide-tiffdump.h"

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <jpeglib.h>

#include "openslide-hash.h"

static const char GROUP_VMS[] = "Virtual Microscope Specimen";
static const char GROUP_VMU[] = "Uncompressed Virtual Microscope Specimen";
static const char KEY_MAP_FILE[] = "MapFile";
static const char KEY_IMAGE_FILE[] = "ImageFile";
static const char KEY_NUM_LAYERS[] = "NoLayers";
static const char KEY_NUM_JPEG_COLS[] = "NoJpegColumns";
static const char KEY_NUM_JPEG_ROWS[] = "NoJpegRows";
static const char KEY_OPTIMISATION_FILE[] = "OptimisationFile";
static const char KEY_MACRO_IMAGE[] = "MacroImage";
static const char KEY_BITS_PER_PIXEL[] = "BitsPerPixel";
static const char KEY_PIXEL_ORDER[] = "PixelOrder";

// returns w and h and tw and th and comment as a convenience
static bool verify_jpeg(FILE *f, int32_t *w, int32_t *h,
			int32_t *tw, int32_t *th,
			char **comment, GError **err) {
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  jmp_buf env;
  bool success = false;

  *w = 0;
  *h = 0;
  *tw = 0;
  *th = 0;

  if (comment) {
    *comment = NULL;
  }

  if (setjmp(env) == 0) {
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
    jpeg_create_decompress(&cinfo);
    _openslide_jpeg_stdio_src(&cinfo, f);

    int header_result;

    if (comment) {
      // extract comment
      jpeg_save_markers(&cinfo, JPEG_COM, 0xFFFF);
    }

    header_result = jpeg_read_header(&cinfo, TRUE);
    if (header_result != JPEG_HEADER_OK
        && header_result != JPEG_HEADER_TABLES_ONLY) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Couldn't read JPEG header");
      goto DONE;
    }
    if (cinfo.num_components != 3) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "JPEG color components != 3");
      goto DONE;
    }
    if (cinfo.restart_interval == 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "No restart markers");
      goto DONE;
    }

    jpeg_start_decompress(&cinfo);

    if (comment) {
      if (cinfo.marker_list) {
	// copy everything out
	char *com = g_strndup((const gchar *) cinfo.marker_list->data,
			      cinfo.marker_list->data_length);
	// but only really save everything up to the first '\0'
	*comment = g_strdup(com);
	g_free(com);
      }
      jpeg_save_markers(&cinfo, JPEG_COM, 0);  // stop saving
    }

    *w = cinfo.output_width;
    *h = cinfo.output_height;

    if (cinfo.restart_interval > cinfo.MCUs_per_row) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Restart interval greater than MCUs per row");
      goto DONE;
    }

    *tw = *w / (cinfo.MCUs_per_row / cinfo.restart_interval);
    *th = *h / cinfo.MCU_rows_in_scan;
    int leftover_mcus = cinfo.MCUs_per_row % cinfo.restart_interval;
    if (leftover_mcus != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Inconsistent restart marker spacing within row");
      goto DONE;
    }


    //  g_debug("w: %d, h: %d, restart_interval: %d\n"
    //	 "mcus_per_row: %d, mcu_rows_in_scan: %d\n"
    //	 "leftover mcus: %d",
    //	 cinfo.output_width, cinfo.output_height,
    //	 cinfo.restart_interval,
    //	 cinfo.MCUs_per_row, cinfo.MCU_rows_in_scan,
    //	 leftover_mcus);
  } else {
    // setjmp has returned again
    g_propagate_error(err, jerr.err);
    goto DONE;
  }

  success = true;

DONE:
  jpeg_destroy_decompress(&cinfo);
  return success;
}

static int64_t *extract_one_optimisation(FILE *opt_f,
					 int32_t num_tiles_down,
					 int32_t num_tiles_across,
					 int32_t mcu_starts_count) {
  int64_t *mcu_starts = g_new(int64_t, mcu_starts_count);
  for (int32_t i = 0; i < mcu_starts_count; i++) {
    mcu_starts[i] = -1; // UNKNOWN value
  }

  // optimisation file is in a weird format, it is 32- (or 64- or 320- ?) bit
  // little endian values, giving the file offset into an MCU row,
  // each offset starts at a 40-byte alignment, and the last row (of the
  // entire file, not each image) seems to be missing

  // also, the offsets are all packed into 1 file, even with multiple images

  // we will read the file and verify at least that the markers
  // are valid, if anything is fishy, we will not use it

  // we represent missing data by -1, which we initialize to,
  // so if we run out of opt file, we can just stop

  for (int32_t row = 0; row < num_tiles_down; row++) {
    // read 40 bytes
    union {
      uint8_t buf[40];
      int64_t i64;
    } u;

    if (fread(u.buf, 40, 1, opt_f) != 1) {
      // EOF or error, we've done all we can

      if (row == 0) {
	// if we don't even get the first one, deallocate
	goto FAIL;
      }

      break;
    }

    // get the offset
    int64_t offset = GINT64_FROM_LE(u.i64);

    // record this marker
    mcu_starts[row * num_tiles_across] = offset;
  }

  return mcu_starts;

 FAIL:
  g_free(mcu_starts);
  return NULL;
}

static void add_properties(GHashTable *ht, GKeyFile *kf,
			   const char *group) {
  g_hash_table_insert(ht,
		      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
		      g_strdup("hamamatsu"));

  char **keys = g_key_file_get_keys(kf, group, NULL, NULL);
  if (keys == NULL) {
    return;
  }

  for (char **key = keys; *key != NULL; key++) {
    char *value = g_key_file_get_value(kf, group, *key, NULL);
    if (value) {
      g_hash_table_insert(ht,
			  g_strdup_printf("hamamatsu.%s", *key),
			  g_strdup(value));
      g_free(value);
    }
  }

  g_strfreev(keys);
}

static bool hamamatsu_vms_part2(openslide_t *osr,
				int num_jpegs, char **image_filenames,
				int num_jpeg_cols,
				FILE *optimisation_file,
				GError **err) {
  bool success = false;

  // initialize individual jpeg structs
  struct _openslide_jpeg_file **jpegs = g_new0(struct _openslide_jpeg_file *,
					       num_jpegs);
  for (int i = 0; i < num_jpegs; i++) {
    jpegs[i] = g_slice_new0(struct _openslide_jpeg_file);
  }

  // init levels: base image + map
  int32_t level_count = 2;
  struct _openslide_jpeg_level **levels =
    g_new0(struct _openslide_jpeg_level *, level_count);
  for (int32_t i = 0; i < level_count; i++) {
    levels[i] = g_slice_new0(struct _openslide_jpeg_level);
    levels[i]->tiles = _openslide_jpeg_create_tiles_table();
  }

  // process jpegs
  int32_t jpeg0_tw = 0;
  int32_t jpeg0_th = 0;
  int32_t jpeg0_ta = 0;
  int32_t jpeg0_td = 0;

  for (int i = 0; i < num_jpegs; i++) {
    struct _openslide_jpeg_file *jp = jpegs[i];

    // these jpeg files always start at 0
    jp->start_in_file = 0;
    jp->filename = g_strdup(image_filenames[i]);

    FILE *f;
    if ((f = _openslide_fopen(jp->filename, "rb", err)) == NULL) {
      g_prefix_error(err, "Can't open JPEG %d: ", i);
      goto DONE;
    }

    // comment?
    char *comment = NULL;
    char **comment_ptr = NULL;
    if (i == 0 && osr) {
      comment_ptr = &comment;
    }

    if (!verify_jpeg(f, &jp->w, &jp->h, &jp->tw, &jp->th, comment_ptr, err)) {
      g_prefix_error(err, "Can't verify JPEG %d: ", i);
      fclose(f);
      goto DONE;
    }

    if (comment) {
      g_hash_table_insert(osr->properties,
			  g_strdup(OPENSLIDE_PROPERTY_NAME_COMMENT),
			  comment);
    }

    fseeko(f, 0, SEEK_END);
    jp->end_in_file = ftello(f);
    if (jp->end_in_file == -1) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read file size for JPEG %d", i);
      fclose(f);
      goto DONE;
    }

    // file is done now
    fclose(f);

    int32_t num_tiles_across = jp->w / jp->tw;
    int32_t num_tiles_down = jp->h / jp->th;

    // because map file is last, ensure that all tw and th are the
    // same for 0 through num_jpegs-2
    //    g_debug("tile size: %d %d", tw, th);
    if (i == 0) {
      jpeg0_tw = jp->tw;
      jpeg0_th = jp->th;
      jpeg0_ta = num_tiles_across;
      jpeg0_td = num_tiles_down;
    } else if (i != (num_jpegs - 1)) {
      // not map file (still within level 0)
      g_assert(jpeg0_tw != 0 && jpeg0_th != 0);
      if (jpeg0_tw != jp->tw || jpeg0_th != jp->th) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Tile size not consistent");
        goto DONE;
      }
    }

    // use the optimisation file, if present
    int32_t mcu_starts_count = (jp->w / jp->tw) * (jp->h / jp->th); // number of tiles
    int64_t *mcu_starts = NULL;
    if (optimisation_file) {
      mcu_starts = extract_one_optimisation(optimisation_file,
					    num_tiles_down,
					    num_tiles_across,
					    mcu_starts_count);
    }
    if (mcu_starts) {
      jp->mcu_starts = mcu_starts;
    } else if (optimisation_file != NULL) {
      // the optimisation file is useless, ignore it
      optimisation_file = NULL;
    }

    // accumulate into some of the fields of the levels
    int32_t level;
    if (i != num_jpegs - 1) {
      // base (level 0)
      level = 0;
    } else {
      // map (level 1)
      level = 1;
    }

    struct _openslide_jpeg_level *l = levels[level];
    int32_t file_x = 0;
    int32_t file_y = 0;
    if (level == 0) {
      file_x = i % num_jpeg_cols;
      file_y = i / num_jpeg_cols;
    }
    if (file_y == 0) {
      l->level_w += jp->w;
      l->tiles_across += num_tiles_across;
    }
    if (file_x == 0) {
      l->level_h += jp->h;
      l->tiles_down += num_tiles_down;
    }

    // set some values (don't accumulate)
    l->raw_tile_width = jp->tw;
    l->raw_tile_height = jp->th;
    l->tile_advance_x = jp->tw;   // no overlaps or funny business
    l->tile_advance_y = jp->th;
  }

  // at this point, jpeg0_ta and jpeg0_td are set to values from 0,0 in level 0

  for (int i = 0; i < num_jpegs; i++) {
    struct _openslide_jpeg_file *jp = jpegs[i];

    int32_t level;
    int32_t file_x;
    int32_t file_y;
    if (i != num_jpegs - 1) {
      // base (level 0)
      level = 0;
      file_x = i % num_jpeg_cols;
      file_y = i / num_jpeg_cols;
    } else {
      // map (level 1)
      level = 1;
      file_x = 0;
      file_y = 0;
    }

    //g_debug("processing file %d %d %d", file_x, file_y, level);

    int32_t num_tiles_across = jp->w / jp->tw;

    struct _openslide_jpeg_level *l = levels[level];

    int32_t tile_count = (jp->w / jp->tw) * (jp->h / jp->th); // number of tiles

    // add all the tiles
    for (int local_tileno = 0; local_tileno < tile_count; local_tileno++) {
      struct _openslide_jpeg_tile *t = g_slice_new0(struct _openslide_jpeg_tile);

      int32_t local_tile_x = local_tileno % num_tiles_across;
      int32_t local_tile_y = local_tileno / num_tiles_across;

      t->fileno = i;
      t->tileno = local_tileno;
      t->w = jp->tw;
      t->h = jp->th;
      // no dest or src offsets

      // compute key for hashtable (y * w + x)
      int64_t x = file_x * jpeg0_ta + local_tile_x;
      int64_t y = file_y * jpeg0_td + local_tile_y;

      int64_t *key = g_slice_new(int64_t);
      *key = (y * l->tiles_across) + x;

      //g_debug("inserting tile: fileno %d tileno %d, %gx%g, file: %d %d, local: %d %d, global: %" G_GINT64_FORMAT " %" G_GINT64_FORMAT ", l->tiles_across: %d, key: %" G_GINT64_FORMAT, t->fileno, t->tileno, t->w, t->h, file_x, file_y, local_tile_x, local_tile_y, x, y, l->tiles_across, *key);
      g_assert(!g_hash_table_lookup(l->tiles, key));
      g_hash_table_insert(l->tiles, key, t);
    }
  }

  success = true;

 DONE:
  if (success) {
    _openslide_add_jpeg_ops(osr, num_jpegs, jpegs, level_count, levels);
  } else {
    // destroy
    for (int i = 0; i < num_jpegs; i++) {
      g_free(jpegs[i]->filename);
      g_free(jpegs[i]->mcu_starts);
      g_slice_free(struct _openslide_jpeg_file, jpegs[i]);
    }
    g_free(jpegs);

    for (int32_t i = 0; i < level_count; i++) {
      g_hash_table_unref(levels[i]->tiles);
      g_slice_free(struct _openslide_jpeg_level, levels[i]);
    }
    g_free(levels);
  }

  return success;
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

static bool hamamatsu_vmu_part2(openslide_t *osr,
				int num_files, char **image_filenames,
				GError **err) {
  bool success = false;

  // initialize individual ngr structs
  struct _openslide_ngr **files = g_new0(struct _openslide_ngr *,
					 num_files);
  for (int i = 0; i < num_files; i++) {
    files[i] = g_slice_new0(struct _openslide_ngr);
  }

  // open files
  for (int i = 0; i < num_files; i++) {
    struct _openslide_ngr *ngr = files[i];

    ngr->filename = g_strdup(image_filenames[i]);

    FILE *f;
    if ((f = _openslide_fopen(ngr->filename, "rb", err)) == NULL) {
      goto DONE;
    }

    // validate magic
    if ((fgetc(f) != 'G') || (fgetc(f) != 'N')) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Bad magic on NGR file");
      fclose(f);
      goto DONE;
    }

    // read w, h, column width, headersize
    fseeko(f, 4, SEEK_SET);
    ngr->w = read_le_int32_from_file(f);
    ngr->h = read_le_int32_from_file(f);
    ngr->column_width = read_le_int32_from_file(f);

    fseeko(f, 24, SEEK_SET);
    ngr->start_in_file = read_le_int32_from_file(f);

    // validate
    if ((ngr->w <= 0) || (ngr->h <= 0) ||
	(ngr->column_width <= 0) || (ngr->start_in_file <= 0)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Error processing header");
      fclose(f);
      goto DONE;
    }

    // ensure no remainder on columns
    if ((ngr->w % ngr->column_width) != 0) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Width not multiple of column width");
      fclose(f);
      goto DONE;
    }

    fclose(f);
  }

  success = true;

 DONE:
  if (success) {
    _openslide_add_ngr_ops(osr, num_files, files);
  } else {
    // destroy
    for (int i = 0; i < num_files; i++) {
      g_slice_free(struct _openslide_ngr, files[i]);
      g_free(files[i]->filename);
    }
    g_free(files);
  }

  return success;
}


bool _openslide_try_hamamatsu(openslide_t *osr, const char *filename,
			      struct _openslide_hash *quickhash1,
			      GError **err) {
  // initialize any variables destroyed/used in DONE
  bool success = false;

  char *dirname = g_path_get_dirname(filename);

  int num_images = 0;
  char **image_filenames = NULL;

  int num_cols = -1;
  int num_rows = -1;

  char **all_keys = NULL;

  int num_layers = -1;

  // first, see if it's a VMS/VMU file
  GKeyFile *key_file = g_key_file_new();
  if (!_openslide_read_key_file(key_file, filename, G_KEY_FILE_NONE, NULL)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Can't load key file");
    goto DONE;
  }

  // select group or fail, then read dimensions
  const char *groupname;
  if (g_key_file_has_group(key_file, GROUP_VMS)) {
    groupname = GROUP_VMS;

    num_cols = g_key_file_get_integer(key_file, groupname,
				      KEY_NUM_JPEG_COLS,
				      NULL);
    num_rows = g_key_file_get_integer(key_file,
				      groupname,
				      KEY_NUM_JPEG_ROWS,
				      NULL);
  } else if (g_key_file_has_group(key_file, GROUP_VMU)) {
    groupname = GROUP_VMU;

    num_cols = 1;  // not specified in file for VMU
    num_rows = 1;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not VMS or VMU file");
    goto DONE;
  }

  // validate cols/rows
  if (num_cols < 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "File has no columns");
    goto DONE;
  }
  if (num_rows < 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "File has no rows");
    goto DONE;
  }

  // init the image filenames
  // this format has cols*rows image files, plus the map
  num_images = (num_cols * num_rows) + 1;
  image_filenames = g_new0(char *, num_images);

  // hash in the key file
  if (!_openslide_hash_file(quickhash1, filename, err)) {
    goto DONE;
  }

  // make sure values are within known bounds
  num_layers = g_key_file_get_integer(key_file, groupname, KEY_NUM_LAYERS,
				      NULL);
  if (num_layers < 1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot handle Hamamatsu files with NoLayers < 1");
    goto DONE;
  }

  // add properties
  if (osr) {
    add_properties(osr->properties, key_file, groupname);
  }

  // extract MapFile
  char *tmp;
  tmp = g_key_file_get_string(key_file,
			      groupname,
			      KEY_MAP_FILE,
			      NULL);
  if (tmp) {
    char *map_filename = g_build_filename(dirname, tmp, NULL);
    g_free(tmp);

    image_filenames[num_images - 1] = map_filename;

    // hash in the map file
    if (!_openslide_hash_file(quickhash1, map_filename, err)) {
      goto DONE;
    }
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Can't read map file");
    goto DONE;
  }

  // now each ImageFile
  all_keys = g_key_file_get_keys(key_file, groupname, NULL, NULL);
  for (char **tmp = all_keys; *tmp != NULL; tmp++) {
    char *key = *tmp;
    char *value = g_key_file_get_string(key_file, groupname, key, NULL);

    //    g_debug("%s", key);

    if (strncmp(KEY_IMAGE_FILE, key, strlen(KEY_IMAGE_FILE)) == 0) {
      // starts with ImageFile
      char *suffix = key + strlen(KEY_IMAGE_FILE);

      int layer;
      int col;
      int row;

      char **split = g_strsplit(suffix, ",", 0);
      switch (g_strv_length(split)) {
      case 0:
	// all zero
	layer = 0;
	col = 0;
	row = 0;
	break;

      case 1:
	// (z)
	// first item, skip '('
	layer = g_ascii_strtoll(split[0] + 1, NULL, 10);
	col = 0;
	row = 0;
	break;

      case 2:
	// (x,y)
	layer = 0;
	// first item, skip '('
	col = g_ascii_strtoll(split[0] + 1, NULL, 10);
	row = g_ascii_strtoll(split[1], NULL, 10);
	break;

      case 3:
        // (z,x,y)
        // first item, skip '('
        layer = g_ascii_strtoll(split[0] + 1, NULL, 10);
        col = g_ascii_strtoll(split[1], NULL, 10);
        row = g_ascii_strtoll(split[2], NULL, 10);
        break;

      default:
        // we just don't know
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Unknown number of image dimensions: %d",
                    g_strv_length(split));
        g_free(value);
        g_strfreev(split);
        g_strfreev(all_keys);
        goto DONE;
      }
      g_strfreev(split);

      //g_debug("layer: %d, col: %d, row: %d", layer, col, row);

      if (layer != 0) {
        // skip non-zero layers for now
        g_free(value);
        continue;
      }

      if (col >= num_cols || row >= num_rows || col < 0 || row < 0) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Invalid row or column in Hamamatsu file (%d,%d)",
                    col, row);
        g_free(value);
	g_strfreev(all_keys);
        goto DONE;
      }

      // compute index from x,y
      int i = row * num_cols + col;

      // init the file
      if (image_filenames[i]) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                    "Duplicate image for (%d,%d)", col, row);
        g_free(value);
        g_strfreev(all_keys);
        goto DONE;
      }
      image_filenames[i] = g_build_filename(dirname, value, NULL);
    }
    g_free(value);
  }
  g_strfreev(all_keys);

  // ensure all image filenames are filled
  for (int i = 0; i < num_images; i++) {
    if (!image_filenames[i]) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read image filename %d", i);
      goto DONE;
    }
  }

  // add macro image
  tmp = g_key_file_get_string(key_file,
			      groupname,
			      KEY_MACRO_IMAGE,
			      NULL);
  if (tmp) {
    char *macro_filename = g_build_filename(dirname, tmp, NULL);
    bool result = _openslide_add_jpeg_associated_image(osr ? osr->associated_images : NULL,
                                                       "macro",
                                                       macro_filename, 0, err);
    g_free(macro_filename);
    g_free(tmp);

    if (!result) {
      g_prefix_error(err, "Could not read macro image: ");
      goto DONE;
    }
  }

  // finalize depending on what format
  if (groupname == GROUP_VMS) {
    // open OptimisationFile
    FILE *optimisation_file = NULL;
    char *tmp = g_key_file_get_string(key_file,
				      GROUP_VMS,
				      KEY_OPTIMISATION_FILE,
				      NULL);
    if (tmp) {
      char *optimisation_filename = g_build_filename(dirname, tmp, NULL);
      g_free(tmp);

      optimisation_file = _openslide_fopen(optimisation_filename, "rb", NULL);

      if (optimisation_file == NULL) {
	// g_debug("Can't open optimisation file");
      }
      g_free(optimisation_filename);
    } else {
      // g_debug("Optimisation file key not present");
    }

    // do all the jpeg stuff
    success = hamamatsu_vms_part2(osr,
				  num_images, image_filenames,
				  num_cols,
				  optimisation_file,
				  err);

    // clean up
    if (optimisation_file) {
      fclose(optimisation_file);
    }
  } else if (groupname == GROUP_VMU) {
    // verify a few assumptions for VMU
    int bits_per_pixel = g_key_file_get_integer(key_file,
						GROUP_VMU,
						KEY_BITS_PER_PIXEL,
						NULL);
    char *pixel_order = g_key_file_get_string(key_file,
					      GROUP_VMU,
					      KEY_PIXEL_ORDER,
					      NULL);

    if (bits_per_pixel != 36) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "%s must be 36", KEY_BITS_PER_PIXEL);
    } else if (!pixel_order || (strcmp(pixel_order, "RGB") != 0)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "%s must be RGB", KEY_PIXEL_ORDER);
    } else {
      // assumptions verified
      success = hamamatsu_vmu_part2(osr,
				    num_images, image_filenames,
				    err);
    }
    g_free(pixel_order);
  } else {
    g_assert_not_reached();
  }

 DONE:
  g_free(dirname);

  if (image_filenames) {
    for (int i = 0; i < num_images; i++) {
      g_free(image_filenames[i]);
    }
    g_free(image_filenames);
  }
  g_key_file_free(key_file);

  return success;
}

bool _openslide_try_hamamatsu_ndpi(openslide_t *osr, const char *filename,
				   struct _openslide_hash *quickhash1,
				   GError **err) {
  FILE *f = _openslide_fopen(filename, "rb", NULL);
  if (!f) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Can't open file");
    return false;
  }

  GSList *dump = _openslide_tiffdump_create(f, err);
  if (!dump) {
    fclose(f);
    return false;
  }
  _openslide_tiffdump_print(dump);
  _openslide_tiffdump_destroy(dump);
  fclose(f);

  /* XXX function is incomplete */
  (void) osr;
  (void) quickhash1;

  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
              "NDPI not supported");
  return false;
}
