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

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>

static const char TRESTLE_SOFTWARE[] = "MedScan";
static const char OVERLAPS_XY[] = "OverlapsXY=";

bool _openslide_try_trestle(openslide_t *osr, const char *filename) {
  char *tagval;

  // first, see if it's a TIFF
  TIFF *tiff = TIFFOpen(filename, "r");
  if (tiff == NULL) {
    return false; // not TIFF, not trestle
  }

  int tiff_result;
  tiff_result = TIFFGetField(tiff, TIFFTAG_SOFTWARE, &tagval);
  if (!tiff_result ||
      (strncmp(TRESTLE_SOFTWARE, tagval, strlen(TRESTLE_SOFTWARE)) != 0)) {
    // not trestle
    TIFFClose(tiff);
    return false;
  }

  // parse
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (!tiff_result) {
    // no description, not trestle
    TIFFClose(tiff);
    return false;
  }

  int32_t overlap_count = 0;
  int32_t *overlaps = NULL;

  char **first_pass = g_strsplit(tagval, ";", -1);
  for (char **cur_str = first_pass; *cur_str != NULL; cur_str++) {
    //g_debug(" XX: %s", *cur_str);
    if (g_str_has_prefix(*cur_str, OVERLAPS_XY)) {
      // found it
      char **second_pass = g_strsplit(*cur_str, " ", -1);

      overlap_count = g_strv_length(second_pass) - 1; // skip fieldname
      overlaps = g_new(int32_t, overlap_count);

      int i = 0;
      // skip fieldname
      for (char **cur_str2 = second_pass + 1; *cur_str2 != NULL; cur_str2++) {
	overlaps[i] = g_ascii_strtoull(*cur_str2, NULL, 10);
	i++;
      }

      g_strfreev(second_pass);
    }
  }

  // count layers
  int32_t layer_count = 0;
  int32_t *layers = NULL;
  do {
    layer_count++;
  } while (TIFFReadDirectory(tiff));
  layers = g_new(int32_t, layer_count);

  // directories are linear
  for (int32_t i = 0; i < layer_count; i++) {
    layers[i] = i;
  }

  // all set, load up the TIFF-specific ops
  _openslide_add_tiff_ops(osr, tiff, overlap_count, overlaps,
			  layer_count, layers,
			  _openslide_generic_tiff_tilereader_create,
			  _openslide_generic_tiff_tilereader_read,
			  _openslide_generic_tiff_tilereader_destroy);

  g_strfreev(first_pass);

  return true;
}
