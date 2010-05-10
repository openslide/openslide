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
 */

#include <config.h>

#include "openslide-private.h"

#include "openslide-hash.h"

#include <string.h>
#include <inttypes.h>

#ifdef HAVE_G_CHECKSUM_NEW
struct _openslide_hash {
  GChecksum *checksum;
};

struct _openslide_hash *_openslide_hash_quickhash1_create(void) {
  struct _openslide_hash *hash = g_slice_new(struct _openslide_hash);
  hash->checksum = g_checksum_new(G_CHECKSUM_SHA256);

  return hash;
}

void _openslide_hash_string(struct _openslide_hash *hash, const char *str) {
  if (hash == NULL) {
    return;
  }

  GChecksum *checksum = hash->checksum;

  const char *str_to_hash = str ? str : "";
  g_checksum_update(checksum,
		    (const guchar *) str_to_hash,
		    strlen(str_to_hash) + 1);
}

bool _openslide_hash_tiff_tiles(struct _openslide_hash *hash, TIFF *tiff) {
  g_assert(TIFFIsTiled(tiff));

  // get tile sizes
  toff_t *sizes;
  if (TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes) == 0) {
    g_critical("Cannot get tile size");
    return false;  // ok, haven't allocated anything yet
  }

  // get offsets
  toff_t *offsets;
  if (TIFFGetField(tiff, TIFFTAG_TILEOFFSETS, &offsets) == 0) {
    g_critical("Cannot get offsets");
    return false;  // ok, haven't allocated anything yet
  }

  // hash each tile's raw data
  ttile_t count = TIFFNumberOfTiles(tiff);
  const char *filename = TIFFFileName(tiff);
  for (ttile_t tile_no = 0; tile_no < count; tile_no++) {
    if (!_openslide_hash_file_part(hash, filename, offsets[tile_no], sizes[tile_no])) {
      return false;
    }
  }

  return true;
}


bool _openslide_hash_file(struct _openslide_hash *hash, const char *filename) {
  gchar *contents;
  gsize length;

  g_return_val_if_fail(g_file_get_contents(filename, &contents, &length, NULL), false);

  if (hash != NULL) {
    GChecksum *checksum = hash->checksum;
    g_checksum_update(checksum, (guchar *) contents, length);
  }

  g_free(contents);
  return true;
}

bool _openslide_hash_file_part(struct _openslide_hash *hash,
			       const char *filename,
			       int64_t offset, int size) {
  FILE *f = fopen(filename, "rb");
  g_return_val_if_fail(f, false);

  void *buf = g_slice_alloc(size);

  if (fseeko(f, offset, SEEK_SET) == -1) {
    g_critical("Can't seek in %s", filename);
    g_slice_free1(size, buf);
    fclose(f);
    return false;
  }

  if (fread(buf, size, 1, f) != 1) {
    g_critical("Can't read from %s", filename);
    g_slice_free1(size, buf);
    fclose(f);
    return false;
  }

  //g_debug("hash '%s' %" PRId64 " %d", filename, offset, size);
  if (hash != NULL) {
    GChecksum *checksum = hash->checksum;
    g_checksum_update(checksum, (guchar *) buf, size);
  }

  g_slice_free1(size, buf);
  fclose(f);
  return true;
}

const char *_openslide_hash_get_string(struct _openslide_hash *hash) {
  return g_checksum_get_string(hash->checksum);
}

void _openslide_hash_destroy(struct _openslide_hash *hash) {
  g_checksum_free(hash->checksum);
  g_slice_free(struct _openslide_hash, hash);
}

#else

struct _openslide_hash *_openslide_hash_quickhash1_create(void) {
  return NULL;
}

bool _openslide_hash_tiff_tiles(struct _openslide_hash *_OPENSLIDE_UNUSED(hash),
				TIFF *_OPENSLIDE_UNUSED(tiff)) { return true; }
void _openslide_hash_string(struct _openslide_hash *_OPENSLIDE_UNUSED(hash),
			    const char *_OPENSLIDE_UNUSED(str)) {}
bool _openslide_hash_file(struct _openslide_hash *_OPENSLIDE_UNUSED(hash),
			  const char *_OPENSLIDE_UNUSED(filename)) { return true; }
bool _openslide_hash_file_part(struct _openslide_hash *_OPENSLIDE_UNUSED(hash),
			       const char *_OPENSLIDE_UNUSED(filename),
			       int64_t _OPENSLIDE_UNUSED(offset),
			       int _OPENSLIDE_UNUSED(size)) { return true; }
const char *_openslide_hash_get_string(struct _openslide_hash *_OPENSLIDE_UNUSED(hash)) {
  return NULL;
}

void _openslide_hash_destroy(struct _openslide_hash *_OPENSLIDE_UNUSED(hash)) {}

#endif
