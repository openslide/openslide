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

#include "wholeslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include <openjpeg.h>

bool _ws_try_generic_jp2k(wholeslide_t *wsd, const char *filename) {
  FILE *f = fopen(filename, "r");
  if (f == NULL) {
    return false;
  }

  // try to read the image header
  OPJ_INT32 tx0;
  OPJ_INT32 ty0;
  OPJ_UINT32 tw;
  OPJ_UINT32 th;
  OPJ_UINT32 ntx;
  OPJ_UINT32 nty;
  opj_stream_t *stream = opj_stream_create_default_file_stream(f, true);
  opj_codec_t *codec = opj_create_decompress(CODEC_JP2);
  opj_image_t *image;
  opj_read_header(codec, &image,
		  &tx0, &ty0, &tw, &th, &ntx, &nty, stream);
  if (image == NULL) {
    // can't read
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    fclose(f);

    return false;
  }

  printf("%d %d %d %d %d %d %d %d\n", tx0, ty0, tw, th, ntx, nty, image->numcomps, image->color_space);
  for (int i = 0; i < image->numcomps; i++) {
    opj_image_comp_t *comp = (image->comps) + i;
    printf(" %d: %d %d %d %d %d %d\n", i, comp->dx, comp->dy,
	   comp->w, comp->h, comp->resno_decoded, comp->factor);
  }

  _ws_add_jp2k_ops(wsd, f,
		   image->comps->dx * image->comps->w,
		   image->comps->dx * image->comps->h);
  opj_image_destroy(image);
  opj_destroy_codec(codec);
  opj_stream_destroy(stream);

  return true;
}
