/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
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
 * Aperio (svs, tif) support
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

static const char APERIO_DESCRIPTION[] = "Aperio";

#define APERIO_COMPRESSION_JP2K_YCBCR 33003
#define APERIO_COMPRESSION_JP2K_RGB   33005

static void aperio_tiff_tilereader(openslide_t *osr,
				   struct _openslide_tiff_level *tiffl,
				   TIFF *tiff,
				   uint32_t *dest,
				   int64_t tile_col, int64_t tile_row) {
  GError *tmp_err = NULL;

  // which compression?
  uint16_t compression_mode;
  TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression_mode);

  // select color space
  enum _openslide_jp2k_colorspace space;
  switch (compression_mode) {
  case APERIO_COMPRESSION_JP2K_YCBCR:
    space = OPENSLIDE_JP2K_YCBCR;
    break;
  case APERIO_COMPRESSION_JP2K_RGB:
    space = OPENSLIDE_JP2K_RGB;
    break;
  default:
    // not for us? fallback
    _openslide_tiff_read_tile(osr, tiffl, tiff, dest, tile_col, tile_row);
    return;
  }

  // get tile dimensions
  int32_t tw, th;
  if (!TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tw)) {
    _openslide_set_error(osr, "Cannot get tile width");
    return;
  }
  if (!TIFFGetField(tiff, TIFFTAG_TILELENGTH, &th)) {
    _openslide_set_error(osr, "Cannot get tile height");
    return;
  }

  // read raw tile
  void *buf;
  int32_t buflen;
  _openslide_tiff_read_tile_data(osr, tiff,
                                 &buf, &buflen,
                                 tile_col, tile_row);
  if (!buflen) {
    if (!openslide_get_error(osr)) {
      // a slide with zero-length tiles has been seen in the wild
      // fill with transparent
      memset(dest, 0, tw * th * 4);
    }
    return;  // ok, haven't allocated anything yet
  }

  // decompress
  if (!_openslide_jp2k_decode_buffer(dest,
                                     tw, th,
                                     buf, buflen,
                                     space,
                                     &tmp_err)) {
    _openslide_set_error_from_gerror(osr, tmp_err);
    g_clear_error(&tmp_err);
  }

  // clean up
  g_free(buf);
}

static void add_properties(GHashTable *ht, char **props) {
  if (*props == NULL) {
    return;
  }

  g_hash_table_insert(ht,
		      g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
		      g_strdup("aperio"));

  // ignore first property in Aperio
  for(char **p = props + 1; *p != NULL; p++) {
    char **pair = g_strsplit(*p, "=", 2);

    if (pair) {
      char *name = g_strstrip(pair[0]);
      if (name) {
	char *value = g_strstrip(pair[1]);

	g_hash_table_insert(ht,
			    g_strdup_printf("aperio.%s", name),
			    g_strdup(value));
      }
    }
    g_strfreev(pair);
  }

  _openslide_duplicate_int_prop(ht, "aperio.AppMag",
                                OPENSLIDE_PROPERTY_NAME_OBJECTIVE_POWER);
  _openslide_duplicate_double_prop(ht, "aperio.MPP",
                                   OPENSLIDE_PROPERTY_NAME_MPP_X);
  _openslide_duplicate_double_prop(ht, "aperio.MPP",
                                   OPENSLIDE_PROPERTY_NAME_MPP_Y);
}

// add the image from the current TIFF directory
// returns false and sets GError if fatal error
// true does not necessarily imply an image was added
static bool add_associated_image(GHashTable *ht, const char *name_if_available,
				 TIFF *tiff, GError **err) {
  char *name = NULL;
  if (name_if_available) {
    name = g_strdup(name_if_available);
  } else {
    char *val;

    // get name
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &val)) {
      return true;
    }

    // parse ImageDescription, after newline up to first whitespace -> gives name
    char **lines = g_strsplit_set(val, "\r\n", -1);
    if (!lines) {
      return true;
    }

    if (lines[0] && lines[1]) {
      char *line = lines[1];

      char **names = g_strsplit(line, " ", -1);
      if (names && names[0]) {
	name = g_strdup(names[0]);
      }
      g_strfreev(names);
    }

    g_strfreev(lines);
  }

  if (!name) {
    return true;
  }

  bool result = _openslide_add_tiff_associated_image(ht, name, tiff, err);
  g_free(name);
  return result;
}


bool _openslide_try_aperio(openslide_t *osr, TIFF *tiff,
			   struct _openslide_hash *quickhash1,
			   GError **err) {
  int32_t level_count = 0;
  int32_t *levels = NULL;
  int32_t i = 0;

  if (!TIFFIsTiled(tiff)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "TIFF is not tiled");
    goto FAIL;
  }

  char *tagval;
  int tiff_result;
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (!tiff_result ||
      (strncmp(APERIO_DESCRIPTION, tagval, strlen(APERIO_DESCRIPTION)) != 0)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "Not an Aperio slide");
    goto FAIL;
  }

  /*
   * http://www.aperio.com/documents/api/Aperio_Digital_Slides_and_Third-party_data_interchange.pdf
   * page 14:
   *
   * The first image in an SVS file is always the baseline image (full
   * resolution). This image is always tiled, usually with a tile size
   * of 240 x 240 pixels. The second image is always a thumbnail,
   * typically with dimensions of about 1024 x 768 pixels. Unlike the
   * other slide images, the thumbnail image is always
   * stripped. Following the thumbnail there may be one or more
   * intermediate "pyramid" images. These are always compressed with
   * the same type of compression as the baseline image, and have a
   * tiled organization with the same tile size.
   *
   * Optionally at the end of an SVS file there may be a slide label
   * image, which is a low resolution picture taken of the slide’s
   * label, and/or a macro camera image, which is a low resolution
   * picture taken of the entire slide. The label and macro images are
   * always stripped.
   */

  // for aperio, the tiled directories are the ones we want
  do {
    if (TIFFIsTiled(tiff)) {
      level_count++;
    }
  } while (TIFFReadDirectory(tiff));
  levels = g_new(int32_t, level_count);

  TIFFSetDirectory(tiff, 0);
  i = 0;
  do {
    tdir_t dir = TIFFCurrentDirectory(tiff);
    if (TIFFIsTiled(tiff)) {
      levels[i++] = dir;
      //g_debug("tiled directory: %d", dir);
    } else {
      // associated image
      const char *name = (dir == 1) ? "thumbnail" : NULL;
      if (!add_associated_image(osr ? osr->associated_images : NULL,
                                name, tiff, err)) {
	g_prefix_error(err, "Can't read associated image: ");
	goto FAIL;
      }
      //g_debug("associated image: %d", dir);
    }

    // check depth
    uint32_t depth;
    tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDEPTH, &depth);
    if (tiff_result && depth != 1) {
      // we can't handle depth != 1
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Cannot handle ImageDepth=%d", depth);
      goto FAIL;
    }

    // check compression
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read compression scheme");
      goto FAIL;
    }
    if ((compression != APERIO_COMPRESSION_JP2K_YCBCR) &&
        (compression != APERIO_COMPRESSION_JP2K_RGB) &&
        !TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Unsupported TIFF compression: %u", compression);
      goto FAIL;
    }
  } while (TIFFReadDirectory(tiff));

  // read properties
  if (osr) {
    TIFFSetDirectory(tiff, 0);
    TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval); // XXX? should be safe, we just did it
    char **props = g_strsplit(tagval, "|", -1);
    add_properties(osr->properties, props);
    g_strfreev(props);
  }

  // special jpeg 2000 aperio thing (with fallback)
  _openslide_add_tiff_ops(osr, tiff, 0, level_count, levels,
			  aperio_tiff_tilereader,
			  quickhash1);
  return true;

 FAIL:
  g_free(levels);
  return false;
}
