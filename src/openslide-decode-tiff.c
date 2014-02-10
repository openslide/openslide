/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2013 Carnegie Mellon University
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

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-tiff.h"

#include <glib.h>
#include <tiffio.h>

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cairo.h>

#include "openslide-hash.h"

#define HANDLE_CACHE_MAX 32

struct _openslide_tiffcache {
  char *filename;
  GQueue *cache;
  GMutex *lock;
  int outstanding;
};

// not thread-safe, like libtiff
struct tiff_file_handle {
  struct _openslide_tiffcache *tc;
  int64_t offset;
  int64_t size;
};

struct associated_image {
  struct _openslide_associated_image base;
  struct _openslide_tiffcache *tc;
  tdir_t directory;
};

#define SET_DIR_OR_FAIL(tiff, i)					\
  do {									\
    if (!TIFFSetDirectory(tiff, i)) {					\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,		\
                  "Cannot set TIFF directory %d", i);			\
      return false;							\
    }									\
  } while (0)

#define GET_FIELD_OR_FAIL(tiff, tag, result)				\
  do {									\
    uint32 tmp;								\
    if (!TIFFGetField(tiff, tag, &tmp)) {				\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,		\
                  "Cannot get required TIFF tag: %d", tag);		\
      return false;							\
    }									\
    result = tmp;							\
  } while (0)

bool _openslide_tiff_level_init(TIFF *tiff,
                                tdir_t dir,
                                struct _openslide_level *level,
                                struct _openslide_tiff_level *tiffl,
                                GError **err) {
  // set the directory
  SET_DIR_OR_FAIL(tiff, dir);

  // figure out tile size
  int64_t tw, th;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_TILEWIDTH, tw);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_TILELENGTH, th);

  // get image size
  int64_t iw, ih;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, iw);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, ih);

  // safe now, start writing
  if (level) {
    level->w = iw;
    level->h = ih;
    // tile size hints
    level->tile_w = tw;
    level->tile_h = th;
  }

  if (tiffl) {
    tiffl->dir = dir;
    tiffl->image_w = iw;
    tiffl->image_h = ih;
    tiffl->tile_w = tw;
    tiffl->tile_h = th;

    // num tiles in each dimension
    tiffl->tiles_across = (iw / tw) + !!(iw % tw);   // integer ceiling
    tiffl->tiles_down = (ih / th) + !!(ih % th);
  }

  return true;
}

// clip right/bottom edges of tile in last row/column
bool _openslide_tiff_clip_tile(struct _openslide_tiff_level *tiffl,
                               uint32_t *tiledata,
                               int64_t tile_col, int64_t tile_row,
                               GError **err) {
  return _openslide_clip_tile(tiledata,
                              tiffl->tile_w, tiffl->tile_h,
                              tiffl->image_w - tile_col * tiffl->tile_w,
                              tiffl->image_h - tile_row * tiffl->tile_h,
                              err);
}

static bool tiff_read_region(TIFF *tiff,
                             uint32_t *dest,
                             int64_t x, int64_t y,
                             int32_t w, int32_t h,
                             GError **err) {
  TIFFRGBAImage img;
  char emsg[1024] = "unknown error";
  bool success = false;

  // init
  if (!TIFFRGBAImageOK(tiff, emsg)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Failure in TIFFRGBAImageOK: %s", emsg);
    return false;
  }
  if (!TIFFRGBAImageBegin(&img, tiff, 1, emsg)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Failure in TIFFRGBAImageBegin: %s", emsg);
    return false;
  }
  img.req_orientation = ORIENTATION_TOPLEFT;
  img.col_offset = x;
  img.row_offset = y;

  // draw it
  if (TIFFRGBAImageGet(&img, dest, w, h)) {
    // convert ABGR -> ARGB
    for (uint32_t *p = dest; p < dest + w * h; p++) {
      uint32_t val = GUINT32_SWAP_LE_BE(*p);
      *p = (val << 24) | (val >> 8);
    }
    success = true;
  } else {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "TIFFRGBAImageGet failed");
    memset(dest, 0, w * h * 4);
  }

  // done
  TIFFRGBAImageEnd(&img);
  return success;
}

bool _openslide_tiff_read_tile(struct _openslide_tiff_level *tiffl,
                               TIFF *tiff,
                               uint32_t *dest,
                               int64_t tile_col, int64_t tile_row,
                               GError **err) {
  // set directory
  SET_DIR_OR_FAIL(tiff, tiffl->dir);

  // read tile
  return tiff_read_region(tiff, dest,
                          tile_col * tiffl->tile_w, tile_row * tiffl->tile_h,
                          tiffl->tile_w, tiffl->tile_h, err);
}

bool _openslide_tiff_read_tile_data(struct _openslide_tiff_level *tiffl,
                                    TIFF *tiff,
                                    void **_buf, int32_t *_len,
                                    int64_t tile_col, int64_t tile_row,
                                    GError **err) {
  // initialize out params
  *_buf = NULL;
  *_len = 0;

  // set directory
  SET_DIR_OR_FAIL(tiff, tiffl->dir);

  // get tile number
  ttile_t tile_no = TIFFComputeTile(tiff,
                                    tile_col * tiffl->tile_w,
                                    tile_row * tiffl->tile_h,
                                    0, 0);

  //g_debug("_openslide_tiff_read_tile_data reading tile %d", tile_no);

  // get tile size
  toff_t *sizes;
  if (TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes) == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot get tile size");
    return false;  // ok, haven't allocated anything yet
  }
  tsize_t tile_size = sizes[tile_no];

  // a slide with zero-length tiles has been seen in the wild
  if (!tile_size) {
    //g_debug("no data for tile %d", tile_no);
    return true;  // ok, haven't allocated anything yet
  }

  // get raw tile
  tdata_t buf = g_malloc(tile_size);
  tsize_t size = TIFFReadRawTile(tiff, tile_no, buf, tile_size);
  if (size == -1) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Cannot read raw tile");
    g_free(buf);
    return false;
  }

  // set outputs
  *_buf = buf;
  *_len = size;
  return true;
}

static bool _get_associated_image_data(TIFF *tiff,
                                       struct associated_image *img,
                                       uint32_t *dest,
                                       GError **err) {
  int64_t width, height;

  // g_debug("read TIFF associated image: %d", img->directory);

  SET_DIR_OR_FAIL(tiff, img->directory);

  // ensure dimensions have not changed
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, width);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, height);
  if (img->base.w != width || img->base.h != height) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected associated image size: "
                "expected %"G_GINT64_FORMAT"x%"G_GINT64_FORMAT", "
                "got %"G_GINT64_FORMAT"x%"G_GINT64_FORMAT,
                img->base.w, img->base.h, width, height);
    return false;
  }

  // load the image
  return tiff_read_region(tiff, dest, 0, 0, width, height, err);
}

static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dest,
                                      GError **err) {
  struct associated_image *img = (struct associated_image *) _img;
  TIFF *tiff = _openslide_tiffcache_get(img->tc, err);
  bool success = false;
  if (tiff) {
    success = _get_associated_image_data(tiff, img, dest, err);
  }
  _openslide_tiffcache_put(img->tc, tiff);
  return success;
}

static void destroy_associated_image(struct _openslide_associated_image *_img) {
  struct associated_image *img = (struct associated_image *) _img;

  g_slice_free(struct associated_image, img);
}

static const struct _openslide_associated_image_ops tiff_associated_ops = {
  .get_argb_data = get_associated_image_data,
  .destroy = destroy_associated_image,
};

static bool _add_associated_image(openslide_t *osr,
                                  const char *name,
                                  struct _openslide_tiffcache *tc,
                                  tdir_t dir,
                                  TIFF *tiff,
                                  GError **err) {
  // set directory
  SET_DIR_OR_FAIL(tiff, dir);

  // get the dimensions
  int64_t w, h;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, w);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, h);

  // check compression
  uint16_t compression;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_COMPRESSION, compression);
  if (!TIFFIsCODECConfigured(compression)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported TIFF compression: %u", compression);
    return false;
  }

  // load into struct
  struct associated_image *img = g_slice_new0(struct associated_image);
  img->base.ops = &tiff_associated_ops;
  img->base.w = w;
  img->base.h = h;
  img->tc = tc;
  img->directory = dir;

  // save
  g_hash_table_insert(osr->associated_images, g_strdup(name), img);

  return true;
}

bool _openslide_tiff_add_associated_image(openslide_t *osr,
                                          const char *name,
                                          struct _openslide_tiffcache *tc,
                                          tdir_t dir,
                                          GError **err) {
  TIFF *tiff = _openslide_tiffcache_get(tc, err);
  bool ret = false;
  if (tiff) {
    ret = _add_associated_image(osr, name, tc, dir, tiff, err);
  }
  _openslide_tiffcache_put(tc, tiff);
  return ret;
}

static tsize_t tiff_do_read(thandle_t th, tdata_t buf, tsize_t size) {
  struct tiff_file_handle *hdl = th;

  // don't leave the file handle open between calls
  // also ensures FD_CLOEXEC is set
  FILE *f = _openslide_fopen(hdl->tc->filename, "rb", NULL);
  if (f == NULL) {
    return 0;
  }
  if (fseeko(f, hdl->offset, SEEK_SET)) {
    fclose(f);
    return 0;
  }
  int64_t rsize = fread(buf, 1, size, f);
  hdl->offset += rsize;
  fclose(f);
  return rsize;
}

static tsize_t tiff_do_write(thandle_t th G_GNUC_UNUSED,
                             tdata_t data G_GNUC_UNUSED,
                             tsize_t size G_GNUC_UNUSED) {
  // fail
  return 0;
}

static toff_t tiff_do_seek(thandle_t th, toff_t offset, int whence) {
  struct tiff_file_handle *hdl = th;

  switch (whence) {
  case SEEK_SET:
    hdl->offset = offset;
    break;
  case SEEK_CUR:
    hdl->offset += offset;
    break;
  case SEEK_END:
    hdl->offset = hdl->size + offset;
    break;
  default:
    g_assert_not_reached();
  }
  return hdl->offset;
}

static int tiff_do_close(thandle_t th) {
  struct tiff_file_handle *hdl = th;

  g_slice_free(struct tiff_file_handle, hdl);
  return 0;
}

static toff_t tiff_do_size(thandle_t th) {
  struct tiff_file_handle *hdl = th;

  return hdl->size;
}

static TIFF *tiff_open(struct _openslide_tiffcache *tc, GError **err) {
  // open
  FILE *f = _openslide_fopen(tc->filename, "rb", err);
  if (f == NULL) {
    return NULL;
  }

  // read magic
  uint8_t buf[4];
  if (fread(buf, 4, 1, f) != 1) {
    // can't read
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Couldn't read TIFF magic number for %s", tc->filename);
    fclose(f);
    return NULL;
  }

  // get size
  if (fseeko(f, 0, SEEK_END) == -1) {
    _openslide_io_error(err, "Couldn't seek to end of %s", tc->filename);
    fclose(f);
    return NULL;
  }
  int64_t size = ftello(f);
  if (size == -1) {
    _openslide_io_error(err, "Couldn't ftello() for %s", tc->filename);
    fclose(f);
    return NULL;
  }
  fclose(f);

  // check magic
  // TODO: remove if libtiff gets private error/warning callbacks
  if (buf[0] != buf[1]) {
    goto NOT_TIFF;
  }
  uint16_t version;
  switch (buf[0]) {
  case 'M':
    // big endian
    version = (buf[2] << 8) | buf[3];
    break;
  case 'I':
    // little endian
    version = (buf[3] << 8) | buf[2];
    break;
  default:
    goto NOT_TIFF;
  }
  if (version != 42 && version != 43) {
    goto NOT_TIFF;
  }
  if (version == 43 && sizeof(toff_t) == 4) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "BigTIFF support requires libtiff >= 4");
    return NULL;
  }

  // allocate
  struct tiff_file_handle *hdl = g_slice_new0(struct tiff_file_handle);
  hdl->tc = tc;
  hdl->size = size;

  // TIFFOpen
  // mode: m disables mmap to avoid sigbus and other mmap fragility
  TIFF *tiff = TIFFClientOpen(tc->filename, "rm", hdl,
                              tiff_do_read, tiff_do_write, tiff_do_seek,
                              tiff_do_close, tiff_do_size, NULL, NULL);
  if (tiff == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid TIFF: %s", tc->filename);
    tiff_do_close(hdl);
  }
  return tiff;

NOT_TIFF:
  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "Not a TIFF file: %s", tc->filename);
  return NULL;
}

struct _openslide_tiffcache *_openslide_tiffcache_create(const char *filename) {
  struct _openslide_tiffcache *tc = g_slice_new0(struct _openslide_tiffcache);
  tc->filename = g_strdup(filename);
  tc->cache = g_queue_new();
  tc->lock = g_mutex_new();
  return tc;
}

TIFF *_openslide_tiffcache_get(struct _openslide_tiffcache *tc, GError **err) {
  //g_debug("get TIFF");
  g_mutex_lock(tc->lock);
  tc->outstanding++;
  TIFF *tiff = g_queue_pop_head(tc->cache);
  g_mutex_unlock(tc->lock);

  if (tiff == NULL) {
    //g_debug("create TIFF");
    // Does not check that we have the same file.  Then again, neither does
    // tiff_do_read.
    tiff = tiff_open(tc, err);
  }
  if (tiff == NULL) {
    g_mutex_lock(tc->lock);
    tc->outstanding--;
    g_mutex_unlock(tc->lock);
  }
  return tiff;
}

void _openslide_tiffcache_put(struct _openslide_tiffcache *tc, TIFF *tiff) {
  if (tiff == NULL) {
    return;
  }

  //g_debug("put TIFF");
  g_mutex_lock(tc->lock);
  g_assert(tc->outstanding);
  tc->outstanding--;
  if (g_queue_get_length(tc->cache) < HANDLE_CACHE_MAX) {
    g_queue_push_head(tc->cache, tiff);
    tiff = NULL;
  }
  g_mutex_unlock(tc->lock);

  if (tiff) {
    //g_debug("too many TIFFs");
    TIFFClose(tiff);
  }
}

void _openslide_tiffcache_destroy(struct _openslide_tiffcache *tc) {
  if (tc == NULL) {
    return;
  }
  g_mutex_lock(tc->lock);
  TIFF *tiff;
  while ((tiff = g_queue_pop_head(tc->cache)) != NULL) {
    TIFFClose(tiff);
  }
  g_assert(tc->outstanding == 0);
  g_mutex_unlock(tc->lock);
  g_queue_free(tc->cache);
  g_mutex_free(tc->lock);
  g_free(tc->filename);
  g_slice_free(struct _openslide_tiffcache, tc);
}
