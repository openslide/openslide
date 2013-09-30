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

#define SET_DIR_OR_FAIL(tiff, i, err)					\
  do {									\
    if (!TIFFSetDirectory(tiff, i)) {					\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,	\
                  "Cannot set TIFF directory %d", i);			\
      return false;							\
    }									\
  } while (0)

#define GET_FIELD_OR_FAIL(tiff, tag, result, err)			\
  do {									\
    uint32 tmp;								\
    if (!TIFFGetField(tiff, tag, &tmp)) {				\
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,	\
                  "Cannot get required TIFF tag: %d", tag);		\
      return false;							\
    }									\
    result = tmp;							\
  } while (0)

static const char *store_string_property(TIFF *tiff, openslide_t *osr,
					 const char *name, ttag_t tag) {
  char *value;
  if (TIFFGetFieldDefaulted(tiff, tag, &value)) {
    value = g_strdup(value);
    g_hash_table_insert(osr->properties, g_strdup(name), value);
    return value;
  }
  return NULL;
}

static void store_and_hash_string_property(TIFF *tiff, openslide_t *osr,
					   struct _openslide_hash *quickhash1,
					   const char *name, ttag_t tag) {
  _openslide_hash_string(quickhash1, name);
  _openslide_hash_string(quickhash1,
                         store_string_property(tiff, osr, name, tag));
}

static void store_float_property(TIFF *tiff, openslide_t *osr,
				 const char *name, ttag_t tag) {
  float value;
  if (TIFFGetFieldDefaulted(tiff, tag, &value)) {
    g_hash_table_insert(osr->properties,
                        g_strdup(name),
                        _openslide_format_double(value));
  }
}

static void store_and_hash_properties(TIFF *tiff, openslide_t *osr,
				      struct _openslide_hash *quickhash1) {
  // strings
  store_string_property(tiff, osr, OPENSLIDE_PROPERTY_NAME_COMMENT,
			TIFFTAG_IMAGEDESCRIPTION);

  // strings to store and hash
  store_and_hash_string_property(tiff, osr, quickhash1,
				 "tiff.ImageDescription", TIFFTAG_IMAGEDESCRIPTION);
  store_and_hash_string_property(tiff, osr, quickhash1,
				 "tiff.Make", TIFFTAG_MAKE);
  store_and_hash_string_property(tiff, osr, quickhash1,
				 "tiff.Model", TIFFTAG_MODEL);
  store_and_hash_string_property(tiff, osr, quickhash1,
				 "tiff.Software", TIFFTAG_SOFTWARE);
  store_and_hash_string_property(tiff, osr, quickhash1,
				 "tiff.DateTime", TIFFTAG_DATETIME);
  store_and_hash_string_property(tiff, osr, quickhash1,
				 "tiff.Artist", TIFFTAG_ARTIST);
  store_and_hash_string_property(tiff, osr, quickhash1,
				 "tiff.HostComputer", TIFFTAG_HOSTCOMPUTER);
  store_and_hash_string_property(tiff, osr, quickhash1,
				 "tiff.Copyright", TIFFTAG_COPYRIGHT);
  store_and_hash_string_property(tiff, osr, quickhash1,
				 "tiff.DocumentName", TIFFTAG_DOCUMENTNAME);


  // don't hash floats, they might be unstable over time
  store_float_property(tiff, osr, "tiff.XResolution", TIFFTAG_XRESOLUTION);
  store_float_property(tiff, osr, "tiff.YResolution", TIFFTAG_YRESOLUTION);
  store_float_property(tiff, osr, "tiff.XPosition", TIFFTAG_XPOSITION);
  store_float_property(tiff, osr, "tiff.YPosition", TIFFTAG_YPOSITION);

  // special
  uint16_t resolution_unit;
  if (TIFFGetFieldDefaulted(tiff, TIFFTAG_RESOLUTIONUNIT, &resolution_unit)) {
    const char *result;

    switch(resolution_unit) {
    case RESUNIT_NONE:
      result = "none";
      break;
    case RESUNIT_INCH:
      result = "inch";
      break;
    case RESUNIT_CENTIMETER:
      result = "centimeter";
      break;
    default:
      result = "unknown";
    }

    g_hash_table_insert(osr->properties,
                        g_strdup("tiff.ResolutionUnit"),
                        g_strdup(result));
  }
}

static bool hash_tiff_tiles(struct _openslide_hash *hash, TIFF *tiff,
                            GError **err) {
  g_assert(TIFFIsTiled(tiff));

  // get tile count
  ttile_t count = TIFFNumberOfTiles(tiff);

  // get tile sizes
  toff_t *sizes;
  if (TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes) == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot get tile size");
    return false;
  }
  toff_t total = 0;
  for (ttile_t tile_no = 0; tile_no < count; tile_no++) {
    total += sizes[tile_no];
    if (total > (5 << 20)) {
      // This is a non-pyramidal image or one with a very large top level.
      // Refuse to calculate a quickhash for it to keep openslide_open()
      // from taking an arbitrary amount of time.  (#79)
      _openslide_hash_disable(hash);
      return true;
    }
  }

  // get offsets
  toff_t *offsets;
  if (TIFFGetField(tiff, TIFFTAG_TILEOFFSETS, &offsets) == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Cannot get offsets");
    return false;
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

bool _openslide_tiff_init_properties_and_hash(openslide_t *osr,
                                              TIFF *tiff,
                                              struct _openslide_hash *quickhash1,
                                              tdir_t lowest_resolution_level,
                                              tdir_t property_dir,
                                              GError **err) {
  // generate hash of the smallest level
  SET_DIR_OR_FAIL(tiff, lowest_resolution_level, err);
  if (!hash_tiff_tiles(quickhash1, tiff, err)) {
    g_prefix_error(err, "Cannot hash TIFF tiles: ");
    return false;
  }

  // load TIFF properties
  SET_DIR_OR_FAIL(tiff, property_dir, err);
  store_and_hash_properties(tiff, osr, quickhash1);

  return true;
}

bool _openslide_tiff_level_init(TIFF *tiff,
                                tdir_t dir,
                                struct _openslide_level *level,
                                struct _openslide_tiff_level *tiffl,
                                GError **err) {
  // set the directory
  SET_DIR_OR_FAIL(tiff, dir, err);

  // figure out tile size
  int64_t tw, th;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_TILEWIDTH, tw, err);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_TILELENGTH, th, err);

  // get image size
  int64_t iw, ih;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, iw, err);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, ih, err);

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

bool _openslide_tiff_clip_tile(struct _openslide_tiff_level *tiffl,
                               uint32_t *tiledata,
                               int64_t tile_col, int64_t tile_row,
                               GError **err) {
  bool success = true;

  // get image dimensions
  int64_t iw = tiffl->image_w;
  int64_t ih = tiffl->image_h;

  // get tile dimensions
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // remaining w/h
  int64_t rw = iw - tile_col * tw;
  int64_t rh = ih - tile_row * th;

  if ((rw < tw) || (rh < th)) {
    // mask right/bottom
    cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
                                                                   CAIRO_FORMAT_ARGB32,
                                                                   tw, th,
                                                                   tw * 4);
    cairo_t *cr = cairo_create(surface);
    cairo_surface_destroy(surface);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);

    cairo_rectangle(cr, rw, 0, tw - rw, th);
    cairo_fill(cr);

    cairo_rectangle(cr, 0, rh, tw, th - rh);
    cairo_fill(cr);

    success = _openslide_check_cairo_status(cr, err);
    cairo_destroy(cr);
  }

  return success;
}

static bool tiff_read_region(TIFF *tiff,
                             uint32_t *dest,
                             int64_t x, int64_t y,
                             int32_t w, int32_t h,
                             GError **err) {
  TIFFRGBAImage img;
  char emsg[1024] = "";
  bool success = false;

  // init
  if (!TIFFRGBAImageOK(tiff, emsg)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Failure in TIFFRGBAImageOK: %s", emsg);
    return false;
  }
  if (!TIFFRGBAImageBegin(&img, tiff, 1, emsg)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
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
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "TIFFRGBAImageGet failed: %s", emsg);
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
  SET_DIR_OR_FAIL(tiff, tiffl->dir, err);

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
  SET_DIR_OR_FAIL(tiff, tiffl->dir, err);

  // get tile number
  ttile_t tile_no = TIFFComputeTile(tiff,
                                    tile_col * tiffl->tile_w,
                                    tile_row * tiffl->tile_h,
                                    0, 0);

  //g_debug("_openslide_tiff_read_tile_data reading tile %d", tile_no);

  // get tile size
  toff_t *sizes;
  if (TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes) == 0) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
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
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
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

  SET_DIR_OR_FAIL(tiff, img->directory, err);

  // ensure dimensions have not changed
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, width, err);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, height, err);
  if (img->base.w != width || img->base.h != height) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
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
  SET_DIR_OR_FAIL(tiff, dir, err);

  // get the dimensions
  int64_t w, h;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGEWIDTH, w, err);
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_IMAGELENGTH, h, err);

  // check compression
  uint16_t compression;
  GET_FIELD_OR_FAIL(tiff, TIFFTAG_COMPRESSION, compression, err);
  if (!TIFFIsCODECConfigured(compression)) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
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
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
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
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
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
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,
                "Invalid TIFF: %s", tc->filename);
    tiff_do_close(hdl);
  }
  return tiff;

NOT_TIFF:
  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
              "Not a TIFF file: %s", tc->filename);
  return NULL;
}

struct _openslide_tiffcache *_openslide_tiffcache_create(const char *filename,
                                                         GError **err) {
  struct _openslide_tiffcache *tc = g_slice_new0(struct _openslide_tiffcache);
  tc->filename = g_strdup(filename);
  tc->cache = g_queue_new();
  tc->lock = g_mutex_new();

  // verify TIFF is valid
  TIFF *tiff = _openslide_tiffcache_get(tc, err);
  _openslide_tiffcache_put(tc, tiff);
  if (tiff == NULL) {
    // nope
    _openslide_tiffcache_destroy(tc);
    tc = NULL;
  }
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
