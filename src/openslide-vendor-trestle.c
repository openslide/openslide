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
 * Trestle (tif) support
 *
 * quickhash comes from what the TIFF backend does
 *
 */

#include <config.h>

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <errno.h>

static const char TRESTLE_SOFTWARE[] = "MedScan";
static const char OVERLAPS_XY[] = "OverlapsXY=";
static const char BACKGROUND_COLOR[] = "Background Color=";

static void add_properties(GHashTable *ht, char **tags) {
  g_hash_table_insert(ht,
		      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
		      g_strdup("trestle"));

  for (char **tag = tags; *tag != NULL; tag++) {
    char **pair = g_strsplit(*tag, "=", 2);
    if (pair) {
      char *name = g_strstrip(pair[0]);
      if (name) {
	char *value = g_strstrip(pair[1]);

	g_hash_table_insert(ht,
			    g_strdup_printf("trestle.%s", name),
			    g_strdup(value));
      }
    }
    g_strfreev(pair);
  }

  _openslide_duplicate_int_prop(ht, "trestle.Objective Power",
                                OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
}

static void parse_trestle_image_description(openslide_t *osr,
					    const char *description,
					    int32_t *overlap_count_OUT,
					    int32_t **overlaps_OUT) {
  char **first_pass = g_strsplit(description, ";", -1);

  int32_t overlap_count = 0;
  int32_t *overlaps = NULL;

  if (osr) {
    add_properties(osr->properties, first_pass);
  }

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
    } else if (g_str_has_prefix(*cur_str, BACKGROUND_COLOR)) {
      // found background color
      errno = 0;
      uint64_t bg = g_ascii_strtoull((*cur_str) + strlen(BACKGROUND_COLOR), NULL, 16);
      if (bg || !errno) {
	if (osr) {
          _openslide_set_background_color_prop(osr->properties,
                                               (bg >> 16) & 0xFF,
                                               (bg >> 8) & 0xFF,
                                               bg & 0xFF);
	}
      }
    }
  }
  g_strfreev(first_pass);

  *overlap_count_OUT = overlap_count;
  *overlaps_OUT = overlaps;
}

static char *get_associated_path(TIFF *tiff, const char *extension) {
  char *base_path = g_strdup(TIFFFileName(tiff));

  // strip file extension, if present
  char *dot = g_strrstr(base_path, ".");
  if (dot != NULL) {
    *dot = 0;
  }

  char *path = g_strdup_printf("%s%s", base_path, extension);
  g_free(base_path);
  return path;
}

static void add_associated_jpeg(openslide_t *osr, TIFF *tiff,
                                const char *extension,
                                const char *name) {
  char *path = get_associated_path(tiff, extension);
  _openslide_add_jpeg_associated_image(osr->associated_images,
                                       name, path, 0, NULL);
  g_free(path);
}

bool _openslide_try_trestle(openslide_t *osr, TIFF *tiff,
			    struct _openslide_hash *quickhash1,
			    GError **err) {
  int32_t overlap_count = 0;
  int32_t *overlaps = NULL;
  int32_t level_count = 0;
  int32_t *levels = NULL;

  if (!TIFFIsTiled(tiff)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "TIFF is not tiled");
    goto FAIL;
  }

  char *tagval;
  int tiff_result;
  tiff_result = TIFFGetField(tiff, TIFFTAG_SOFTWARE, &tagval);
  if (!tiff_result ||
      (strncmp(TRESTLE_SOFTWARE, tagval, strlen(TRESTLE_SOFTWARE)) != 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a Trestle slide");
    goto FAIL;
  }

  // parse
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (!tiff_result) {
    // no description, not trestle
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not a Trestle slide");
    goto FAIL;
  }
  parse_trestle_image_description(osr, tagval, &overlap_count, &overlaps);

  // count and validate levels
  do {
    if (!TIFFIsTiled(tiff)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                  "TIFF level is not tiled");
      goto FAIL;
    }

    // verify that we can read this compression (hard fail if not)
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read compression scheme");
      goto FAIL;
    };
    if (!TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Unsupported TIFF compression: %u", compression);
      goto FAIL;
    }

    // level ok
    level_count++;
  } while (TIFFReadDirectory(tiff));
  levels = g_new(int32_t, level_count);

  // directories are linear
  for (int32_t i = 0; i < level_count; i++) {
    levels[i] = i;
  }

  // add associated images
  if (osr) {
    add_associated_jpeg(osr, tiff, ".Full", "macro");
  }

  // all set, load up the TIFF-specific ops
  _openslide_add_tiff_ops(osr, tiff, 0,
			  overlap_count / 2, overlaps,
			  level_count, levels,
			  _openslide_tiff_read_tile,
			  quickhash1);

  // copy the TIFF resolution props to the standard MPP properties
  // this is a totally non-standard use of these TIFF tags
  if (osr) {
    _openslide_duplicate_double_prop(osr->properties, "tiff.XResolution",
                                     OPENSLIDE_PROPERTY_NAME_MPP_X);
    _openslide_duplicate_double_prop(osr->properties, "tiff.YResolution",
                                     OPENSLIDE_PROPERTY_NAME_MPP_Y);
  }

  return true;

 FAIL:
  g_free(levels);
  g_free(overlaps);
  return false;
}
