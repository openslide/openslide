/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#include "config.h"

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <jpeglib.h>

#include "openslide-private.h"

static const char GROUP_VMS[] = "Virtual Microscope Specimen";
static const char KEY_MAP_FILE[] = "MapFile";
static const char KEY_IMAGE_FILE[] = "ImageFile";
static const char KEY_NUM_LAYERS[] = "NoLayers";
static const char KEY_NUM_JPEG_COLS[] = "NoJpegColumns";
static const char KEY_NUM_JPEG_ROWS[] = "NoJpegRows";
static const char KEY_OPTIMISATION_FILE[] = "OptimisationFile";

#define INPUT_BUF_SIZE  4096

// returns w and h and tw and th as a convenience
static bool verify_jpeg(FILE *f, int32_t *w, int32_t *h,
			int32_t *tw, int32_t *th) {
  struct jpeg_decompress_struct cinfo;
  struct _openslide_jpeg_error_mgr jerr;
  jmp_buf env;

  *w = 0;
  *h = 0;
  *tw = 0;
  *th = 0;

  if (setjmp(env) == 0) {
    cinfo.err = _openslide_jpeg_set_error_handler(&jerr, &env);
    jpeg_create_decompress(&cinfo);

    int header_result;

    jpeg_stdio_src(&cinfo, f);
    header_result = jpeg_read_header(&cinfo, TRUE);
    if ((header_result != JPEG_HEADER_OK
	 && header_result != JPEG_HEADER_TABLES_ONLY)
	|| cinfo.num_components != 3 || cinfo.restart_interval == 0) {
      jpeg_destroy_decompress(&cinfo);
      return false;
    }

    jpeg_start_decompress(&cinfo);

    *w = cinfo.output_width;
    *h = cinfo.output_height;

    if (cinfo.restart_interval > cinfo.MCUs_per_row) {
      g_warning("Restart interval greater than MCUs per row");
      jpeg_destroy_decompress(&cinfo);
      return false;
    }

    *tw = *w / (cinfo.MCUs_per_row / cinfo.restart_interval);
    *th = *h / cinfo.MCU_rows_in_scan;
    int leftover_mcus = cinfo.MCUs_per_row % cinfo.restart_interval;
    if (leftover_mcus != 0) {
      jpeg_destroy_decompress(&cinfo);
      return false;
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
    return false;
  }

  jpeg_destroy_decompress(&cinfo);
  return true;
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


bool _openslide_try_hamamatsu(openslide_t *osr, const char *filename) {
  char *dirname = g_path_get_dirname(filename);
  char **image_filenames = NULL;
  struct _openslide_jpeg_fragment **jpegs = NULL;
  int num_jpegs = 0;

  char *optimisation_filename = NULL;
  FILE *optimisation_file = NULL;

  char **all_keys = NULL;

  bool success = false;
  char *tmp;

  // first, see if it's a VMS file
  GKeyFile *vms_file = g_key_file_new();
  if (!g_key_file_load_from_file(vms_file, filename, G_KEY_FILE_NONE, NULL)) {
    //    g_debug("Can't load VMS file");
    goto FAIL;
  }
  if (!g_key_file_has_group(vms_file, GROUP_VMS)) {
    g_warning("Can't find VMS group");
    goto FAIL;
  }


  // make sure values are within known bounds
  int num_layers = g_key_file_get_integer(vms_file, GROUP_VMS, KEY_NUM_LAYERS,
					  NULL);
  if (num_layers != 1) {
    g_warning("Cannot handle VMS files with NoLayers != 1");
    goto FAIL;
  }

  int num_jpeg_cols = g_key_file_get_integer(vms_file, GROUP_VMS,
					     KEY_NUM_JPEG_COLS,
					     NULL);
  if (num_jpeg_cols < 1) {
    goto FAIL;
  }

  int num_jpeg_rows = g_key_file_get_integer(vms_file,
					  GROUP_VMS,
					  KEY_NUM_JPEG_ROWS,
					  NULL);
  if (num_jpeg_rows < 1) {
    goto FAIL;
  }

  // this format has cols*rows jpeg files, plus the map
  num_jpegs = (num_jpeg_cols * num_jpeg_rows) + 1;
  image_filenames = g_new0(char *, num_jpegs);
  jpegs = g_new0(struct _openslide_jpeg_fragment *, num_jpegs);

  //  g_debug("vms rows: %d, vms cols: %d, num_jpegs: %d", num_jpeg_rows, num_jpeg_cols, num_jpegs);

  // extract MapFile
  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_MAP_FILE,
			      NULL);
  if (tmp) {
    image_filenames[num_jpegs - 1] = g_build_filename(dirname, tmp, NULL);
    struct _openslide_jpeg_fragment *frag =
      g_slice_new0(struct _openslide_jpeg_fragment);
    frag->x = 0;
    frag->y = 0;
    frag->z = 1;  // map is smaller
    jpegs[num_jpegs - 1] = frag;

    g_free(tmp);
  }


  // extract OptimisationFile
  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_OPTIMISATION_FILE,
			      NULL);
  if (tmp) {
    optimisation_filename = g_build_filename(dirname, tmp, NULL);
    g_free(tmp);
  }

  // now each ImageFile
  all_keys = g_key_file_get_keys(vms_file, GROUP_VMS, NULL, NULL);
  char **tmp2;
  for (tmp2 = all_keys; *tmp2 != NULL; tmp2++) {
    char *key = *tmp2;
    char *value = g_key_file_get_string(vms_file, GROUP_VMS, key, NULL);

    //    g_debug("%s", key);

    if (strncmp(KEY_IMAGE_FILE, key, strlen(KEY_IMAGE_FILE)) == 0) {
      // starts with ImageFile
      char *suffix = key + strlen(KEY_IMAGE_FILE);

      int col;
      int row;

      if (suffix[0] != '\0') {
	// parse out the row and col
	char *endptr;

	// skip (
	suffix++;

	// col
	col = g_ascii_strtoll(suffix, &endptr, 10);
	//	g_debug("%d", col);

	// skip ,
	endptr++;

	// row
	row = g_ascii_strtoll(endptr, NULL, 10);
	//	g_debug("%d", row);
      } else {
	col = 0;
	row = 0;
      }

      //      g_debug("col: %d, row: %d", col, row);

      if (col >= num_jpeg_cols || row >= num_jpeg_rows) {
	g_warning("Invalid row or column in VMS file");
	goto FAIL;
      }

      // compute index from x,y
      int i = row * num_jpeg_cols + col;

      // init the fragment
      if (jpegs[i]) {
	g_warning("Ignoring duplicate image for (%d,%d)", col, row);
      } else {
	image_filenames[i] = g_build_filename(dirname, value, NULL);

	jpegs[i] = g_slice_new0(struct _openslide_jpeg_fragment);
	jpegs[i]->x = col;
	jpegs[i]->y = row;
	jpegs[i]->z = 0;
      }
    }
    g_free(value);
  }

  // check image filenames (the others are sort of optional)
  for (int i = 0; i < num_jpegs; i++) {
    if (!image_filenames[i]) {
      g_warning("Can't read image filename %d", i);
      goto FAIL;
    }
  }

  // open jpegs
  if (optimisation_filename != NULL) {
    optimisation_file = fopen(optimisation_filename, "rb");
  }

  if (optimisation_file == NULL) {
    g_warning("Can't use optimisation file");
  }

  int32_t img00_w = 0;
  int32_t img00_h = 0;
  int32_t w;
  int32_t h;
  int32_t tw;
  int32_t th;
  int32_t layer1_tw = 0;
  int32_t layer1_th = 0;
  for (int i = 0; i < num_jpegs; i++) {
    struct _openslide_jpeg_fragment *jp = jpegs[i];

    // these jpeg files always start at 0
    jp->start_in_file = 0;

    if ((jp->f = fopen(image_filenames[i], "rb")) == NULL) {
      g_warning("Can't open JPEG %d", i);
      goto FAIL;
    }
    if (!verify_jpeg(jp->f, &w, &h, &tw, &th)) {
      g_warning("Can't verify JPEG %d", i);
      goto FAIL;
    }

    fseeko(jp->f, 0, SEEK_END);
    jp->end_in_file = ftello(jp->f);
    if (jp->end_in_file == -1) {
      g_warning("Can't read file size for JPEG %d", i);
      goto FAIL;
    }

    // because map file is last, ensure that all tw and th are the
    // same for 0 through num_jpegs-2
    //    g_debug("tile size: %d %d", tw, th);
    if (i == 0) {
      layer1_tw = tw;
      layer1_th = th;
    } else if (i < num_jpegs - 1) {
      g_assert(layer1_tw != 0 && layer1_th != 0);
      if (layer1_tw != tw || layer1_th != th) {
	g_warning("Tile size not consistent");
	goto FAIL;
      }
    }

    // verify that all files except the right and bottom edges are
    // the same size as image 0,0
    g_assert(jp->z != 2);
    if (jp->z == 0) { // revisit if we support NoLayers != 1
      if (jp->x == 0 && jp->y == 0) {
	img00_w = w;
	img00_h = h;
      } else {
	if ((jp->x != num_jpeg_cols - 1) && (w != img00_w)) {
	  g_warning("Incorrect width at non-right edge");
	  goto FAIL;
	}
	if ((jp->y != num_jpeg_rows - 1) && (h != img00_h)) {
	  g_warning("Incorrect height at non-bottom edge");
	  goto FAIL;
	}
      }
    }

    // leverage the optimisation file, if present
    int32_t num_tiles_down = h / th;
    int32_t num_tiles_across = w / tw;
    int32_t mcu_starts_count = (w / tw) * (h / th); // number of tiles
    int64_t *mcu_starts = NULL;
    if (optimisation_file) {
      mcu_starts = extract_one_optimisation(optimisation_file,
					    num_tiles_down,
					    num_tiles_across,
					    mcu_starts_count);
    }
    if (mcu_starts) {
      jp->mcu_starts_count = mcu_starts_count;
      jp->mcu_starts = mcu_starts;
    } else if (optimisation_file != NULL) {
      // the optimisation file is useless, close it
      fclose(optimisation_file);
      optimisation_file = NULL;
    }
  }

  _openslide_add_jpeg_ops(osr, num_jpegs, jpegs);
  success = true;
  goto DONE;

 FAIL:
  if (jpegs) {
    for (int i = 0; i < num_jpegs; i++) {
      FILE *f = jpegs[i]->f;
      if (f) {
	fclose(f);
      }
    }
    g_free(jpegs);
  }

  success = false;

 DONE:
  if (all_keys) {
    g_strfreev(all_keys);
  }

  g_free(dirname);

  if (optimisation_filename) {
    g_free(optimisation_filename);
  }

  if (optimisation_file) {
    fclose(optimisation_file);
  }

  if (image_filenames) {
    for (int i = 0; i < num_jpegs; i++) {
      g_free(image_filenames[i]);
    }
    g_free(image_filenames);
  }
  g_key_file_free(vms_file);

  return success;
}
