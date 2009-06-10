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
  struct _openslide_ops *ops;
  void *data;
  int32_t layer_count;

  int32_t overlap_count;
  int32_t *overlaps;

  uint32_t fill_color_argb;

  double *downsamples;  // filled in automatically from dimensions

  // associated images
  GHashTable *associated_images;  // created automatically
  const char **associated_image_names; // filled in automatically from hashtable

  // metadata
  GHashTable *properties; // created automatically
  const char **property_names; // filled in automatically from hashtable
};

/* overlaps support */
void _openslide_get_overlaps(openslide_t *osr, int32_t layer,
			     int32_t *x, int32_t *y);
void _openslide_add_in_overlaps(openslide_t *osr,
				int32_t layer,
				int64_t tw, int64_t th,
				int64_t total_tiles_across,
				int64_t total_tiles_down,
				int64_t x, int64_t y,
				int64_t *out_x, int64_t *out_y);

/* the function pointer structure for backends */
struct _openslide_ops {
  void (*read_region)(openslide_t *osr, uint32_t *dest,
		      int64_t x, int64_t y,
		      int32_t layer,
		      int64_t w, int64_t h);
  void (*destroy)(openslide_t *osr);
  void (*get_dimensions)(openslide_t *osr, int32_t layer,
			 int64_t *image_w, int64_t *image_h,
			 int64_t *tile_w, int64_t *tile_h);
};


/* vendor detection and parsing */
bool _openslide_try_trestle(openslide_t *osr, const char* filename);
bool _openslide_try_aperio(openslide_t *osr, const char* filename);
bool _openslide_try_hamamatsu(openslide_t *osr, const char* filename);
bool _openslide_try_mirax(openslide_t *osr, const char* filename);

/* TIFF support */
struct _openslide_tiff_tilereader;

void _openslide_add_tiff_ops(openslide_t *osr,
			     TIFF *tiff,
			     int32_t layer_count,
			     int32_t *layers,
			     struct _openslide_tiff_tilereader *(*tilereader_create)(TIFF *tiff),
			     void (*tilereader_read)(struct _openslide_tiff_tilereader *wtt,
						     uint32_t *dest,
						     int64_t x,
						     int64_t y),
			     void (*tilereader_destroy)(struct _openslide_tiff_tilereader *wtt));

struct _openslide_tiff_tilereader *_openslide_generic_tiff_tilereader_create(TIFF *tiff);
void _openslide_generic_tiff_tilereader_read(struct _openslide_tiff_tilereader *wtt,
					     uint32_t *dest,
					     int64_t x, int64_t y);
void _openslide_generic_tiff_tilereader_destroy(struct _openslide_tiff_tilereader *wtt);


/* JPEG support */
struct _openslide_jpeg_fragment {
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

  // all fragments together should form a dense space,
  // with no gaps in x,y,z

  // these coordinates start from 0 and look like this:
  //
  // ----------------
  // |       |      |
  // |       |      |
  // | (0,0) | (1,0)|
  // |       |      |
  // |       |      |
  // ----------------
  // |       |      |
  // | (0,1) | (1,1)|
  // |       |      |
  // ----------------
  int32_t x;
  int32_t y;

  // this value starts from 0 at the largest layer
  int32_t z;
};

// note: fragments MUST be sorted by z, then x, then y
void _openslide_add_jpeg_ops(openslide_t *osr,
			     int32_t count,
			     struct _openslide_jpeg_fragment **fragments);

// error function for libjpeg
struct _openslide_jpeg_error_mgr {
  struct jpeg_error_mgr pub;      // public fields

  jmp_buf *env;
};

struct jpeg_error_mgr *_openslide_jpeg_set_error_handler(struct _openslide_jpeg_error_mgr *err,
							 jmp_buf *env);

#endif
