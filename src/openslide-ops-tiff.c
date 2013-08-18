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

#include <glib.h>
#include <tiffio.h>

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cairo.h>

#include "openslide-cache.h"
#include "openslide-hash.h"

#define HANDLE_CACHE_MAX 32

struct _openslide_tiffcache {
  char *filename;
  GQueue *cache;
  GMutex *lock;
};

// not thread-safe, like libtiff
struct tiff_file_handle {
  char *filename;
  int64_t offset;
  int64_t size;
};

struct _openslide_tiffopsdata {
  struct _openslide_tiffcache *tc;
};

struct level {
  struct _openslide_level base;
  struct _openslide_tiff_level tiffl;
  struct _openslide_grid *grid;
};

struct tiff_associated_image {
  struct _openslide_associated_image base;
  tdir_t directory;
};

#define SET_DIR_OR_FAIL(osr, tiff, i)				\
  if (!TIFFSetDirectory(tiff, i)) {				\
    _openslide_set_error(osr, "Cannot set TIFF directory");	\
    return;							\
  }

#define GET_FIELD_OR_FAIL(osr, tiff, tag, result)		\
  if (!TIFFGetField(tiff, tag, &tmp)) {				\
    _openslide_set_error(osr, "Cannot get required TIFF tag: %d", tag);	\
    return;							\
  }								\
  result = tmp;

#define SET_DIR_OR_ERR(tiff, i, err)				\
  if (!TIFFSetDirectory(tiff, i)) {				\
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,	\
                "Cannot set TIFF directory %d", i);		\
    return false;						\
  }

#define GET_FIELD_OR_ERR(tiff, tag, result, err)		\
  if (!TIFFGetField(tiff, tag, &tmp)) {				\
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_BAD_DATA,	\
                "Cannot get required TIFF tag: %d", tag);	\
    return false;						\
  }								\
  result = tmp;

static const char *store_string_property(TIFF *tiff, GHashTable *ht,
					 const char *name, ttag_t tag) {
  char *value;
  if (TIFFGetFieldDefaulted(tiff, tag, &value)) {
    value = g_strdup(value);
    g_hash_table_insert(ht, g_strdup(name), value);
    return value;
  }
  return NULL;
}

static void store_and_hash_string_property(TIFF *tiff, GHashTable *ht,
					   struct _openslide_hash *quickhash1,
					   const char *name, ttag_t tag) {
  _openslide_hash_string(quickhash1, name);
  _openslide_hash_string(quickhash1, store_string_property(tiff, ht, name, tag));
}

static void store_float_property(TIFF *tiff, GHashTable *ht,
				 const char *name, ttag_t tag) {
  float value;
  if (TIFFGetFieldDefaulted(tiff, tag, &value)) {
    g_hash_table_insert(ht, g_strdup(name), _openslide_format_double(value));
  }
}

static void store_and_hash_properties(TIFF *tiff, GHashTable *ht,
				      struct _openslide_hash *quickhash1) {
  // strings
  store_string_property(tiff, ht, OPENSLIDE_PROPERTY_NAME_COMMENT,
			TIFFTAG_IMAGEDESCRIPTION);

  // strings to store and hash
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.ImageDescription", TIFFTAG_IMAGEDESCRIPTION);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Make", TIFFTAG_MAKE);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Model", TIFFTAG_MODEL);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Software", TIFFTAG_SOFTWARE);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.DateTime", TIFFTAG_DATETIME);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Artist", TIFFTAG_ARTIST);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.HostComputer", TIFFTAG_HOSTCOMPUTER);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.Copyright", TIFFTAG_COPYRIGHT);
  store_and_hash_string_property(tiff, ht, quickhash1,
				 "tiff.DocumentName", TIFFTAG_DOCUMENTNAME);


  // don't hash floats, they might be unstable over time
  store_float_property(tiff, ht, "tiff.XResolution", TIFFTAG_XRESOLUTION);
  store_float_property(tiff, ht, "tiff.YResolution", TIFFTAG_YRESOLUTION);
  store_float_property(tiff, ht, "tiff.XPosition", TIFFTAG_XPOSITION);
  store_float_property(tiff, ht, "tiff.YPosition", TIFFTAG_YPOSITION);

  // special
  uint16_t resolution_unit;
  if (TIFFGetFieldDefaulted(tiff, TIFFTAG_RESOLUTIONUNIT, &resolution_unit)) {
    const char *result;

    switch(resolution_unit) {
    case 1:
      result = "none";
      break;
    case 2:
      result = "inch";
      break;
    case 3:
      result = "centimeter";
      break;
    default:
      result = "unknown";
    }

    g_hash_table_insert(ht, g_strdup("tiff.ResolutionUnit"), g_strdup(result));
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
  if (osr == NULL) {
    return true;
  }

  // generate hash of the smallest level
  SET_DIR_OR_ERR(tiff, lowest_resolution_level, err)
  if (!hash_tiff_tiles(quickhash1, tiff, err)) {
    g_prefix_error(err, "Cannot hash TIFF tiles: ");
    return false;
  }

  // load TIFF properties
  SET_DIR_OR_ERR(tiff, property_dir, err)
  store_and_hash_properties(tiff, osr->properties, quickhash1);

  return true;
}

static void destroy_data(struct _openslide_tiffopsdata *data,
                         struct level **levels, int32_t level_count) {
  _openslide_tiffcache_destroy(data->tc);
  g_slice_free(struct _openslide_tiffopsdata, data);

  for (int32_t i = 0; i < level_count; i++) {
    _openslide_grid_destroy(levels[i]->grid);
    g_slice_free(struct level, levels[i]);
  }
  g_free(levels);
}

static void destroy(openslide_t *osr) {
  struct _openslide_tiffopsdata *data = osr->data;
  struct level **levels = (struct level **) osr->levels;
  destroy_data(data, levels, osr->level_count);
}


bool _openslide_tiff_level_init(TIFF *tiff,
                                tdir_t dir,
                                struct _openslide_level *level,
                                struct _openslide_tiff_level *tiffl,
                                GError **err) {
  uint32_t tmp;

  // set the directory
  SET_DIR_OR_ERR(tiff, dir, err)

  // figure out tile size
  int64_t tw, th;
  GET_FIELD_OR_ERR(tiff, TIFFTAG_TILEWIDTH, tw, err)
  GET_FIELD_OR_ERR(tiff, TIFFTAG_TILELENGTH, th, err)

  // get image size
  int64_t iw, ih;
  GET_FIELD_OR_ERR(tiff, TIFFTAG_IMAGEWIDTH, iw, err)
  GET_FIELD_OR_ERR(tiff, TIFFTAG_IMAGELENGTH, ih, err)

  // safe now, start writing
  level->w = iw;
  level->h = ih;
  // tile size hints
  level->tile_w = tw;
  level->tile_h = th;

  tiffl->dir = dir;
  tiffl->image_w = iw;
  tiffl->image_h = ih;
  tiffl->tile_w = tw;
  tiffl->tile_h = th;

  // num tiles in each dimension
  tiffl->tiles_across = (iw / tw) + !!(iw % tw);   // integer ceiling
  tiffl->tiles_down = (ih / th) + !!(ih % th);

  return true;
}

void _openslide_tiff_clip_tile(openslide_t *osr,
                               struct _openslide_tiff_level *tiffl,
                               uint32_t *tiledata,
                               int64_t tile_col, int64_t tile_row) {
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

    _openslide_check_cairo_status_possibly_set_error(osr, cr);
    cairo_destroy(cr);
  }
}

static void read_tile(openslide_t *osr,
		      cairo_t *cr,
		      struct _openslide_level *level,
		      struct _openslide_grid *grid,
		      int64_t tile_col, int64_t tile_row,
		      void *arg) {
  struct level *l = (struct level *) level;
  struct _openslide_tiff_level *tiffl = &l->tiffl;
  TIFF *tiff = arg;

  // tile size
  int64_t tw = tiffl->tile_w;
  int64_t th = tiffl->tile_h;

  // cache
  struct _openslide_cache_entry *cache_entry;
  uint32_t *tiledata = _openslide_cache_get(osr->cache,
                                            tile_col, tile_row, grid,
                                            &cache_entry);
  if (!tiledata) {
    tiledata = g_slice_alloc(tw * th * 4);
    _openslide_tiff_read_tile(osr, tiffl, tiff, tiledata, tile_col, tile_row);

    // clip, if necessary
    _openslide_tiff_clip_tile(osr, tiffl, tiledata, tile_col, tile_row);

    // put it in the cache
    _openslide_cache_put(osr->cache, tile_col, tile_row, grid,
			 tiledata, tw * th * 4,
			 &cache_entry);
  }

  // draw it
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *) tiledata,
								 CAIRO_FORMAT_ARGB32,
								 tw, th,
								 tw * 4);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_surface_destroy(surface);
  cairo_paint(cr);

  //_openslide_grid_label_tile(grid, cr, tile_col, tile_row);

  // done with the cache entry, release it
  _openslide_cache_entry_unref(cache_entry);
}

static void paint_region(openslide_t *osr, cairo_t *cr,
			 int64_t x, int64_t y,
			 struct _openslide_level *level,
			 int32_t w, int32_t h) {
  struct _openslide_tiffopsdata *data = osr->data;
  struct level *l = (struct level *) level;

  TIFF *tiff = _openslide_tiffcache_get(data->tc);
  if (tiff) {
    if (TIFFSetDirectory(tiff, l->tiffl.dir)) {
      _openslide_grid_paint_region(l->grid, cr, tiff,
                                   x / l->base.downsample,
                                   y / l->base.downsample,
                                   level, w, h);
    } else {
      _openslide_set_error(osr, "Cannot set TIFF directory");
    }
  } else {
    _openslide_set_error(osr, "Cannot open TIFF file");
  }
  _openslide_tiffcache_put(data->tc, tiff);
}


static const struct _openslide_ops _openslide_tiff_ops = {
  .paint_region = paint_region,
  .destroy = destroy,
};

void _openslide_add_tiff_ops(openslide_t *osr,
			     TIFF *tiff,
			     int32_t property_dir,
			     int32_t level_count,
			     int32_t *directories,
			     struct _openslide_hash *quickhash1) {
  // allocate private data
  struct _openslide_tiffopsdata *data =
    g_slice_new0(struct _openslide_tiffopsdata);

  GError *tmp_err = NULL;

  // create levels
  struct level **levels = g_new(struct level *, level_count);
  for (int32_t i = 0; i < level_count; i++) {
    struct level *l = g_slice_new0(struct level);
    struct _openslide_tiff_level *tiffl = &l->tiffl;
    levels[i] = l;

    if (!_openslide_tiff_level_init(tiff,
                                    directories[i],
                                    (struct _openslide_level *) l,
                                    tiffl,
                                    &tmp_err)) {
      _openslide_set_error_from_gerror(osr, tmp_err);
      g_clear_error(&tmp_err);
      destroy_data(data, levels, level_count);
      return;
    }

    l->grid = _openslide_grid_create_simple(osr,
                                            tiffl->tiles_across,
                                            tiffl->tiles_down,
                                            tiffl->tile_w,
                                            tiffl->tile_h,
                                            read_tile);
  }
  g_free(directories);

  if (osr == NULL) {
    // free now and return
    TIFFClose(tiff);
    destroy_data(data, levels, level_count);
    return;
  }

  // generate hash of the smallest level
  if (!_openslide_tiff_init_properties_and_hash(osr, tiff, quickhash1,
                                                levels[level_count - 1]->tiffl.dir,
                                                property_dir,
                                                &tmp_err)) {
    _openslide_set_error_from_gerror(osr, tmp_err);
    g_clear_error(&tmp_err);
  }

  // store tiff-specific data into osr
  g_assert(osr->data == NULL);
  g_assert(osr->levels == NULL);

  // general osr data
  osr->levels = (struct _openslide_level **) levels;
  osr->level_count = level_count;
  osr->data = data;
  osr->ops = &_openslide_tiff_ops;

  // create TIFF cache from handle
  data->tc = _openslide_tiffcache_create(tiff);
}

static void tiff_read_region(openslide_t *osr, TIFF *tiff,
                             uint32_t *dest,
                             int64_t x, int64_t y,
                             int32_t w, int32_t h) {
  TIFFRGBAImage img;
  char emsg[1024] = "";

  // init
  if (!TIFFRGBAImageOK(tiff, emsg)) {
    _openslide_set_error(osr, "Failure in TIFFRGBAImageOK: %s", emsg);
    return;
  }
  if (!TIFFRGBAImageBegin(&img, tiff, 1, emsg)) {
    _openslide_set_error(osr, "Failure in TIFFRGBAImageBegin: %s", emsg);
    return;
  }
  img.req_orientation = ORIENTATION_TOPLEFT;
  img.col_offset = x;
  img.row_offset = y;

  // draw it
  if (TIFFRGBAImageGet(&img, dest, w, h)) {
    // permute
    uint32_t *p = dest;
    uint32_t *end = dest + w * h;
    while (p < end) {
      uint32_t val = *p;
      *p++ = (val & 0xFF00FF00)
	| ((val << 16) & 0xFF0000)
	| ((val >> 16) & 0xFF);
    }
  } else {
    _openslide_set_error(osr, "TIFFRGBAImageGet failed: %s", emsg);
    memset(dest, 0, w * h * 4);
  }

  // done
  TIFFRGBAImageEnd(&img);
}

void _openslide_tiff_read_tile(openslide_t *osr,
                               struct _openslide_tiff_level *tiffl,
                               TIFF *tiff,
                               uint32_t *dest,
                               int64_t tile_col, int64_t tile_row) {
  // set directory
  SET_DIR_OR_FAIL(osr, tiff, tiffl->dir);

  // read tile
  tiff_read_region(osr, tiff, dest,
                   tile_col * tiffl->tile_w, tile_row * tiffl->tile_h,
                   tiffl->tile_w, tiffl->tile_h);
}

void _openslide_tiff_read_tile_data(openslide_t *osr,
                                    struct _openslide_tiff_level *tiffl,
                                    TIFF *tiff,
                                    void **_buf, int32_t *_len,
                                    int64_t tile_col, int64_t tile_row) {
  // initialize out params
  *_buf = NULL;
  *_len = 0;

  // set directory
  SET_DIR_OR_FAIL(osr, tiff, tiffl->dir);

  // get tile number
  ttile_t tile_no = TIFFComputeTile(tiff,
                                    tile_col * tiffl->tile_w,
                                    tile_row * tiffl->tile_h,
                                    0, 0);

  //g_debug("_openslide_tiff_read_tile_data reading tile %d", tile_no);

  // get tile size
  toff_t *sizes;
  if (TIFFGetField(tiff, TIFFTAG_TILEBYTECOUNTS, &sizes) == 0) {
    _openslide_set_error(osr, "Cannot get tile size");
    return;  // ok, haven't allocated anything yet
  }
  tsize_t tile_size = sizes[tile_no];

  // a slide with zero-length tiles has been seen in the wild
  if (!tile_size) {
    //g_debug("no data for tile %d", tile_no);
    return;  // ok, haven't allocated anything yet
  }

  // get raw tile
  tdata_t buf = g_malloc(tile_size);
  tsize_t size = TIFFReadRawTile(tiff, tile_no, buf, tile_size);
  if (size == -1) {
    _openslide_set_error(osr, "Cannot read raw tile");
    g_free(buf);
    return;
  }

  // set outputs
  *_buf = buf;
  *_len = size;
}

static void _tiff_get_associated_image_data(openslide_t *osr, TIFF *tiff,
                                            struct _openslide_associated_image *_img,
                                            uint32_t *dest) {
  struct tiff_associated_image *img = (struct tiff_associated_image *) _img;
  uint32_t tmp;
  int64_t width, height;

  // g_debug("read TIFF associated image: %d", img->directory);

  SET_DIR_OR_FAIL(osr, tiff, img->directory);

  // ensure dimensions have not changed
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_IMAGEWIDTH, width);
  GET_FIELD_OR_FAIL(osr, tiff, TIFFTAG_IMAGELENGTH, height);
  if (img->base.w != width || img->base.h != height) {
    _openslide_set_error(osr, "Unexpected associated image size");
    return;
  }

  // load the image
  tiff_read_region(osr, tiff, dest, 0, 0, width, height);
}

static void tiff_get_associated_image_data(openslide_t *osr,
                                           struct _openslide_associated_image *img,
                                           uint32_t *dest) {
  struct _openslide_tiffopsdata *data = osr->data;
  TIFF *tiff = _openslide_tiffcache_get(data->tc);
  if (tiff) {
    _tiff_get_associated_image_data(osr, tiff, img, dest);
  } else {
    _openslide_set_error(osr, "Cannot open TIFF file");
  }
  _openslide_tiffcache_put(data->tc, tiff);
}

static void tiff_destroy_associated_image(struct _openslide_associated_image *_img) {
  struct tiff_associated_image *img = (struct tiff_associated_image *) _img;

  g_slice_free(struct tiff_associated_image, img);
}

static const struct _openslide_associated_image_ops tiff_associated_ops = {
  .get_argb_data = tiff_get_associated_image_data,
  .destroy = tiff_destroy_associated_image,
};

bool _openslide_add_tiff_associated_image(GHashTable *ht,
					  const char *name,
					  TIFF *tiff,
					  GError **err) {
  uint32_t tmp;

  // get the dimensions
  int64_t w, h;
  GET_FIELD_OR_ERR(tiff, TIFFTAG_IMAGEWIDTH, w, err)
  GET_FIELD_OR_ERR(tiff, TIFFTAG_IMAGELENGTH, h, err)

  // possibly load into struct
  if (ht) {
    struct tiff_associated_image *img =
      g_slice_new0(struct tiff_associated_image);
    img->base.ops = &tiff_associated_ops;
    img->base.w = w;
    img->base.h = h;
    img->directory = TIFFCurrentDirectory(tiff);

    // save
    g_hash_table_insert(ht, g_strdup(name), img);
  }

  return true;
}

static tsize_t tiff_do_read(thandle_t th, tdata_t buf, tsize_t size) {
  struct tiff_file_handle *hdl = th;

  // don't leave the file handle open between calls
  // also ensures FD_CLOEXEC is set
  FILE *f = _openslide_fopen(hdl->filename, "rb", NULL);
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

  g_free(hdl->filename);
  g_slice_free(struct tiff_file_handle, hdl);
  return 0;
}

static toff_t tiff_do_size(thandle_t th) {
  struct tiff_file_handle *hdl = th;

  return hdl->size;
}

TIFF *_openslide_tiff_open(const char *filename) {
  // open
  FILE *f = _openslide_fopen(filename, "rb", NULL);
  if (f == NULL) {
    return NULL;
  }

  // read magic
  uint8_t buf[4];
  if (fread(buf, 4, 1, f) != 1) {
    // can't read
    fclose(f);
    return NULL;
  }

  // get size
  if (fseeko(f, 0, SEEK_END) == -1) {
    fclose(f);
    return NULL;
  }
  int64_t size = ftello(f);
  fclose(f);
  if (size == -1) {
    return NULL;
  }

  // check magic
  // TODO: remove if libtiff gets private error/warning callbacks
  if (buf[0] != buf[1]) {
    return NULL;
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
    return NULL;
  }
  // only accept BigTIFF on libtiff >= 4
  if (!(version == 42 || (sizeof(toff_t) > 4 && version == 43))) {
    return NULL;
  }

  // allocate
  struct tiff_file_handle *hdl = g_slice_new0(struct tiff_file_handle);
  hdl->filename = g_strdup(filename);
  hdl->size = size;

  // TIFFOpen
  // mode: m disables mmap to avoid sigbus and other mmap fragility
  TIFF *tiff = TIFFClientOpen(filename, "rm", hdl,
                              tiff_do_read, tiff_do_write, tiff_do_seek,
                              tiff_do_close, tiff_do_size, NULL, NULL);
  if (tiff == NULL) {
    tiff_do_close(hdl);
  }
  return tiff;
}

struct _openslide_tiffcache *_openslide_tiffcache_create(TIFF *tiff) {
  struct _openslide_tiffcache *tc = g_slice_new0(struct _openslide_tiffcache);
  tc->filename = g_strdup(TIFFFileName(tiff));
  tc->cache = g_queue_new();
  tc->lock = g_mutex_new();
  _openslide_tiffcache_put(tc, tiff);
  return tc;
}

TIFF *_openslide_tiffcache_get(struct _openslide_tiffcache *tc) {
  //g_debug("get TIFF");
  g_mutex_lock(tc->lock);
  TIFF *tiff = g_queue_pop_head(tc->cache);
  g_mutex_unlock(tc->lock);

  if (tiff == NULL) {
    //g_debug("create TIFF");
    // Does not check that we have the same file.  Then again, neither does
    // tiff_do_read.
    tiff = _openslide_tiff_open(tc->filename);
  }
  return tiff;
}

void _openslide_tiffcache_put(struct _openslide_tiffcache *tc, TIFF *tiff) {
  if (tiff == NULL) {
    return;
  }

  //g_debug("put TIFF");
  g_mutex_lock(tc->lock);
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
  g_mutex_unlock(tc->lock);
  g_queue_free(tc->cache);
  g_mutex_free(tc->lock);
  g_free(tc->filename);
  g_slice_free(struct _openslide_tiffcache, tc);
}
