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


#ifdef HAVE_G_CHECKSUM_NEW

// use GChecksum
#define Checksum GChecksum
#define checksum_new g_checksum_new
#define checksum_update g_checksum_update
#define checksum_get_string g_checksum_get_string
#define checksum_free g_checksum_free
#define CHECKSUM_SHA256 G_CHECKSUM_SHA256

#else

// fallback
#include "sha256.h"

#define Checksum SHA256_CTX
#define CHECKSUM_SHA256

static SHA256_CTX *checksum_new(void)
{
  SHA256_CTX *ctx = g_slice_new(SHA256_CTX);
  SHA256_Init(ctx);
  return ctx;
}

static void checksum_update(SHA256_CTX *ctx, const guchar *data, gssize length)
{
  SHA256_Update(ctx, data, length);
}

static const gchar _tohex[] = "0123456789abcdef";
static gchar *checksum_get_string(SHA256_CTX *ctx)
{
  static gchar hexdigest[SHA256_DIGEST_LENGTH*2+1];
  guchar digest[SHA256_DIGEST_LENGTH];
  SHA256_Final(digest, ctx);
  for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    hexdigest[i*2] = _tohex[(digest[i] & 0xf0) >> 4];
    hexdigest[i*2+1] = _tohex[(digest[i] & 0x0f)];
  }
  hexdigest[SHA256_DIGEST_LENGTH*2] = '\0';
  return hexdigest;
}

static void checksum_free(SHA256_CTX *ctx)
{
  g_slice_free(SHA256_CTX, ctx);
}

#endif

struct _openslide_hash {
  Checksum *checksum;
};

struct _openslide_hash *_openslide_hash_quickhash1_create(void) {
  struct _openslide_hash *hash = g_slice_new(struct _openslide_hash);
  hash->checksum = checksum_new(CHECKSUM_SHA256);

  return hash;
}

void _openslide_hash_string(struct _openslide_hash *hash, const char *str) {
  if (hash == NULL) {
    return;
  }

  Checksum *checksum = hash->checksum;

  const char *str_to_hash = str ? str : "";
  checksum_update(checksum,
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
  // determine size of file
  FILE *f = fopen(filename, "rb");
  g_return_val_if_fail(f, false);
  fseeko(f, 0, SEEK_END);
  int64_t size = ftello(f);
  fclose(f);

  g_return_val_if_fail(size != -1, false);

  return _openslide_hash_file_part(hash, filename, 0, size);
}

bool _openslide_hash_file_part(struct _openslide_hash *hash,
			       const char *filename,
			       int64_t offset, int64_t size) {
  FILE *f = fopen(filename, "rb");
  g_return_val_if_fail(f, false);

  uint8_t buf[4096];

  if (fseeko(f, offset, SEEK_SET) == -1) {
    g_critical("Can't seek in %s", filename);
    fclose(f);
    return false;
  }

  int64_t bytes_left = size;
  while (bytes_left > 0) {
    int64_t bytes_to_read = MIN((int64_t) sizeof buf, bytes_left);
    int64_t bytes_read = fread(buf, 1, bytes_to_read, f);

    if (bytes_read != bytes_to_read) {
      g_critical("Can't read from %s", filename);
      fclose(f);
      return false;
    }

    //    g_debug("hash '%s' %" G_GINT64_FORMAT " %d", filename, offset + (size - bytes_left), bytes_to_read);

    bytes_left -= bytes_read;

    if (hash != NULL) {
      Checksum *checksum = hash->checksum;
      checksum_update(checksum, (guchar *) buf, bytes_read);
    }
  }

  fclose(f);
  return true;
}

const char *_openslide_hash_get_string(struct _openslide_hash *hash) {
  return checksum_get_string(hash->checksum);
}

void _openslide_hash_destroy(struct _openslide_hash *hash) {
  checksum_free(hash->checksum);
  g_slice_free(struct _openslide_hash, hash);
}
