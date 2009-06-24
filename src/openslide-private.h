/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
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

#ifndef OPENSLIDE_OPENSLIDE_PRIVATE_H_
#define OPENSLIDE_OPENSLIDE_PRIVATE_H_

#ifdef _WIN32
#define WIN32 1
#endif

#if !defined(fseeko) && defined(_WIN32)
#define fseeko(stream, offset, origin) _fseeki64(stream, offset, origin)
#define ftello(stream) _ftelli64(stream)
#endif

#include "openslide.h"

#include <glib.h>
#include <stdbool.h>
#include <setjmp.h>
#include <tiffio.h>
#include <jpeglib.h>
#include <cairo.h>

#include <openjpeg/openjpeg.h>

#define _OPENSLIDE_COMMENT_NAME "comment"

/* the associated image structure */
struct _openslide_associated_image {
  int64_t w;
  int64_t h;
  uint32_t *argb_data;
};

/* the main structure */
struct _openslide {
  const struct _openslide_ops *ops;
  void *data;
  int32_t layer_count;

  uint32_t fill_color_argb;

  double *downsamples;  // filled in automatically from dimensions

  // associated images
  GHashTable *associated_images;  // created automatically
  const char **associated_image_names; // filled in automatically from hashtable

  // metadata
  GHashTable *properties; // created automatically
  const char **property_names; // filled in automatically from hashtable

  // cache
  struct _openslide_cache *cache;
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
bool _openslide_try_trestle(openslide_t *osr, const char* filename);
bool _openslide_try_aperio(openslide_t *osr, const char* filename);
bool _openslide_try_hamamatsu(openslide_t *osr, const char* filename);
bool _openslide_try_mirax(openslide_t *osr, const char* filename);

/* TIFF support */
typedef void (*_openslide_tiff_tilereader_fn)(TIFF *tiff,
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
			     _openslide_tiff_tilereader_fn tileread);

void _openslide_generic_tiff_tilereader(TIFF *tiff,
					uint32_t *dest,
					int64_t x, int64_t y,
					int32_t w, int32_t h);


/* JPEG support */
struct _openslide_jpeg_file {
  FILE *f;

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
  float src_x;
  float src_y;
  float w;
  float h;

  // delta for this tile from the standard advance
  float dest_offset_x;
  float dest_offset_y;
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
};

void _openslide_add_jpeg_ops(openslide_t *osr,
			     int32_t file_count,
			     struct _openslide_jpeg_file **files,
			     int32_t layer_count,
			     struct _openslide_jpeg_layer **layers);

// error function for libjpeg
struct _openslide_jpeg_error_mgr {
  struct jpeg_error_mgr pub;      // public fields

  jmp_buf *env;
};

struct jpeg_error_mgr *_openslide_jpeg_set_error_handler(struct _openslide_jpeg_error_mgr *err,
							 jmp_buf *env);
GHashTable *_openslide_jpeg_create_tiles_table(void);

#endif
