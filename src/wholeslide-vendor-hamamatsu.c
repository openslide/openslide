/*
 *  Wholeslide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  Wholeslide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  Wholeslide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Wholeslide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking Wholeslide statically or dynamically with other modules is
 *  making a combined work based on Wholeslide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#include "config.h"

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <jpeglib.h>

#include "wholeslide-private.h"

static const char GROUP_VMS[] = "Virtual Microscope Specimen";
static const char KEY_MAP_FILE[] = "MapFile";
static const char KEY_IMAGE_FILE[] = "ImageFile";
static const char KEY_NUM_LAYERS[] = "NoLayers";
static const char KEY_NUM_JPEG_COLS[] = "NoJpegColumns";
static const char KEY_NUM_JPEG_ROWS[] = "NoJpegRows";

#define INPUT_BUF_SIZE  4096

static bool verify_jpeg(FILE *f) {
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;

  cinfo.err = jpeg_std_error(&jerr);
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

  unsigned int restart_interval = cinfo.restart_interval;
  JDIMENSION MCUs_per_row = cinfo.MCUs_per_row;
  JDIMENSION MCU_rows_in_scan = cinfo.MCU_rows_in_scan;

  unsigned int leftover_mcus = MCUs_per_row % restart_interval;

  g_debug("w: %d, h: %d, restart_interval: %d\n"
	 "mcus_per_row: %d, mcu_rows_in_scan: %d\n"
	 "leftover mcus: %d",
	 cinfo.output_width, cinfo.output_height,
	 restart_interval,
	 MCUs_per_row, MCU_rows_in_scan,
	 leftover_mcus);

  if (leftover_mcus != 0) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  jpeg_destroy_decompress(&cinfo);
  return true;
}


bool _ws_try_hamamatsu(wholeslide_t *wsd, const char *filename) {
  char *dirname = g_path_get_dirname(filename);
  char **image_filenames = NULL;
  struct _ws_jpeg_fragment **jpegs = NULL;
  int num_jpegs = 0;

  char **all_keys = NULL;

  bool success = false;
  char *tmp;

  // first, see if it's a VMS file
  GKeyFile *vms_file = g_key_file_new();
  if (!g_key_file_load_from_file(vms_file, filename, G_KEY_FILE_NONE, NULL)) {
    g_debug("Can't load VMS file");
    goto FAIL;
  }
  if (!g_key_file_has_group(vms_file, GROUP_VMS)) {
    g_debug("Can't find VMS group");
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
  jpegs = g_new0(struct _ws_jpeg_fragment *, num_jpegs);

  g_debug("vms rows: %d, vms cols: %d, num_jpegs: %d", num_jpeg_rows, num_jpeg_cols, num_jpegs);

  // extract MapFile
  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_MAP_FILE,
			      NULL);
  if (tmp) {
    image_filenames[num_jpegs - 1] = g_build_filename(dirname, tmp, NULL);
    struct _ws_jpeg_fragment *frag = g_slice_new0(struct _ws_jpeg_fragment);
    frag->x = 0;
    frag->y = 0;
    frag->z = 1;  // map is smaller
    jpegs[num_jpegs - 1] = frag;

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
	g_debug("%d", col);

	// skip ,
	endptr++;

	// row
	row = g_ascii_strtoll(endptr, NULL, 10);
	g_debug("%d", row);
      } else {
	col = 0;
	row = 0;
      }

      g_debug("col: %d, row: %d", col, row);

      if (col >= num_jpeg_cols || row >= num_jpeg_rows) {
	g_debug("Invalid row or column in VMS file");
	goto FAIL;
      }

      // compute index from x,y
      int i = row * num_jpeg_cols + col;

      // init the fragment
      if (jpegs[i]) {
	g_warning("Ignoring duplicate image for (%d,%d)", col, row);
      } else {
	image_filenames[i] = g_build_filename(dirname, value, NULL);

	jpegs[i] = g_slice_new0(struct _ws_jpeg_fragment);
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
      g_debug("Can't read image filename %d", i);
      goto FAIL;
    }
  }

  // open jpegs
  for (int i = 0; i < num_jpegs; i++) {
    if ((jpegs[i]->f = fopen(image_filenames[i], "rb")) == NULL) {
      g_debug("Can't open JPEG %d", i);
      goto FAIL;
    }
    if (!verify_jpeg(jpegs[i]->f)) {
      g_debug("Can't verify JPEG %d", i);
      goto FAIL;
    }
  }

  _ws_add_jpeg_ops(wsd, num_jpegs, jpegs);
  success = true;
  goto DONE;

 FAIL:
  if (jpegs) {
    for (int i = 0; i < num_jpegs; i++) {
      FILE *f = jpegs[i]->f;
      if (f) {
	fclose(f);
      }
      g_free(jpegs[i]);
    }
    g_free(jpegs);
  }

  success = false;

 DONE:
  if (all_keys) {
    g_strfreev(all_keys);
  }

  g_free(dirname);

  if (image_filenames) {
    for (int i = 0; i < num_jpegs; i++) {
      g_free(image_filenames[i]);
    }
    g_free(image_filenames);
  }
  g_key_file_free(vms_file);

  return success;
}
