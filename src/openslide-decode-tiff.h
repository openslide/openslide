/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
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

#ifndef OPENSLIDE_OPENSLIDE_DECODE_TIFF_H_
#define OPENSLIDE_OPENSLIDE_DECODE_TIFF_H_

#include "openslide-private.h"
#include "openslide-hash.h"

#include <stdint.h>
#include <glib.h>
#include <tiffio.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(TIFF, TIFFClose)

struct _openslide_tiff_level {
  tdir_t dir;
  int64_t image_w;
  int64_t image_h;
  int64_t tile_w;
  int64_t tile_h;
  int64_t tiles_across;
  int64_t tiles_down;

  bool tile_read_direct;
  gint warned_read_indirect;
  uint16_t photometric;
};

struct _openslide_tiffcache;

struct _openslide_cached_tiff {
  struct _openslide_tiffcache *tc;
  TIFF *tiff;
};

bool _openslide_tiff_level_init(TIFF *tiff,
                                tdir_t dir,
                                struct _openslide_level *level,
                                struct _openslide_tiff_level *tiffl,
                                GError **err);

bool _openslide_tiff_check_missing_tile(struct _openslide_tiff_level *tiffl,
                                        TIFF *tiff,
                                        int64_t tile_col, int64_t tile_row,
                                        bool *is_missing,
                                        GError **err);

bool _openslide_tiff_read_tile(struct _openslide_tiff_level *tiffl,
                               TIFF *tiff,
                               uint32_t *dest,
                               int64_t tile_col, int64_t tile_row,
                               GError **err);

bool _openslide_tiff_read_tile_data(struct _openslide_tiff_level *tiffl,
                                    TIFF *tiff,
                                    void **buf, int32_t *len,
                                    int64_t tile_col, int64_t tile_row,
                                    GError **err);

bool _openslide_tiff_clip_tile(struct _openslide_tiff_level *tiffl,
                               uint32_t *tiledata,
                               int64_t tile_col, int64_t tile_row,
                               GError **err);

bool _openslide_tiff_add_associated_image(openslide_t *osr,
                                          const char *name,
                                          struct _openslide_tiffcache *tc,
                                          tdir_t dir,
                                          GError **err);

bool _openslide_tiff_set_dir(TIFF *tiff,
                             tdir_t dir,
                             GError **err);


/* TIFF handles are not thread-safe, so we have a handle cache for
   multithreaded access */
struct _openslide_tiffcache *_openslide_tiffcache_create(const char *filename);

// result.tiff is NULL on error
struct _openslide_cached_tiff _openslide_tiffcache_get(struct _openslide_tiffcache *tc,
                                                       GError **err);

void _openslide_cached_tiff_put(struct _openslide_cached_tiff *ct);

void _openslide_tiffcache_destroy(struct _openslide_tiffcache *tc);

typedef struct _openslide_tiffcache _openslide_tiffcache;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(_openslide_tiffcache,
                              _openslide_tiffcache_destroy)

typedef struct _openslide_cached_tiff _openslide_cached_tiff;
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(_openslide_cached_tiff,
                                 _openslide_cached_tiff_put)

#endif
