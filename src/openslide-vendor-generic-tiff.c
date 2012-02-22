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
 * Generic TIFF support
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

struct layer {
  int32_t directory;
  int64_t width;
};

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct layer *la = (const struct layer *) a;
  const struct layer *lb = (const struct layer *) b;

  if (la->width > lb->width) {
    return -1;
  } else if (la->width == lb->width) {
    return 0;
  } else {
    return 1;
  }
}

bool _openslide_try_generic_tiff(openslide_t *osr, TIFF *tiff,
				 struct _openslide_hash *quickhash1) {
  GList *layer_list = NULL;
  int32_t layer_count = 0;
  int32_t *layers = NULL;

  if (!TIFFIsTiled(tiff)) {
    goto FAIL; // not tiled
  }

  if (osr) {
    g_hash_table_insert(osr->properties,
			g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
			g_strdup("generic-tiff"));
  }

  // accumulate tiled layers
  layer_count = 0;
  do {
    // confirm that this directory is tiled
    if (!TIFFIsTiled(tiff)) {
      continue;
    }

    // get width
    uint32_t width;
    if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width)) {
      // oh no
      continue;
    }

    // confirm it is either the first image, or reduced-resolution
    if (TIFFCurrentDirectory(tiff) != 0) {
      uint32_t subfiletype;
      if (!TIFFGetField(tiff, TIFFTAG_SUBFILETYPE, &subfiletype)) {
        continue;
      }

      if (!(subfiletype & FILETYPE_REDUCEDIMAGE)) {
        continue;
      }
    }

    // verify that we can read this compression (hard fail if not)
    uint16_t compression;
    if (!TIFFGetField(tiff, TIFFTAG_COMPRESSION, &compression)) {
      g_warning("Can't read compression scheme");
      goto FAIL;
    };
    if (!TIFFIsCODECConfigured(compression)) {
      g_warning("Unsupported TIFF compression: %u", compression);
      goto FAIL;
    }

    // push into list
    struct layer *l = g_slice_new(struct layer);
    l->directory = TIFFCurrentDirectory(tiff);
    l->width = width;
    layer_list = g_list_prepend(layer_list, l);
    layer_count++;
  } while (TIFFReadDirectory(tiff));

  // sort tiled layers
  layer_list = g_list_sort(layer_list, width_compare);

  // copy layers in, while deleting the list
  layers = g_new(int32_t, layer_count);
  for (int i = 0; i < layer_count; i++) {
    struct layer *l = (struct layer *)layer_list->data;
    layer_list = g_list_delete_link(layer_list, layer_list);

    layers[i] = l->directory;
    g_slice_free(struct layer, l);
  }

  g_assert(layer_list == NULL);

  // all set, load up the TIFF-specific ops
  _openslide_add_tiff_ops(osr, tiff,
			  0, NULL,
			  layer_count, layers,
			  _openslide_generic_tiff_tilereader,
			  quickhash1);


  return true;

 FAIL:
  // free the layer list
  for (GList *i = layer_list; i != NULL; i = g_list_delete_link(i, i)) {
    g_slice_free(struct layer, i->data);
  }

  return false;
}
