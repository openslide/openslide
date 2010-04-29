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

//#include <config.h>

#include "openslide-private.h"

#include "openslide-hash.h"

#include <string.h>

#ifndef _MSC_VER
#include <inttypes.h>
#endif

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

void _openslide_hash_tiff_tiles(struct _openslide_hash *hash, TIFF *tiff) {
  if (hash == NULL) {
    return;
  }

  g_assert(TIFFIsTiled(tiff));

  // get tile sizes
  toff_t *sizes;
  if (TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes) == 0) {
    g_critical("Cannot get tile size");
    return;  // ok, haven't allocated anything yet
  }

  // get offsets
  toff_t *offsets;
  if (TIFFGetField(tiff, TIFFTAG_TILEOFFSETS, &offsets) == 0) {
    g_critical("Cannot get offsets");
    return;  // ok, haven't allocated anything yet
  }

  // hash each tile's raw data
  ttile_t count = TIFFNumberOfTiles(tiff);
  const char *filename = TIFFFileName(tiff);
  for (ttile_t tile_no = 0; tile_no < count; tile_no++) {
    _openslide_hash_file_part(hash, filename, offsets[tile_no], sizes[tile_no]);
  }
}


void _openslide_hash_file(struct _openslide_hash *hash, const char *filename) {
  if (hash == NULL) {
    return;
  }

  GChecksum *checksum = hash->checksum;

  gchar *contents;
  gsize length;

  g_return_if_fail(g_file_get_contents(filename, &contents, &length, NULL));

  g_checksum_update(checksum, (guchar *) contents, length);
  g_free(contents);
}

void _openslide_hash_file_part(struct _openslide_hash *hash,
			       const char *filename,
			       int64_t offset, int size) {
  if (hash == NULL) {
    return;
  }

  GChecksum *checksum = hash->checksum;

  FILE *f = fopen(filename, "rb");
  g_return_if_fail(f);

  void *buf = g_slice_alloc(size);

  if (fseeko(f, offset, SEEK_SET) == -1) {
    g_critical("Can't seek in %s", filename);
    g_slice_free1(size, buf);
    fclose(f);
  }

  if (fread(buf, size, 1, f) != 1) {
    g_critical("Can't read from %s", filename);
    g_slice_free1(size, buf);
    fclose(f);
  }

  //g_debug("hash '%s' %" PRId64 " %d", filename, offset, size);
  g_checksum_update(checksum, (guchar *) buf, size);
  g_slice_free1(size, buf);
  fclose(f);
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

void _openslide_hash_tiff_tiles(struct _openslide_hash *_OPENSLIDE_UNUSED(hash),
				TIFF *_OPENSLIDE_UNUSED(tiff)) {}
void _openslide_hash_string(struct _openslide_hash *_OPENSLIDE_UNUSED(hash),
			    const char *_OPENSLIDE_UNUSED(str)) {}
void _openslide_hash_file(struct _openslide_hash *_OPENSLIDE_UNUSED(hash),
			  const char *_OPENSLIDE_UNUSED(filename)) {}
void _openslide_hash_file_part(struct _openslide_hash *_OPENSLIDE_UNUSED(hash),
			       const char *_OPENSLIDE_UNUSED(filename),
			       int64_t _OPENSLIDE_UNUSED(offset),
			       int _OPENSLIDE_UNUSED(size)) {}
const char *_openslide_hash_get_string(struct _openslide_hash *_OPENSLIDE_UNUSED(hash)) {
  return NULL;
}

void _openslide_hash_destroy(struct _openslide_hash *_OPENSLIDE_UNUSED(hash)) {}

#endif
