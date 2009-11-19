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

#include "openslide-private.h"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <tiffio.h>
#include <errno.h>

bool _openslide_try_generic_tiff(openslide_t *osr, const char *filename) {
  // first, see if it's a TIFF
  TIFF *tiff = TIFFOpen(filename, "r");
  if (tiff == NULL) {
    return false; // not TIFF
  }

  if (osr) {
    g_hash_table_insert(osr->properties,
			g_strdup(_OPENSLIDE_VENDOR_NAME),
			g_strdup("generic-tiff"));
  }

  // count layers
  int32_t layer_count = 0;
  int32_t *layers = NULL;
  do {
    layer_count++;
  } while (TIFFReadDirectory(tiff));
  layers = g_new(int32_t, layer_count);

  // assume directories are linear
  for (int32_t i = 0; i < layer_count; i++) {
    layers[i] = i;
  }

  // all set, load up the TIFF-specific ops
  _openslide_add_tiff_ops(osr, tiff,
			  0, NULL,
			  layer_count, layers,
			  _openslide_generic_tiff_tilereader);


  return true;
}
