/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2024 Benjamin Gilbert
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
#include "openslide-decode-jpeg.h"

#include <glib.h>
#include <tiffio.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <cairo.h>

#include "openslide-hash.h"

#define HANDLE_CACHE_MAX 32

struct _openslide_tiffcache {
  char *filename;
  GQueue *cache;
  GMutex lock;
  int outstanding;
};

// not thread-safe, like libtiff
struct tiff_file_handle {
  struct _openslide_tiffcache *tc;
  struct _openslide_file *f;
  int64_t offset;
  int64_t size;
  char *last_error;
};

struct associated_image {
  struct _openslide_associated_image base;
  struct _openslide_tiffcache *tc;
  tdir_t directory;
};

#define SET_DIR_OR_FAIL(tiff, i)					\
  do {									\
    if (!_openslide_tiff_set_dir(tiff, i, err)) {			\
      return false;							\
    }									\
  } while (0)

#define GET_FIELD_OR_FAIL(tiff, tag, type, result)			\
  do {									\
    type tmp;								\
    if (!TIFFGetField(tiff, tag, &tmp)) {				\
      _openslide_tiff_error(err, tiff,					\
                            "Cannot get required TIFF tag: %d", tag);	\
      return false;							\
    }									\
    result = tmp;							\
  } while (0)

bool _openslide_tiff_set_dir(TIFF *tiff,
                             tdir_t dir,
                             GError **err) {
  if (dir == TIFFCurrentDirectory(tiff)) {
    // avoid libtiff unnecessarily rereading directory contents
    return true;
  }
  if (!TIFFSetDirectory(tiff, dir)) {  // ci-allow
    _openslide_tiff_error(err, tiff, "Cannot set TIFF directory %d", dir);
    return false;
  }
  return true;
}

bool _openslide_tiff_level_init(TIFF *tiff,
                                tdir_t dir,
                                struct _openslide_level *level,
                                struct _openslide_tiff_level *tiffl,
                                GError **err) {
  // set the directory
  SET_DIR_OR_FAIL(tiff, dir);

  // figure out tile size
  int64_t tw, th;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_TILEWIDTH, uint32_t, tw);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_TILELENGTH, uint32_t, th);

  // get image size
  int64_t iw, ih;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, uint32_t, iw);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, uint32_t, ih);

  // decide whether we can bypass libtiff when reading tiles
  uint16_t compression, planar_config, photometric;
  uint16_t bits_per_sample, samples_per_pixel;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_COMPRESSION, uint16_t, compression);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_PLANARCONFIG, uint16_t, planar_config);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_PHOTOMETRIC, uint16_t, photometric);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_BITSPERSAMPLE, uint16_t, bits_per_sample);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_SAMPLESPERPIXEL, uint16_t, samples_per_pixel);
  bool read_direct =
    compression == COMPRESSION_JPEG &&
    planar_config == PLANARCONFIG_CONTIG &&
    (photometric == PHOTOMETRIC_RGB || photometric == PHOTOMETRIC_YCBCR) &&
    bits_per_sample == 8 &&
    samples_per_pixel == 3;
  //g_debug("directory %d, read_direct %d", dir, read_direct);

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

    tiffl->tile_read_direct = read_direct;
    tiffl->photometric = photometric;
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
    _openslide_tiff_error(err, tiff, "TIFFRGBAImageGet failed");
    memset(dest, 0, w * h * 4);
  }

  // done
  TIFFRGBAImageEnd(&img);
  return success;
}

static bool decode_jpeg(const void *buf, uint32_t buflen,
                        const void *tables, uint32_t tables_len,  // optional
                        J_COLOR_SPACE space,
                        uint32_t *dest,
                        int32_t w, int32_t h,
                        GError **err) {
  jmp_buf env;

  struct jpeg_decompress_struct *cinfo;
  g_auto(_openslide_jpeg_decompress) dc =
    _openslide_jpeg_decompress_create(&cinfo);

  if (setjmp(env) == 0) {
    _openslide_jpeg_decompress_init(dc, &env);

    // load JPEG tables
    if (tables) {
      _openslide_jpeg_mem_src(cinfo, tables, tables_len);
      if (jpeg_read_header(cinfo, false) != JPEG_HEADER_TABLES_ONLY) {
        g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                    "Couldn't load JPEG tables");
        return false;
      }
    }

    // set up I/O
    _openslide_jpeg_mem_src(cinfo, buf, buflen);

    // read header
    if (jpeg_read_header(cinfo, true) != JPEG_HEADER_OK) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG header");
      return false;
    }

    // set color space from TIFF photometric tag (for Aperio)
    cinfo->jpeg_color_space = space;

    // decompress
    if (!_openslide_jpeg_decompress_run(dc, dest, false, w, h, err)) {
      return false;
    }
    return true;
  } else {
    // setjmp has returned again
    _openslide_jpeg_propagate_error(err, dc);
    return false;
  }
}

bool _openslide_tiff_read_tile(struct _openslide_tiff_level *tiffl,
                               TIFF *tiff,
                               uint32_t *dest,
                               int64_t tile_col, int64_t tile_row,
                               GError **err) {
  // set directory
  SET_DIR_OR_FAIL(tiff, tiffl->dir);

  if (tiffl->tile_read_direct) {
    // Fast path: read raw data, decode through libjpeg
    // Reading through tiff_read_region() reformats pixel data in three
    // passes: libjpeg converts from planar to R G B, libtiff converts
    // to BGRA, we convert to ARGB.  If we can bypass libtiff when
    // decoding JPEG tiles, we can reduce this to one optimized pass in
    // libjpeg-turbo.

    // read tables
    void *tables;
    uint32_t tables_len;
    if (!TIFFGetField(tiff, TIFFTAG_JPEGTABLES, &tables_len, &tables)) {
      // no separate tables
      tables = NULL;
      tables_len = 0;
    }

    // read data
    g_autofree void *buf = NULL;
    int32_t buflen;
    if (!_openslide_tiff_read_tile_data(tiffl, tiff,
                                        &buf, &buflen,
                                        tile_col, tile_row,
                                        err)) {
      return false;
    }

    // decompress
    return decode_jpeg(buf, buflen, tables, tables_len,
                       tiffl->photometric == PHOTOMETRIC_YCBCR ? JCS_YCbCr : JCS_RGB,
                       dest,
                       tiffl->tile_w, tiffl->tile_h,
                       err);
  } else {
    // Fallback: read tile through libtiff
    _openslide_performance_warn_once(&tiffl->warned_read_indirect,
                                     "Using slow libtiff read path for "
                                     "directory %d", tiffl->dir);
    return tiff_read_region(tiff, dest,
                            tile_col * tiffl->tile_w, tile_row * tiffl->tile_h,
                            tiffl->tile_w, tiffl->tile_h, err);
  }
}

bool _openslide_tiff_read_tile_data(struct _openslide_tiff_level *tiffl,
                                    TIFF *tiff,
                                    void **_buf, int32_t *_len,
                                    int64_t tile_col, int64_t tile_row,
                                    GError **err) {
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
    _openslide_tiff_error(err, tiff, "Cannot get tile size");
    return false;  // ok, haven't allocated anything yet
  }
  tsize_t tile_size = sizes[tile_no];

  // get raw tile
  g_autofree tdata_t buf = g_malloc(tile_size);
  tsize_t size = TIFFReadRawTile(tiff, tile_no, buf, tile_size);
  if (size == -1) {
    _openslide_tiff_error(err, tiff, "Cannot read raw tile");
    return false;
  }

  // set outputs
  *_buf = g_steal_pointer(&buf);
  *_len = size;
  return true;
}

// sets out-argument to indicate whether the tile data is zero bytes long
// returns false on error
bool _openslide_tiff_check_missing_tile(struct _openslide_tiff_level *tiffl,
                                        TIFF *tiff,
                                        int64_t tile_col, int64_t tile_row,
                                        bool *is_missing,
                                        GError **err) {
  // set directory
  if (!_openslide_tiff_set_dir(tiff, tiffl->dir, err)) {
    return false;
  }

  // get tile number
  ttile_t tile_no = TIFFComputeTile(tiff,
                                    tile_col * tiffl->tile_w,
                                    tile_row * tiffl->tile_h,
                                    0, 0);

  //g_debug("_openslide_tiff_check_missing_tile: tile %d", tile_no);

  // get tile size
  toff_t *sizes;
  if (!TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes)) {
    _openslide_tiff_error(err, tiff, "Cannot get tile size");
    return false;
  }
  tsize_t tile_size = sizes[tile_no];

  // return result
  *is_missing = tile_size == 0;
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
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, uint32_t, width);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, uint32_t, height);
  if (img->base.w != width || img->base.h != height) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unexpected associated image size: "
                "expected %"PRId64"x%"PRId64", got %"PRId64"x%"PRId64,
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
  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(img->tc, err);
  if (!ct.tiff) {
    return false;
  }
  return _get_associated_image_data(ct.tiff, img, dest, err);
}

static void destroy_associated_image(struct _openslide_associated_image *_img) {
  struct associated_image *img = (struct associated_image *) _img;

  g_free(img);
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
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, uint32_t, w);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, uint32_t, h);

  // check compression
  uint16_t compression;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_COMPRESSION, uint16_t, compression);
  if (!TIFFIsCODECConfigured(compression)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Unsupported TIFF compression: %u", compression);
    return false;
  }

  // load into struct
  struct associated_image *img = g_new0(struct associated_image, 1);
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
  g_auto(_openslide_cached_tiff) ct = _openslide_tiffcache_get(tc, err);
  bool ret = false;
  if (ct.tiff) {
    ret = _add_associated_image(osr, name, tc, dir, ct.tiff, err);
  }

  // safe even if successful
  g_prefix_error(err, "Can't read %s associated image: ", name);
  return ret;
}

bool _openslide_tiff_get_icc_profile_size(struct _openslide_tiff_level *tiffl,
                                          TIFF *tiff,
                                          int64_t *icc_profile_size,
                                          GError **err) {
  // set directory
  SET_DIR_OR_FAIL(tiff, tiffl->dir);

  // get profile
  void *data;
  uint32_t data_len;
  if (TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &data_len, &data)) {
    *icc_profile_size = data_len;
  } else {
    // 0 means no profile available
    *icc_profile_size = 0;
  }

  return true;
}

bool _openslide_tiff_read_icc_profile(openslide_t *osr,
                                      struct _openslide_tiff_level *tiffl,
                                      TIFF *tiff,
                                      void *dest,
                                      GError **err) {
  // set directory
  SET_DIR_OR_FAIL(tiff, tiffl->dir);

  // get profile
  uint32_t data_len;
  void *data;
  if (!TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &data_len, &data)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "No ICC profile");
    return false;
  }

  // copy to output
  if (data_len != osr->icc_profile_size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "ICC profile size changed");
    return false;
  }
  memcpy(dest, data, data_len);

  return true;
}

void _openslide_tiff_error(GError **err, TIFF *tiff, const char *fmt, ...) {
  struct tiff_file_handle *hdl = TIFFClientdata(tiff);

  g_autoptr(GString) str = g_string_new(NULL);
  va_list ap;
  va_start(ap, fmt);
  g_string_vprintf(str, fmt, ap);
  va_end(ap);

  if (hdl->last_error) {
    g_string_append_printf(str, ", last error: %s", hdl->last_error);
    g_free(g_steal_pointer(&hdl->last_error));
  }

  g_set_error_literal(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED, str->str);
}

static tsize_t tiff_do_read(thandle_t th, tdata_t buf, tsize_t size) {
  struct tiff_file_handle *hdl = th;

  // Open the file handle on first use.  cached_tiff_put will close it so it
  // doesn't stay open across API calls.  Also ensures FD_CLOEXEC is set.
  if (!hdl->f) {
    g_autoptr(_openslide_file) f = _openslide_fopen(hdl->tc->filename, NULL);
    if (!f) {
      return 0;
    }
    // restore file position
    if (!_openslide_fseek(f, hdl->offset, SEEK_SET, NULL)) {
      return 0;
    }
    hdl->f = g_steal_pointer(&f);
  }
  int64_t rsize = _openslide_fread(hdl->f, buf, size);
  hdl->offset += rsize;
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
  int64_t new_offset =
    _openslide_compute_seek(hdl->offset, hdl->size, offset, whence);
  if (hdl->f) {
    if (!_openslide_fseek(hdl->f, new_offset, SEEK_SET, NULL)) {
      return -1;
    }
  }
  hdl->offset = new_offset;
  return new_offset;
}

static void tiff_clear_error(struct tiff_file_handle *hdl) {
  if (hdl->last_error) {
    if (_openslide_debug(OPENSLIDE_DEBUG_DECODING)) {
      g_warning("libtiff uncaptured error: %s", hdl->last_error);
    }
    g_free(g_steal_pointer(&hdl->last_error));
  }
}

static int tiff_do_close(thandle_t th) {
  struct tiff_file_handle *hdl = th;

  tiff_clear_error(hdl);
  if (hdl->f) {
    _openslide_fclose(hdl->f);
  }
  g_free(hdl);
  return 0;
}

static toff_t tiff_do_size(thandle_t th) {
  struct tiff_file_handle *hdl = th;

  return hdl->size;
}

#ifdef HAVE_TIFF_LOG_CALLBACKS
static int tiff_do_warning(TIFF *tiff G_GNUC_UNUSED, void *data G_GNUC_UNUSED,
                           const char *module, const char *fmt, va_list ap) {
  if (_openslide_debug(OPENSLIDE_DEBUG_DECODING)) {
    g_autofree char *msg = g_strdup_vprintf(fmt, ap);
    g_warning("libtiff warning: %s: %s", module, msg);
  }
  // stop propagation
  return 1;
}

static int tiff_do_error(TIFF *tiff, void *data G_GNUC_UNUSED,
                         const char *module, const char *fmt, va_list ap) {
  struct tiff_file_handle *hdl = TIFFClientdata(tiff);

  tiff_clear_error(hdl);
  GString *str = g_string_new(NULL);
  g_string_printf(str, "%s: ", module);
  g_string_append_vprintf(str, fmt, ap);
  hdl->last_error = g_string_free(str, false);
  // stop propagation
  return 1;
}
#endif

static TIFF *tiff_open(struct _openslide_tiffcache *tc, GError **err) {
  // open
  g_autoptr(_openslide_file) f = _openslide_fopen(tc->filename, err);
  if (f == NULL) {
    return NULL;
  }

  // get size
  int64_t size = _openslide_fsize(f, err);
  if (size == -1) {
    g_prefix_error(err, "Couldn't get size of %s: ", tc->filename);
    return NULL;
  }

  // allocate
  struct tiff_file_handle *hdl = g_new0(struct tiff_file_handle, 1);
  hdl->tc = tc;
  hdl->f = g_steal_pointer(&f);
  hdl->size = size;

  // TIFFOpen
  // mode: m disables mmap to avoid sigbus and other mmap fragility
#ifdef HAVE_TIFF_LOG_CALLBACKS
  TIFFOpenOptions *opts = TIFFOpenOptionsAlloc();
  TIFFOpenOptionsSetWarningHandlerExtR(opts, tiff_do_warning, NULL);
  TIFFOpenOptionsSetErrorHandlerExtR(opts, tiff_do_error, NULL);
  TIFF *tiff = TIFFClientOpenExt(tc->filename, "rm", hdl,  // ci-allow
                                 tiff_do_read, tiff_do_write, tiff_do_seek,
                                 tiff_do_close, tiff_do_size, NULL, NULL, opts);
  TIFFOpenOptionsFree(opts);
#else
  TIFF *tiff = TIFFClientOpen(tc->filename, "rm", hdl,  // ci-allow
                              tiff_do_read, tiff_do_write, tiff_do_seek,
                              tiff_do_close, tiff_do_size, NULL, NULL);
#endif
  if (tiff == NULL) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Invalid TIFF: %s", tc->filename);
    tiff_do_close(hdl);
  }
  return tiff;
}

struct _openslide_tiffcache *_openslide_tiffcache_create(const char *filename) {
  struct _openslide_tiffcache *tc = g_new0(struct _openslide_tiffcache, 1);
  tc->filename = g_strdup(filename);
  tc->cache = g_queue_new();
  g_mutex_init(&tc->lock);
  return tc;
}

struct _openslide_cached_tiff _openslide_tiffcache_get(struct _openslide_tiffcache *tc,
                                                       GError **err) {
  //g_debug("get TIFF");
  g_mutex_lock(&tc->lock);
  tc->outstanding++;
  TIFF *tiff = g_queue_pop_head(tc->cache);
  g_mutex_unlock(&tc->lock);

  if (tiff == NULL) {
    //g_debug("create TIFF");
    // Does not check that we have the same file.  Then again, neither does
    // tiff_do_read.
    tiff = tiff_open(tc, err);
  }
  if (tiff == NULL) {
    g_mutex_lock(&tc->lock);
    tc->outstanding--;
    g_mutex_unlock(&tc->lock);
  }
  struct _openslide_cached_tiff ct = {
    .tiff = tiff,
  };
  return ct;
}

void _openslide_cached_tiff_put(struct _openslide_cached_tiff *ct) {
  if (ct == NULL || ct->tiff == NULL) {
    return;
  }
  g_autoptr(TIFF) tiff = ct->tiff;
  struct tiff_file_handle *hdl = TIFFClientdata(tiff);
  struct _openslide_tiffcache *tc = hdl->tc;

  //g_debug("put TIFF");
  g_mutex_lock(&tc->lock);
  g_assert(tc->outstanding);
  tc->outstanding--;
  if (g_queue_get_length(tc->cache) < HANDLE_CACHE_MAX) {
    tiff_clear_error(hdl);
    if (hdl->f) {
      _openslide_fclose(g_steal_pointer(&hdl->f));
    }
    g_queue_push_head(tc->cache, g_steal_pointer(&tiff));
  }
  g_mutex_unlock(&tc->lock);
}

void _openslide_tiffcache_destroy(struct _openslide_tiffcache *tc) {
  if (tc == NULL) {
    return;
  }
  g_mutex_lock(&tc->lock);
  TIFF *tiff;
  while ((tiff = g_queue_pop_head(tc->cache)) != NULL) {
    TIFFClose(tiff);
  }
  g_assert(tc->outstanding == 0);
  g_mutex_unlock(&tc->lock);
  g_queue_free(tc->cache);
  g_mutex_clear(&tc->lock);
  g_free(tc->filename);
  g_free(tc);
}
