/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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
 *
 * This file is derived from tiffdump.c:
 *
 * Copyright (c) 1988-1997 Sam Leffler
 * Copyright (c) 1991-1997 Silicon Graphics, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 *
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 *
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-tiffdump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>


#include <tiffio.h>


static int64_t read_uint16(FILE *f, uint16_t endian) {
  uint16_t result;
  if (fread(&result, sizeof result, 1, f) != 1) {
    return -1;
  }

  switch (endian) {
  case TIFF_BIGENDIAN:
    return GUINT16_FROM_BE(result);

  case TIFF_LITTLEENDIAN:
    return GUINT16_FROM_LE(result);

  default:
    g_return_val_if_reached(-1);
  }
}

static int64_t read_uint32(FILE *f, uint16_t endian) {
  uint32_t result;
  if (fread(&result, sizeof result, 1, f) != 1) {
    return -1;
  }

  switch (endian) {
  case TIFF_BIGENDIAN:
    return GUINT32_FROM_BE(result);

  case TIFF_LITTLEENDIAN:
    return GUINT32_FROM_LE(result);

  default:
    g_return_val_if_reached(-1);
  }
}

static void *read_tiff_tag_1(int64_t count, int64_t offset, uint16_t endian) {
  return NULL;
}

static void *read_tiff_tag_2(int64_t count, int64_t offset, uint16_t endian) {
  return NULL;
}

static void *read_tiff_tag_4(int64_t count, int64_t offset, uint16_t endian) {
  return NULL;
}

static void *read_tiff_tag_4r(int64_t count, int64_t offset, uint16_t endian) {
  return NULL;
}

static void *read_tiff_tag_8(int64_t count, int64_t offset, uint16_t endian) {
  return NULL;
}

static void tiffdump_data_destroy(gpointer data) {
  struct _openslide_tiffdump_item *td = data;

  g_free(td->value);
  g_slice_free(struct _openslide_tiffdump_item, td);
}

static GHashTable *ReadDirectory(FILE *f, int64_t *diroff,
				 GHashTable *loop_detector,
				 uint16_t endian) {
  int64_t off = *diroff;
  *diroff = 0;
  GHashTable *result = NULL;

  // loop detection
  if (g_hash_table_lookup_extended(loop_detector, &off, NULL, NULL)) {
    // loop
    // TODO ERROR 
    goto done;
  }
  int64_t *key = g_slice_new(int64_t);
  *key = off;
  g_hash_table_insert(loop_detector, key, NULL);

  // no loop, let's seek
  if (fseeko(f, off, SEEK_SET) != 0) {
    // TODO ERROR 
    goto done;
  }

  // read directory count
  int dircount = read_uint16(f, endian);
  if (dircount == -1) {
    // TODO ERROR 
    goto done;
  }

  result = g_hash_table_new_full(g_int_hash, g_int_equal,
				 g_free, tiffdump_data_destroy);

  // read all directory entries
  for (int i = 0; i < dircount; i++) {
    int32_t tag = read_uint16(f, endian);
    int32_t type = read_uint16(f, endian);
    int64_t count = read_uint32(f, endian);
    int64_t offset = read_uint32(f, endian);

    if ((tag == -1) || (type == -1) || (count = -1) || (offset = -1)) {
      // TODO ERROR 
      goto done;
    }

    struct _openslide_tiffdump_item *data =
      g_slice_new(struct _openslide_tiffdump_item);
    data->type = type;
    data->count = count;

    switch (type) {
    case TIFF_BYTE:
    case TIFF_ASCII:
    case TIFF_SBYTE:
    case TIFF_UNDEFINED:
      data->value = read_tiff_tag_1(count, offset, endian);
      break;

    case TIFF_SHORT:
    case TIFF_SSHORT:
      data->value = read_tiff_tag_2(count, offset, endian);
      break;

    case TIFF_LONG:
    case TIFF_SLONG:
    case TIFF_FLOAT:
    case TIFF_IFD:
      data->value = read_tiff_tag_4(count, offset, endian);

    case TIFF_RATIONAL:
    case TIFF_SRATIONAL:
      data->value = read_tiff_tag_4r(count, offset, endian);

    case TIFF_DOUBLE:
      data->value = read_tiff_tag_8(count, offset, endian);

    default:
      //TODO ERROR 
      goto done;
    }

    int *key = g_new(int, 1);
    *key = tag;
    g_hash_table_insert(result, key, data);
  }

  // read the next dir offset
  int64_t nextdiroff = read_uint32(f, endian);
  if (nextdiroff == -1) {
    // TODO ERROR 
    goto done;
  }
  *diroff = nextdiroff;

done:
  return result;
}

/* returns list of hashtables of (int -> struct _openslide_tiffdump_item) */
GSList *_openslide_tiffdump(FILE *f) {
  GSList *result = NULL;

  // read and check magic
  uint16_t magic;
  fseeko(f, 0, SEEK_SET);
  if (fread(&magic, sizeof magic, 1, f) != 1) {
    // TODO ERROR 
  }
  if (magic != TIFF_BIGENDIAN && magic != TIFF_LITTLEENDIAN) {
    // TODO ERROR 
  }

  int32_t version = read_uint16(f, magic);
  int64_t diroff = read_uint32(f, magic);

  /*
   * Now check version (if needed, it's been byte-swapped).
   * Note that this isn't actually a version number, it's a
   * magic number that doesn't change (stupid).
   */
  if (version != TIFF_VERSION) {
    // TODO ERROR 
  }

  // initialize loop detector
  GHashTable *loop_detector = g_hash_table_new_full(_openslide_int64_hash,
						    _openslide_int64_equal,
						    _openslide_int64_free,
						    NULL);
  // read all the directories
  while (diroff != 0) {
    GHashTable *ht = ReadDirectory(f, &diroff, loop_detector, magic);
    result = g_slist_prepend(result, ht);
  }
  g_hash_table_unref(loop_detector);

  return g_slist_reverse(result);
}
