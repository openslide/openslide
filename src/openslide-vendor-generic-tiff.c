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

struct level {
  int32_t directory;
  int64_t width;
};

static int width_compare(gconstpointer a, gconstpointer b) {
  const struct level *la = a;
  const struct level *lb = b;

  if (la->width > lb->width) {
    return -1;
  } else if (la->width == lb->width) {
    return 0;
  } else {
    return 1;
  }
}

bool _openslide_try_generic_tiff(openslide_t *osr, TIFF *tiff,
				 struct _openslide_hash *quickhash1,
				 GError **err) {
  GList *level_list = NULL;
  int32_t level_count = 0;
  int32_t *levels = NULL;

  if (!TIFFIsTiled(tiff)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
                "TIFF is not tiled");
    goto FAIL;
  }

  if (osr) {
    g_hash_table_insert(osr->properties,
			g_strdup(OPENSLIDE_PROPERTY_NAME_VENDOR),
			g_strdup("generic-tiff"));
  }

  // accumulate tiled levels
  level_count = 0;
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
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read compression scheme");
      goto FAIL;
    };
    if (!TIFFIsCODECConfigured(compression)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Unsupported TIFF compression: %u", compression);
      goto FAIL;
    }

    // push into list
    struct level *l = g_slice_new(struct level);
    l->directory = TIFFCurrentDirectory(tiff);
    l->width = width;
    level_list = g_list_prepend(level_list, l);
    level_count++;
  } while (TIFFReadDirectory(tiff));

  // sort tiled levels
  level_list = g_list_sort(level_list, width_compare);

  // copy levels in, while deleting the list
  levels = g_new(int32_t, level_count);
  for (int i = 0; i < level_count; i++) {
    struct level *l = level_list->data;
    level_list = g_list_delete_link(level_list, level_list);

    levels[i] = l->directory;
    g_slice_free(struct level, l);
  }

  g_assert(level_list == NULL);

  // all set, load up the TIFF-specific ops
  _openslide_add_tiff_ops(osr, tiff, 0,
			  level_count, levels,
			  _openslide_tiff_read_tile,
			  quickhash1);


  return true;

 FAIL:
  // free the level list
  for (GList *i = level_list; i != NULL; i = g_list_delete_link(i, i)) {
    g_slice_free(struct level, i->data);
  }

  return false;
}
