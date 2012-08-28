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
#include <glib.h>

struct _openslide_hash {
  GChecksum *checksum;
  bool enabled;
};

struct _openslide_hash *_openslide_hash_quickhash1_create(void) {
  struct _openslide_hash *hash = g_slice_new(struct _openslide_hash);
  hash->checksum = g_checksum_new(G_CHECKSUM_SHA256);
  hash->enabled = true;

  return hash;
}

void _openslide_hash_string(struct _openslide_hash *hash, const char *str) {
  if (hash == NULL || !hash->enabled) {
    return;
  }

  GChecksum *checksum = hash->checksum;

  const char *str_to_hash = str ? str : "";
  g_checksum_update(checksum,
		    (const guchar *) str_to_hash,
		    strlen(str_to_hash) + 1);
}

bool _openslide_hash_tiff_tiles(struct _openslide_hash *hash, TIFF *tiff,
                                GError **err) {
  g_assert(TIFFIsTiled(tiff));

  // get tile count
  ttile_t count = TIFFNumberOfTiles(tiff);

  // get tile sizes
  toff_t *sizes;
  if (TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes) == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot get tile size");
    return false;  // ok, haven't allocated anything yet
  }
  toff_t total = 0;
  for (ttile_t tile_no = 0; tile_no < count; tile_no++) {
    total += sizes[tile_no];
    if (total > (5 << 20)) {
      // This is a non-pyramidal image or one with a very large top level.
      // Refuse to calculate a quickhash for it to keep openslide_open()
      // from taking an arbitrary amount of time.  (#79)
      hash->enabled = false;
      return true;  // ok, haven't allocated anything yet
    }
  }

  // get offsets
  toff_t *offsets;
  if (TIFFGetField(tiff, TIFFTAG_TILEOFFSETS, &offsets) == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot get offsets");
    return false;  // ok, haven't allocated anything yet
  }

  // hash each tile's raw data
  const char *filename = TIFFFileName(tiff);
  for (ttile_t tile_no = 0; tile_no < count; tile_no++) {
    if (!_openslide_hash_file_part(hash, filename, offsets[tile_no], sizes[tile_no], err)) {
      return false;
    }
  }

  return true;
}


bool _openslide_hash_file(struct _openslide_hash *hash, const char *filename,
                          GError **err) {
  // determine size of file
  FILE *f = _openslide_fopen(filename, "rb", err);
  if (f == NULL) {
    return false;
  }

  fseeko(f, 0, SEEK_END);
  int64_t size = ftello(f);
  if (size == -1) {
    _openslide_io_error(err, "Couldn't get size of %s", filename);
    fclose(f);
    return false;
  }

  fclose(f);

  return _openslide_hash_file_part(hash, filename, 0, size, err);
}

bool _openslide_hash_file_part(struct _openslide_hash *hash,
			       const char *filename,
			       int64_t offset, int64_t size,
			       GError **err) {
  FILE *f = _openslide_fopen(filename, "rb", err);
  if (f == NULL) {
    return false;
  }

  uint8_t buf[4096];

  if (fseeko(f, offset, SEEK_SET) == -1) {
    _openslide_io_error(err, "Can't seek in %s", filename);
    fclose(f);
    return false;
  }

  int64_t bytes_left = size;
  while (bytes_left > 0) {
    int64_t bytes_to_read = MIN((int64_t) sizeof buf, bytes_left);
    int64_t bytes_read = fread(buf, 1, bytes_to_read, f);

    if (bytes_read != bytes_to_read) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                  "Can't read from %s", filename);
      fclose(f);
      return false;
    }

    //    g_debug("hash '%s' %" G_GINT64_FORMAT " %d", filename, offset + (size - bytes_left), bytes_to_read);

    bytes_left -= bytes_read;

    if (hash != NULL) {
      GChecksum *checksum = hash->checksum;
      g_checksum_update(checksum, buf, bytes_read);
    }
  }

  fclose(f);
  return true;
}

const char *_openslide_hash_get_string(struct _openslide_hash *hash) {
  if (hash->enabled) {
    return g_checksum_get_string(hash->checksum);
  } else {
    return NULL;
  }
}

void _openslide_hash_destroy(struct _openslide_hash *hash) {
  g_checksum_free(hash->checksum);
  g_slice_free(struct _openslide_hash, hash);
}
