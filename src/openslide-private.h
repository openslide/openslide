/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
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

#include "openslide.h"

#include <stdbool.h>
#include <tiffio.h>
#include <jpeglib.h>
#include <openjpeg.h>

/* the main structure */
struct _openslide {
  struct _openslide_ops *ops;
  void *data;
  int32_t layer_count;

  double *downsamples;  // filled in automatically
};

/* the function pointer structure for backends */
struct _openslide_ops {
  void (*read_region)(openslide_t *osr, uint32_t *dest,
		      int64_t x, int64_t y,
		      int32_t layer,
		      int64_t w, int64_t h);
  void (*destroy)(openslide_t *osr);
  void (*get_dimensions)(openslide_t *osr, int32_t layer,
			 int64_t *w, int64_t *h);
  const char* (*get_comment)(openslide_t *osr);
};


/* vendor detection and parsing */
bool _openslide_try_trestle(openslide_t *osr, const char* filename);
bool _openslide_try_aperio(openslide_t *osr, const char* filename);
bool _openslide_try_hamamatsu(openslide_t *osr, const char* filename);
bool _openslide_try_generic_jp2k(openslide_t *osr, const char* filename);

/* TIFF support */
struct _openslide_tiff_tilereader;

void _openslide_add_tiff_ops(openslide_t *osr,
			     TIFF *tiff,
			     int32_t overlap_count,
			     int32_t *overlaps,
			     int32_t layer_count,
			     int32_t *layers,
			     struct _openslide_tiff_tilereader *(*tilereader_create)(TIFF *tiff),
			     void (*tilereader_read)(struct _openslide_tiff_tilereader *wtt,
						     uint32_t *dest,
						     int64_t x, int64_t y),
			     void (*tilereader_destroy)(struct _openslide_tiff_tilereader *wtt));

struct _openslide_tiff_tilereader *_openslide_generic_tiff_tilereader_create(TIFF *tiff);
void _openslide_generic_tiff_tilereader_read(struct _openslide_tiff_tilereader *wtt,
					     uint32_t *dest,
					     int64_t x, int64_t y);
void _openslide_generic_tiff_tilereader_destroy(struct _openslide_tiff_tilereader *wtt);


/* JPEG support */
struct _openslide_jpeg_fragment {
  FILE *f;

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

void _openslide_jpeg_fancy_src(j_decompress_ptr cinfo, FILE *infile,
			       int64_t *start_positions,
			       int64_t start_positions_count,
			       int64_t topleft,
			       int32_t width, int32_t stride);
int64_t _openslide_jpeg_fancy_src_get_filepos(j_decompress_ptr cinfo);


/* JPEG-2000 support */
void _openslide_add_jp2k_ops(openslide_t *osr,
			     FILE *f,
			     int64_t w, int64_t h);

#endif
