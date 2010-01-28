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

#include "openslide-hash.h"

#include <string.h>
#include <inttypes.h>

void _openslide_hash_string(GChecksum *checksum, const char *str) {
  if (checksum == NULL) {
    return;
  }

  const char *str_to_hash = str ? str : "";
  g_checksum_update(checksum,
		    (const guchar *) str_to_hash,
		    strlen(str_to_hash) + 1);
}

void _openslide_hash_tiff_tiles(GChecksum *checksum, TIFF *tiff) {
  if (checksum == NULL) {
    return;
  }

  g_assert(TIFFIsTiled(tiff));

  // set up buffer
  tsize_t buf_size = TIFFTileSize(tiff);
  tdata_t buf = g_slice_alloc(buf_size);

  // hash each tile's raw data
  ttile_t count = TIFFNumberOfTiles(tiff);
  for (ttile_t tile_no = 0; tile_no < count; tile_no++) {
    tsize_t size = TIFFReadRawTile(tiff, tile_no, buf, buf_size);
    if (size == -1) {
      g_critical("TIFFReadRawTile failed");
      continue;
    }

    //g_debug("hashing tile %d", tile_no);
    g_checksum_update(checksum, buf, size);
  }

  // free
  g_slice_free1(buf_size, buf);
}


void _openslide_hash_file(GChecksum *checksum, const char *filename) {
  if (checksum == NULL) {
    return;
  }

  gchar *contents;
  gsize length;

  g_return_if_fail(g_file_get_contents(filename, &contents, &length, NULL));

  g_checksum_update(checksum, (guchar *) contents, length);
  g_free(contents);
}

void _openslide_hash_file_part(GChecksum *checksum, const char *filename,
			       int64_t offset, int size) {
  if (checksum == NULL) {
    return;
  }

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
