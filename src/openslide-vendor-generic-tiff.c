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

struct layer {
  int32_t layer_number;
  int64_t width;
};

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct layer *la = a;
  const struct layer *lb = b;

  if (la->width > lb->width) {
    return -1;
  } else if (la->width == lb->width) {
    return 0;
  } else {
    return 1;
  }
}

bool _openslide_try_generic_tiff(openslide_t *osr, const char *filename) {
  // first, see if it's a TIFF
  TIFF *tiff = TIFFOpen(filename, "r");
  if (tiff == NULL) {
    return false; // not TIFF
  }

  if (!TIFFIsTiled(tiff)) {
    return false; // not tiled
  }

  if (osr) {
    g_hash_table_insert(osr->properties,
			g_strdup(_OPENSLIDE_VENDOR_NAME),
			g_strdup("generic-tiff"));
  }

  // accumulate tiled layers
  GList *layer_list = NULL;
  int current_layer = 0;
  int layer_count = 0;
  do {
    if (TIFFIsTiled(tiff)) {
      // get width
      uint32_t width;
      if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width)) {
	// oh no
	continue;
      }

      // confirm it is either the first image, or reduced-resolution
      if (current_layer != 0) {
	uint32_t subfiletype;
	if (!TIFFGetField(tiff, TIFFTAG_SUBFILETYPE, &subfiletype)) {
	  continue;
	}

	if (!(subfiletype & FILETYPE_REDUCEDIMAGE)) {
	  continue;
	}
      }

      // push into list
      struct layer *l = g_slice_new(struct layer);
      l->layer_number = current_layer;
      l->width = width;
      layer_list = g_list_prepend(layer_list, l);
      layer_count++;
    }
    current_layer++;
  } while (TIFFReadDirectory(tiff));

  // sort tiled layers
  layer_list = g_list_sort(layer_list, width_compare);

  // copy layers in, while deleting the list
  int32_t *layers = g_new(int32_t, layer_count);
  for (int i = 0; i < layer_count; i++) {
    struct layer *l = layer_list->data;
    layer_list = g_list_delete_link(layer_list, layer_list);

    layers[i] = l->layer_number;
    g_slice_free(struct layer, l);
  }

  g_assert(layer_list == NULL);

  // all set, load up the TIFF-specific ops
  _openslide_add_tiff_ops(osr, tiff,
			  0, NULL,
			  layer_count, layers,
			  _openslide_generic_tiff_tilereader);


  return true;
}
