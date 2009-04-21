/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
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

static const char MRXS_EXT[] = ".mrxs";
static const char SLIDEDAT_INI[] = "Slidedat.ini";

static const char GROUP_GENERAL[] = "GENERAL";
static const char KEY_SLIDE_VERSION[] = "SLIDE_VERSION";
static const char KEY_SLIDE_ID[] = "SLIDE_ID";
static const char KEY_IMAGENUMBER_X[] = "IMAGENUMBER_X";
static const char KEY_IMAGENUMBER_Y[] = "IMAGENUMBER_Y";

static const char GROUP_HIERARCHICAL[] = "HIERARCHICAL";
static const char KEY_INDEXFILE[] = "INDEXFILE";
static const char KEY_HIER_0_COUNT[] = "HIER_0_COUNT";
static const char KEY_HIER_0_VAL_d_SECTION[] = "HIER_0_VAL_%d_SECTION";

static const char GROUP_DATAFILE[] = "DATAFILE";
static const char KEY_FILE_COUNT[] = "FILE_COUNT";
static const char KEY_d_FILE[] = "FILE_%d";

static const char KEY_OVERLAP_X[] = "OVERLAP_X";
static const char KEY_OVERLAP_Y[] = "OVERLAP_Y";
static const char KEY_IMAGE_FORMAT[] = "IMAGE_FORMAT";
static const char KEY_IMAGE_FILL_COLOR_BGR[] = "IMAGE_FILL_COLOR_BGR";
static const char KEY_DIGITIZER_WIDTH[] = "DIGITIZER_WIDTH";
static const char KEY_DIGITIZER_HEIGHT[] = "DIGITIZER_HEIGHT";

#define READ_KEY_OR_FAIL(TARGET, KEYFILE, GROUP, KEY, TYPE, FAIL_MSG) \
  TARGET = g_key_file_get_ ## TYPE(KEYFILE, GROUP, KEY, NULL);	      \
  if (!TARGET) { g_warning(FAIL_MSG); goto FAIL; }


bool _openslide_try_mirax(openslide_t *osr, const char *filename) {
  struct _openslide_jpeg_fragment **jpegs = NULL;
  int num_jpegs = 0;

  char *dirname = NULL;

  GKeyFile *slidedat = NULL;

  bool success = false;
  char *tmp = NULL;

  // info about this slide
  char *slide_version = NULL;
  char *slide_id = NULL;
  int tiles_x = 0;
  int tiles_y = 0;

  char *index_filename = NULL;
  int zoom_levels = 0;
  char **hier_0_section_names = NULL;

  int datafile_count = 0;
  char **datafile_names = NULL;

  // start reading

  // verify filename
  if (!g_str_has_suffix(filename, MRXS_EXT)) {
    goto FAIL;
  }

  // get directory from filename
  dirname = g_strndup(filename, strlen(filename) - strlen(MRXS_EXT));

  // first, check slidedat
  tmp = g_build_filename(dirname, SLIDEDAT_INI, NULL);
  slidedat = g_key_file_new();
  if (!g_key_file_load_from_file(slidedat, tmp, G_KEY_FILE_NONE, NULL)) {
    g_warning("Can't load Slidedat file");
    goto FAIL;
  }
  g_free(tmp);
  tmp = NULL;

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

  // load hierarchical stuff
  if (!g_key_file_has_group(slidedat, GROUP_HIERARCHICAL)) {
    g_warning("Can't find %s group", GROUP_HIERARCHICAL);
    goto FAIL;
  }

  READ_KEY_OR_FAIL(index_filename, slidedat, GROUP_HIERARCHICAL,
		   KEY_INDEXFILE, value, "Can't read index filename");
  READ_KEY_OR_FAIL(zoom_levels, slidedat, GROUP_HIERARCHICAL,
		   KEY_HIER_0_COUNT, integer, "Can't read zoom levels");

  hier_0_section_names = g_new0(char *, zoom_levels + 1);
  for (int i = 0; i < zoom_levels; i++) {
    tmp = g_strdup_printf(KEY_HIER_0_VAL_d_SECTION, i);

    READ_KEY_OR_FAIL(hier_0_section_names[i], slidedat, GROUP_HIERARCHICAL,
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

  datafile_names = g_new0(char *, datafile_count + 1);
  for (int i = 0; i < datafile_count; i++) {
    tmp = g_strdup_printf(KEY_d_FILE, i);

    READ_KEY_OR_FAIL(datafile_names[i], slidedat, GROUP_DATAFILE,
		     tmp, value, "Can't read datafile name");

    g_free(tmp);
    tmp = NULL;
  }

  // load data from all hier_0_section_names sections


  g_debug("dirname: %s", dirname);
  g_debug("slide_version: %s", slide_version);
  g_debug("slide_id: %s", slide_id);
  g_debug("tiles (%d,%d)", tiles_x, tiles_y);
  g_debug("index_filename: %s", index_filename);
  g_debug("zoom_levels: %d", zoom_levels);
  for (int i = 0; i < zoom_levels; i++) {
    g_debug(" section name %d: %s", i, hier_0_section_names[i]);
  }
  g_debug("datafile_count: %d", datafile_count);
  for (int i = 0; i < datafile_count; i++) {
    g_debug(" datafile name %d: %s", i, datafile_names[i]);
  }

  goto FAIL;



  _openslide_add_jpeg_ops(osr, num_jpegs, jpegs);
  success = true;
  goto DONE;

 FAIL:

  success = false;

 DONE:
  g_free(dirname);
  g_free(tmp);
  g_free(slide_version);
  g_free(slide_id);
  g_free(index_filename);
  g_strfreev(datafile_names);
  g_strfreev(hier_0_section_names);

  if (slidedat) {
    g_key_file_free(slidedat);
  }

  return success;
}
