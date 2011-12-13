/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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

#include <openjpeg.h>

static const char APERIO_DESCRIPTION[] = "Aperio";

#define APERIO_COMPRESSION_JP2K_YCBCR 33003
#define APERIO_COMPRESSION_JP2K_RGB   33005

// TODO: replace with tables
static void write_pixel_ycbcr(uint32_t *dest, uint8_t c0, uint8_t c1, uint8_t c2) {
  double R = c0 + 1.402 * (c2 - 128);
  double G = c0 - 0.34414 * (c1 - 128) - 0.71414 * (c2 - 128);
  double B = c0 + 1.772 * (c1 - 128);

  if (R > 255) {
    R = 255;
  }
  if (R < 0) {
    R = 0;
  }
  if (G > 255) {
    G = 255;
  }
  if (G < 0) {
    G = 0;
  }
  if (B > 255) {
    B = 255;
  }
  if (B < 0) {
    B = 0;
  }

  *dest = 255 << 24 | ((uint8_t) R << 16) | ((uint8_t) G << 8) | ((uint8_t) B);
}

static void write_pixel_rgb(uint32_t *dest, uint8_t c0, uint8_t c1, uint8_t c2) {
  *dest = 255 << 24 | c0 << 16 | c1 << 8 | c2;
}

static void warning_callback(const char *msg, void *data G_GNUC_UNUSED) {
  g_warning("%s", msg);
}
static void error_callback(const char *msg, void *data) {
  openslide_t *osr = (openslide_t *) data;
  _openslide_set_error(osr, "%s", msg);
}

static void copy_aperio_tile(uint16_t compression_mode,
			     opj_image_comp_t *comps,
			     uint32_t *dest,
			     int32_t w, int32_t h,
			     int c0_sub_x, int c0_sub_y,
			     int c1_sub_x, int c1_sub_y,
			     int c2_sub_x, int c2_sub_y) {
  // TODO: too slow, and with duplicated code!

  int i = 0;

  switch (compression_mode) {
  case APERIO_COMPRESSION_JP2K_YCBCR:
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
	uint8_t c0 = comps[0].data[(y / c0_sub_y) * comps[0].w + (x / c0_sub_x)];
	uint8_t c1 = comps[1].data[(y / c1_sub_y) * comps[1].w + (x / c1_sub_x)];
	uint8_t c2 = comps[2].data[(y / c2_sub_y) * comps[2].w + (x / c2_sub_x)];

	write_pixel_ycbcr(dest + i, c0, c1, c2);
	i++;
      }
    }

    break;

  case APERIO_COMPRESSION_JP2K_RGB:
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
	uint8_t c0 = comps[0].data[(y / c0_sub_y) * comps[0].w + (x / c0_sub_x)];
	uint8_t c1 = comps[1].data[(y / c1_sub_y) * comps[1].w + (x / c1_sub_x)];
	uint8_t c2 = comps[2].data[(y / c2_sub_y) * comps[2].w + (x / c2_sub_x)];

	write_pixel_rgb(dest + i, c0, c1, c2);
	i++;
      }
    }
    break;
  }
}

static void aperio_tiff_tilereader(openslide_t *osr,
				   TIFF *tiff,
				   uint32_t *dest,
				   int64_t x, int64_t y,
				   int32_t w, int32_t h) {
  // which compression?
  uint16_t compression_mode;
  TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression_mode);

  // not for us? fallback
  if ((compression_mode != APERIO_COMPRESSION_JP2K_YCBCR) &&
      (compression_mode != APERIO_COMPRESSION_JP2K_RGB)) {
    _openslide_generic_tiff_tilereader(osr, tiff, dest, x, y, w, h);
    return;
  }

  // else, JPEG 2000!
  opj_cio_t *stream = NULL;
  opj_dinfo_t *dinfo = NULL;
  opj_image_t *image = NULL;
  opj_image_comp_t *comps = NULL;

  // note: don't use info_handler, it outputs lots of junk
  opj_event_mgr_t event_callbacks = { error_callback, warning_callback, NULL };

  // get tile number
  ttile_t tile_no = TIFFComputeTile(tiff, x, y, 0, 0);

  //  g_debug("aperio reading tile_no: %d", tile_no);

  // get tile size
  toff_t *sizes;
  if (TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes) == 0) {
    _openslide_set_error(osr, "Cannot get tile size");
    return;  // ok, haven't allocated anything yet
  }
  tsize_t tile_size = sizes[tile_no];

  // get raw tile
  tdata_t buf = g_slice_alloc(tile_size);
  tsize_t size = TIFFReadRawTile(tiff, tile_no, buf, tile_size);
  if (size == -1) {
    _openslide_set_error(osr, "Cannot get raw tile");
    goto DONE;
  }

  // init decompressor
  opj_dparameters_t parameters;
  dinfo = opj_create_decompress(CODEC_J2K);
  opj_set_default_decoder_parameters(&parameters);
  opj_setup_decoder(dinfo, &parameters);
  stream = opj_cio_open((opj_common_ptr) dinfo, (unsigned char *) buf, size);
  opj_set_event_mgr((opj_common_ptr) dinfo, &event_callbacks, osr);


  // decode
  image = opj_decode(dinfo, stream);

  // check error
  if (openslide_get_error(osr)) {
    goto DONE;
  }

  comps = image->comps;

  // sanity check
  if (image->numcomps != 3) {
    _openslide_set_error(osr, "image->numcomps != 3");
    goto DONE;
  }

  // TODO more checks?

  copy_aperio_tile(compression_mode, comps, dest,
		   w, h,
		   w / comps[0].w, h / comps[0].h,
		   w / comps[1].w, h / comps[1].h,
		   w / comps[2].w, h / comps[2].h);

 DONE:
  // erase
  g_slice_free1(tile_size, buf);
  if (image) opj_image_destroy(image);
  if (stream) opj_cio_close(stream);
  if (dinfo) opj_destroy_decompress(dinfo);
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
}

// add the image from the current TIFF directory
// returns false if fatal error
// true does not necessarily imply an image was added
static bool add_associated_image(GHashTable *ht, const char *name_if_available,
				 TIFF *tiff) {
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

  bool result = _openslide_add_tiff_associated_image(ht, name, tiff);
  g_free(name);
  return result;
}


bool _openslide_try_aperio(openslide_t *osr, TIFF *tiff,
			   struct _openslide_hash *quickhash1) {
  int32_t layer_count = 0;
  int32_t *layers = NULL;
  int32_t i = 0;

  if (!TIFFIsTiled(tiff)) {
    goto FAIL;
  }

  char *tagval;
  int tiff_result;
  tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &tagval);
  if (!tiff_result ||
      (strncmp(APERIO_DESCRIPTION, tagval, strlen(APERIO_DESCRIPTION)) != 0)) {
    // not aperio
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

  // for aperio, the tiled layers are the ones we want
  do {
    if (TIFFIsTiled(tiff)) {
      layer_count++;
    }
  } while (TIFFReadDirectory(tiff));
  layers = g_new(int32_t, layer_count);

  TIFFSetDirectory(tiff, 0);
  i = 0;
  do {
    if (TIFFIsTiled(tiff)) {
      layers[i++] = TIFFCurrentDirectory(tiff);
      //g_debug("tiled layer: %d", TIFFCurrentDirectory(tiff));
    } else {
      // associated image
      const char *name = (i == 1) ? "thumbnail" : NULL;
      if (!add_associated_image(osr ? osr->associated_images : NULL, name, tiff)) {
	g_warning("Can't read associated image");
	goto FAIL;
      }
      //g_debug("associated image: %d", TIFFCurrentDirectory(tiff));
    }

    // check depth
    uint32_t depth;
    tiff_result = TIFFGetField(tiff, TIFFTAG_IMAGEDEPTH, &depth);
    if (tiff_result && depth != 1) {
      // we can't handle depth != 1
      g_warning("Cannot handle ImageDepth=%d", depth);
      goto FAIL;
    }

    // check compression
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_warning("Can't read compression scheme");
      goto FAIL;
    }
    if ((compression != APERIO_COMPRESSION_JP2K_YCBCR) &&
        (compression != APERIO_COMPRESSION_JP2K_RGB) &&
        !TIFFIsCODECConfigured(compression)) {
      g_warning("Unsupported TIFF compression: %u", compression);
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
  _openslide_add_tiff_ops(osr, tiff, 0, NULL, layer_count, layers,
			  aperio_tiff_tilereader,
			  quickhash1);
  return true;

 FAIL:
  g_free(layers);
  return false;
}
