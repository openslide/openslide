/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2010 Carnegie Mellon University
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

#include "openslide-hash.h"

#include <string.h>

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
