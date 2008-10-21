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

  printf("w: %d, h: %d, restart_interval: %d\n"
	 "mcus_per_row: %d, mcu_rows_in_scan: %d\n"
	 "leftover mcus: %d\n",
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
  char *map_filename = NULL;
  char *image_filename = NULL;
  bool success = false;

  // this format has 2 jpeg files
  FILE *jpegs[2] = { NULL, NULL };

  // first, see if it's a VMS file
  GKeyFile *vms_file = g_key_file_new();
  if (!g_key_file_load_from_file(vms_file, filename, G_KEY_FILE_NONE, NULL)) {
    goto FAIL;
  }

  printf("vms file exists\n");

  if (!g_key_file_has_group(vms_file, GROUP_VMS)) {
    goto FAIL;
  }

  printf("vms file has group\n");

  char **all_keys = g_key_file_get_keys(vms_file, GROUP_VMS, NULL, NULL);
  char **tmp2;
  for (tmp2 = all_keys; *tmp2 != NULL; tmp2++) {
    printf(" %s: %s\n", *tmp2, g_key_file_get_string(vms_file, GROUP_VMS, *tmp2, NULL));
  }
  g_strfreev(all_keys);

  // extract relevant info
  char *tmp;
  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_MAP_FILE,
			      NULL);
  if (tmp) {
    map_filename = g_build_filename(dirname, tmp, NULL);
    g_free(tmp);
  }

  tmp = g_key_file_get_string(vms_file,
			      GROUP_VMS,
			      KEY_IMAGE_FILE,
			      NULL);
  if (tmp) {
    image_filename = g_build_filename(dirname, tmp, NULL);
    g_free(tmp);
  }

  printf("map: %s, image: %s\n",
	 map_filename, image_filename);

  // check image filename (the others are sort of optional)
  if (!image_filename || !map_filename) {
    goto FAIL;
  }

  // make sure values are within known bounds
  int num_layers = g_key_file_get_integer(vms_file,
					  GROUP_VMS,
					  KEY_NUM_LAYERS,
					  NULL);
  if (num_layers != 1) {
    goto FAIL;
  }

  int num_jpeg_cols = g_key_file_get_integer(vms_file,
					  GROUP_VMS,
					  KEY_NUM_JPEG_COLS,
					  NULL);
  if (num_jpeg_cols != 1) {  // TODO
    goto FAIL;
  }

  int num_jpeg_rows = g_key_file_get_integer(vms_file,
					  GROUP_VMS,
					  KEY_NUM_JPEG_ROWS,
					  NULL);
  if (num_jpeg_rows != 1) {  // TODO
    goto FAIL;
  }

  // verify jpegs

  // image 0
  if ((jpegs[0] = fopen(image_filename, "rb")) == NULL) {
    goto FAIL;
  }
  if (!verify_jpeg(jpegs[0])) {
    goto FAIL;
  }

  // image 1
  if ((jpegs[1] = fopen(map_filename, "rb")) == NULL) {
    goto FAIL;
  }
  if (!verify_jpeg(jpegs[1])) {
    goto FAIL;
  }

  _ws_add_jpeg_ops(wsd, 2, jpegs);
  success = true;
  goto DONE;

 FAIL:
  if (jpegs[0]) {
    fclose(jpegs[0]);
  }
  if (jpegs[1]) {
    fclose(jpegs[1]);
  }
  success = false;

 DONE:
  g_free(dirname);
  g_free(image_filename);
  g_free(map_filename);
  g_key_file_free(vms_file);

  return success;
}
