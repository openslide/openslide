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

#ifndef OPENSLIDE_OPENSLIDE_PRIVATE_H_
#define OPENSLIDE_OPENSLIDE_PRIVATE_H_

#ifdef _WIN32
#define WIN32 1
#define __MSVCRT_VERSION__ 0x0800
#endif

#if !defined(fseeko) && defined(_WIN32)
#define fseeko(stream, offset, origin) _fseeki64(stream, offset, origin)
#define ftello(stream) _ftelli64(stream)
#endif

#include <config.h>

#include "openslide.h"
#include "openslide-hash.h"

#include <glib.h>
#ifndef _MSC_VER
#include <stdbool.h>
#endif
#include <setjmp.h>
#include <tiffio.h>


// TODO detect if libjpeg is built as a C++ library?
#ifdef __cplusplus
extern "C" {
#endif
#include <jpeglib.h>
#ifdef __cplusplus
};
#endif


#include <cairo.h>

#include <openjpeg.h>

/* the associated image structure */
struct _openslide_associated_image {
  int64_t w;
  int64_t h;
  void *ctx;

  // must fail if width or height doesn't match the image
  void (*get_argb_data)(openslide_t *osr, void *ctx, uint32_t *dest,
                        int64_t w, int64_t h);
  void (*destroy_ctx)(void *ctx);
};

/* the main structure */
struct _openslide {
  const struct _openslide_ops *ops;
  void *data;
  int32_t layer_count;

  double *downsamples;  // zero values or NULL are filled in automatically from dimensions

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

/* the function pointer structure for backends */
struct _openslide_ops {
  void (*get_dimensions)(openslide_t *osr,
			 int32_t layer,
			 int64_t *w, int64_t *h);
  void (*paint_region)(openslide_t *osr, cairo_t *cr,
		       int64_t x, int64_t y,
		       int32_t layer,
		       int32_t w, int32_t h);
  void (*destroy)(openslide_t *osr);
};

/* vendor detection and parsing */
typedef bool (*_openslide_vendor_fn)(openslide_t *osr, const char *filename,
				     struct _openslide_hash *quickhash1);
typedef bool (*_openslide_tiff_vendor_fn)(openslide_t *osr, TIFF *tiff,
					  struct _openslide_hash *quickhash1);
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


bool _openslide_try_trestle(openslide_t *osr, TIFF *tiff,
			    struct _openslide_hash *quickhash1);
bool _openslide_try_aperio(openslide_t *osr, TIFF *tiff,
			   struct _openslide_hash *quickhash1);
bool _openslide_try_hamamatsu(openslide_t *osr, const char* filename,
			      struct _openslide_hash *quickhash1);
bool _openslide_try_hamamatsu_ndpi(openslide_t *osr, const char* filename,
				   struct _openslide_hash *quickhash1);
bool _openslide_try_mirax(openslide_t *osr, const char* filename,
			  struct _openslide_hash *quickhash1);
bool _openslide_try_generic_tiff(openslide_t *osr, TIFF *tiff,
				 struct _openslide_hash *quickhash1);


/* GHashTable utils */
guint _openslide_int64_hash(gconstpointer v);
gboolean _openslide_int64_equal(gconstpointer v1, gconstpointer v2);
void _openslide_int64_free(gpointer data);

/* g_key_file_load_from_file wrapper */
gboolean _openslide_read_key_file(GKeyFile *key_file, const char *filename,
                                  GKeyFileFlags flags, GError **error);

/* fopen() wrapper which properly sets FD_CLOEXEC */
FILE *_openslide_fopen(const char *path, const char *mode);

/* TIFF support */
typedef void (*_openslide_tiff_tilereader_fn)(openslide_t *osr,
					      TIFF *tiff,
					      uint32_t *dest,
					      int64_t x,
					      int64_t y,
					      int32_t w,
					      int32_t h);

void _openslide_add_tiff_ops(openslide_t *osr,
			     TIFF *tiff,
			     int32_t overlap_count,
			     int32_t *overlaps,
			     int32_t layer_count,
			     int32_t *layers,
			     _openslide_tiff_tilereader_fn tileread,
			     struct _openslide_hash *quickhash1);

void _openslide_generic_tiff_tilereader(openslide_t *osr,
					TIFF *tiff,
					uint32_t *dest,
					int64_t x, int64_t y,
					int32_t w, int32_t h);

bool _openslide_add_tiff_associated_image(GHashTable *ht,
					  const char *name,
					  TIFF *tiff);

/* JPEG support */
struct _openslide_jpeg_file {
  char *filename;

  int64_t start_in_file;
  int64_t end_in_file;

  // if known, put mcu starts here, set unknowns to -1,
  // and give dimensions and tile dimensions
  int64_t *mcu_starts;
  int32_t w;
  int32_t h;
  int32_t tw;
  int32_t th;
};

struct _openslide_jpeg_tile {
  // which tile and file?
  int32_t fileno;
  int32_t tileno;

  // bounds in the physical tile?
  double src_x;
  double src_y;
  double w;
  double h;

  // delta for this tile from the standard advance
  double dest_offset_x;
  double dest_offset_y;
};

struct _openslide_jpeg_layer {
  GHashTable *tiles;

  // size of canvas
  int64_t layer_w;
  int64_t layer_h;

  int32_t tiles_across;
  int32_t tiles_down;

  // ONLY for convenience in checking even scale_denom division
  int32_t raw_tile_width;
  int32_t raw_tile_height;

  // standard advance
  double tile_advance_x;
  double tile_advance_y;

  // if zero, calculated automatically
  double downsample;
};

void _openslide_add_jpeg_ops(openslide_t *osr,
			     int32_t file_count,
			     struct _openslide_jpeg_file **files,
			     int32_t layer_count,
			     struct _openslide_jpeg_layer **layers);

GHashTable *_openslide_jpeg_create_tiles_table(void);

bool _openslide_add_jpeg_associated_image(GHashTable *ht,
					  const char *name,
					  const char *filename,
					  int64_t offset);

/*
 * On Windows, we cannot fopen a file and pass it to another DLL that does fread.
 * So we need to compile all our freading into the OpenSlide DLL directly.
 */
void _openslide_jpeg_stdio_src(j_decompress_ptr cinfo, FILE *infile);

// error function for libjpeg
struct _openslide_jpeg_error_mgr {
  struct jpeg_error_mgr pub;      // public fields

  jmp_buf *env;
};

struct jpeg_error_mgr *_openslide_jpeg_set_error_handler(struct _openslide_jpeg_error_mgr *err,
							 jmp_buf *env);

// Hamamatsu NGR
struct _openslide_ngr {
  char *filename;

  int64_t start_in_file;

  int32_t w;
  int32_t h;

  int32_t column_width;
};

void _openslide_add_ngr_ops(openslide_t *osr,
			    int32_t ngr_count,
			    struct _openslide_ngr **ngrs);


// error handling
bool _openslide_set_error(openslide_t *osr, const char *format, ...);
bool _openslide_check_cairo_status_possibly_set_error(openslide_t *osr,
						      cairo_t *cr);


// background color helper
void _openslide_set_background_color_property(GHashTable *ht,
					      uint8_t r, uint8_t g, uint8_t b);

// private properties, for now
#define _OPENSLIDE_PROPERTY_NAME_LAYER_COUNT "openslide.layer-count"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LAYER_WIDTH "openslide.layer[%d].width"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LAYER_HEIGHT "openslide.layer[%d].height"
#define _OPENSLIDE_PROPERTY_NAME_TEMPLATE_LAYER_DOWNSAMPLE "openslide.layer[%d].downsample"

// deprecated prefetch stuff (maybe we'll undeprecate it someday),
// still needs these declarations for ABI compat
// TODO: remove if soname bump
#undef openslide_give_prefetch_hint
OPENSLIDE_PUBLIC()
int openslide_give_prefetch_hint(openslide_t *osr,
				 int64_t x, int64_t y,
				 int32_t layer,
				 int64_t w, int64_t h);
#undef openslide_cancel_prefetch_hint
OPENSLIDE_PUBLIC()
void openslide_cancel_prefetch_hint(openslide_t *osr, int prefetch_id);


#endif
