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

#ifndef OPENSLIDE_OPENSLIDE_PRIVATE_H_
#define OPENSLIDE_OPENSLIDE_PRIVATE_H_

#ifdef _WIN32
#define WIN32 1
#endif

#include <config.h>

#include "openslide.h"
#include "openslide-hash.h"

#include <glib.h>
#include <stdio.h>
#include <stdbool.h>
#include <setjmp.h>
#include <tiffio.h>

// jconfig.h redefines HAVE_STDLIB_H if libjpeg was not built with Autoconf
#undef HAVE_STDLIB_H
#include <jpeglib.h>
#undef HAVE_STDLIB_H
#include <config.h>  // again

#include <cairo.h>

#include <openjpeg.h>


/* the associated image structure */
struct _openslide_associated_image {
  const struct _openslide_associated_image_ops *ops;

  int64_t w;
  int64_t h;
};

/* associated image operations */
struct _openslide_associated_image_ops {
  // must fail if stored width or height doesn't match the image
  bool (*get_argb_data)(struct _openslide_associated_image *img,
                        uint32_t *dest,
                        GError **err);
  void (*destroy)(struct _openslide_associated_image *img);
};

/* the main structure */
struct _openslide {
  const struct _openslide_ops *ops;
  struct _openslide_level **levels;
  void *data;
  int32_t level_count;

  // associated images
  GHashTable *associated_images;  // created automatically
  const char **associated_image_names; // filled in automatically from hashtable

  // metadata
  GHashTable *properties; // created automatically
  const char **property_names; // filled in automatically from hashtable

  // cache
  struct _openslide_cache *cache;

  // error handling, NULL if no error
  gpointer error; // must use g_atomic_pointer!
};

struct _openslide_level {
  double downsample;  // zero value is filled in automatically from dimensions

  int64_t w;
  int64_t h;

  // only for tile geometry properties; 0 to omit.
  // all levels must set these, or none
  int64_t tile_w;
  int64_t tile_h;
};

/* the function pointer structure for backends */
struct _openslide_ops {
  void (*paint_region)(openslide_t *osr, cairo_t *cr,
		       int64_t x, int64_t y,
		       struct _openslide_level *level,
		       int32_t w, int32_t h);
  void (*destroy)(openslide_t *osr);
};

struct _openslide_tiffcache;

/* vendor detection and parsing */
typedef bool (*_openslide_vendor_fn)(openslide_t *osr, const char *filename,
				     struct _openslide_hash *quickhash1,
				     GError **err);
typedef bool (*_openslide_tiff_vendor_fn)(openslide_t *osr,
					  struct _openslide_tiffcache *tc,
					  TIFF *tiff,
					  struct _openslide_hash *quickhash1,
					  GError **err);
/*
 * A note on quickhash1: this should be a hash of data that
 * will not change with revisions to the openslide library. It should
 * also be quick to generate. It should be a way to uniquely identify
 * a particular slide by content, but does not need to be sensitive
 * to file corruption.
 *
 * It is called "quickhash1" so that we can create a "quickhash2" if needed.
 * The hash is stored in a property, it is expected that we will store
 * more hash properties if needed.
 *
 * Suggested data to hash:
 * easily available image metadata + raw compressed lowest resolution image
 */


bool _openslide_try_trestle(openslide_t *osr,
			    struct _openslide_tiffcache *tc, TIFF *tiff,
			    struct _openslide_hash *quickhash1, GError **err);
bool _openslide_try_aperio(openslide_t *osr,
                           struct _openslide_tiffcache *tc, TIFF *tiff,
			   struct _openslide_hash *quickhash1, GError **err);
bool _openslide_try_hamamatsu(openslide_t *osr, const char* filename,
			      struct _openslide_hash *quickhash1,
			      GError **err);
bool _openslide_try_hamamatsu_ndpi(openslide_t *osr, const char* filename,
				   struct _openslide_hash *quickhash1,
				   GError **err);
bool _openslide_try_mirax(openslide_t *osr, const char* filename,
			  struct _openslide_hash *quickhash1, GError **err);
bool _openslide_try_leica(openslide_t *osr,
                          struct _openslide_tiffcache *tc, TIFF *tiff,
                          struct _openslide_hash *quickhash1,
                          GError **err);
bool _openslide_try_generic_tiff(openslide_t *osr,
				 struct _openslide_tiffcache *tc, TIFF *tiff,
				 struct _openslide_hash *quickhash1,
				 GError **err);


/* GHashTable utils */
guint _openslide_int64_hash(gconstpointer v);
gboolean _openslide_int64_equal(gconstpointer v1, gconstpointer v2);
void _openslide_int64_free(gpointer data);

/* g_key_file_load_from_file wrapper */
gboolean _openslide_read_key_file(GKeyFile *key_file, const char *filename,
                                  GKeyFileFlags flags, GError **err);

/* fopen() wrapper which properly sets FD_CLOEXEC */
FILE *_openslide_fopen(const char *path, const char *mode, GError **err);

/* Returns the size of the file */
int64_t _openslide_fsize(const char *path, GError **err);

/* Serialize double to string */
char *_openslide_format_double(double d);

/* Duplicate OpenSlide properties */
void _openslide_duplicate_int_prop(GHashTable *ht, const char *src,
                                   const char *dest);
void _openslide_duplicate_double_prop(GHashTable *ht, const char *src,
                                      const char *dest);

// background color helper
void _openslide_set_background_color_prop(GHashTable *ht,
                                          uint8_t r, uint8_t g, uint8_t b);


// Grid helpers
struct _openslide_grid;

typedef void (*_openslide_tileread_fn)(openslide_t *osr,
                                       cairo_t *cr,
                                       struct _openslide_level *level,
                                       struct _openslide_grid *grid,
                                       int64_t tile_col, int64_t tile_row,
                                       void *arg);

typedef void (*_openslide_tilemap_fn)(openslide_t *osr,
                                      cairo_t *cr,
                                      struct _openslide_level *level,
                                      struct _openslide_grid *grid,
                                      int64_t tile_col, int64_t tile_row,
                                      void *tile,
                                      void *arg);

typedef void (*_openslide_tilemap_foreach_fn)(struct _openslide_grid *grid,
                                              int64_t tile_col,
                                              int64_t tile_row,
                                              void *tile,
                                              void *arg);

struct _openslide_grid *_openslide_grid_create_simple(openslide_t *osr,
                                                      int64_t tiles_across,
                                                      int64_t tiles_down,
                                                      int32_t tile_w,
                                                      int32_t tile_h,
                                                      _openslide_tileread_fn read_tile);

struct _openslide_grid *_openslide_grid_create_tilemap(openslide_t *osr,
                                                       double tile_advance_x,
                                                       double tile_advance_y,
                                                       _openslide_tilemap_fn read_tile,
                                                       GDestroyNotify destroy_tile);

void _openslide_grid_tilemap_add_tile(struct _openslide_grid *grid,
                                      int64_t col, int64_t row,
                                      double offset_x, double offset_y,
                                      double w, double h,
                                      void *data);

void _openslide_grid_tilemap_foreach(struct _openslide_grid *grid,
                                     _openslide_tilemap_foreach_fn func,
                                     void *arg);

void _openslide_grid_get_bounds(struct _openslide_grid *grid,
                                double *x, double *y,
                                double *w, double *h);

void _openslide_grid_paint_region(struct _openslide_grid *grid,
                                  cairo_t *cr,
                                  void *arg,
                                  double x, double y,
                                  struct _openslide_level *level,
                                  int32_t w, int32_t h);

void _openslide_grid_destroy(struct _openslide_grid *grid);

void _openslide_grid_label_tile(struct _openslide_grid *grid,
                                cairo_t *cr,
                                int64_t tile_col, int64_t tile_row);

/* TIFF support */
struct _openslide_tiff_level {
  tdir_t dir;
  int64_t image_w;
  int64_t image_h;
  int64_t tile_w;
  int64_t tile_h;
  int64_t tiles_across;
  int64_t tiles_down;
};

bool _openslide_tiff_level_init(TIFF *tiff,
                                tdir_t dir,
                                struct _openslide_level *level,
                                struct _openslide_tiff_level *tiffl,
                                GError **err);

bool _openslide_tiff_init_properties_and_hash(openslide_t *osr,
                                              TIFF *tiff,
                                              struct _openslide_hash *quickhash1,
                                              tdir_t lowest_resolution_level,
                                              tdir_t property_dir,
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

void _openslide_tiff_clip_tile(openslide_t *osr,
                               struct _openslide_tiff_level *tiffl,
                               uint32_t *tiledata,
                               int64_t tile_col, int64_t tile_row);

bool _openslide_tiff_add_associated_image(openslide_t *osr,
					  const char *name,
					  struct _openslide_tiffcache *tc,
					  tdir_t dir,
					  GError **err);

struct _openslide_tiffcache *_openslide_tiffcache_create(const char *filename,
                                                         GError **err);

TIFF *_openslide_tiffcache_get(struct _openslide_tiffcache *tc, GError **err);

void _openslide_tiffcache_put(struct _openslide_tiffcache *tc, TIFF *tiff);

void _openslide_tiffcache_destroy(struct _openslide_tiffcache *tc);

/* JPEG support */
bool _openslide_jpeg_read_dimensions(const char *filename,
                                     int64_t offset,
                                     int32_t *w, int32_t *h,
                                     GError **err);

bool _openslide_jpeg_read(const char *filename,
                          int64_t offset,
                          uint32_t *dest,
                          int32_t w, int32_t h,
                          GError **err);

bool _openslide_jpeg_add_associated_image(openslide_t *osr,
					  const char *name,
					  const char *filename,
					  int64_t offset,
					  GError **err);

/*
 * On Windows, we cannot fopen a file and pass it to another DLL that does fread.
 * So we need to compile all our freading into the OpenSlide DLL directly.
 */
void _openslide_jpeg_stdio_src(j_decompress_ptr cinfo, FILE *infile);

// error function for libjpeg
struct _openslide_jpeg_error_mgr {
  struct jpeg_error_mgr pub;      // public fields

  jmp_buf *env;
  GError *err;
};

struct jpeg_error_mgr *_openslide_jpeg_set_error_handler(struct _openslide_jpeg_error_mgr *jerr,
							 jmp_buf *env);

/* JPEG 2000 support */
enum _openslide_jp2k_colorspace {
  OPENSLIDE_JP2K_RGB,
  OPENSLIDE_JP2K_YCBCR,
};

bool _openslide_jp2k_decode_buffer(uint32_t *dest,
                                   int32_t w, int32_t h,
                                   void *data, int32_t datalen,
                                   enum _openslide_jp2k_colorspace space,
                                   GError **err);

/* Cache */
#define _OPENSLIDE_USEFUL_CACHE_SIZE 1024*1024*32

struct _openslide_cache_entry;

// constructor/destructor
struct _openslide_cache *_openslide_cache_create(int capacity_in_bytes);

void _openslide_cache_destroy(struct _openslide_cache *cache);

// cache size
int _openslide_cache_get_capacity(struct _openslide_cache *cache);

void _openslide_cache_set_capacity(struct _openslide_cache *cache,
				   int capacity_in_bytes);

// put and get
void _openslide_cache_put(struct _openslide_cache *cache,
			  int64_t x,
			  int64_t y,
			  struct _openslide_grid *grid,
			  void *data,
			  int size_in_bytes,
			  struct _openslide_cache_entry **entry);

void *_openslide_cache_get(struct _openslide_cache *cache,
			   int64_t x,
			   int64_t y,
			   struct _openslide_grid *grid,
			   struct _openslide_cache_entry **entry);

// value unref
void _openslide_cache_entry_unref(struct _openslide_cache_entry *entry);


// external error propagation
bool _openslide_set_error(openslide_t *osr, const char *format, ...);
bool _openslide_check_cairo_status_possibly_set_error(openslide_t *osr,
						      cairo_t *cr);

// internal error propagation
enum OpenSlideError {
  // file format unrecognized; try other formats
  OPENSLIDE_ERROR_FORMAT_NOT_SUPPORTED,
  // file corrupt; hard fail
  OPENSLIDE_ERROR_BAD_DATA,
};
#define OPENSLIDE_ERROR _openslide_error_quark()
GQuark _openslide_error_quark(void);

void _openslide_io_error(GError **err, const char *fmt, ...);
void _openslide_set_error_from_gerror(openslide_t *osr, GError *err);

// private properties, for now
#define _OPENSLIDE_PROPERTY_NAME_LEVEL_COUNT "openslide.level-count"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_WIDTH "openslide.level[%d].width"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_HEIGHT "openslide.level[%d].height"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_DOWNSAMPLE "openslide.level[%d].downsample"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_TILE_WIDTH "openslide.level[%d].tile-width"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LEVEL_TILE_HEIGHT "openslide.level[%d].tile-height"

// deprecated prefetch stuff (maybe we'll undeprecate it someday),
// still needs these declarations for ABI compat
// TODO: remove if soname bump
#undef openslide_give_prefetch_hint
OPENSLIDE_PUBLIC()
int openslide_give_prefetch_hint(openslide_t *osr,
				 int64_t x, int64_t y,
				 int32_t level,
				 int64_t w, int64_t h);
#undef openslide_cancel_prefetch_hint
OPENSLIDE_PUBLIC()
void openslide_cancel_prefetch_hint(openslide_t *osr, int prefetch_id);


#endif
