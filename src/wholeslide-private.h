/*
 *  Wholeslide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  Wholeslide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  Wholeslide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Wholeslide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking Wholeslide statically or dynamically with other modules is
 *  making a combined work based on Wholeslide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#ifndef WHOLESLIDE_PRIVATE_H
#define WHOLESLIDE_PRIVATE_H

#include "wholeslide.h"

#include <stdbool.h>
#include <tiffio.h>
#include <jpeglib.h>
#include <openjpeg.h>

/* the main structure */
struct _wholeslide {
  struct _wholeslide_ops *ops;
  void *data;
  uint32_t layer_count;

  double *downsamples;  // filled in automatically
};

/* the function pointer structure for backends */
struct _wholeslide_ops {
  void (*read_region)(wholeslide_t *wsd, uint32_t *dest,
		      uint32_t x, uint32_t y,
		      uint32_t layer,
		      uint32_t w, uint32_t h);
  void (*destroy)(wholeslide_t *wsd);
  void (*get_dimensions)(wholeslide_t *wsd, uint32_t layer,
			 uint32_t *w, uint32_t *h);
  const char* (*get_comment)(wholeslide_t *wsd);
};


/* vendor detection and parsing */
bool _ws_try_trestle(wholeslide_t *wsd, const char* filename);
bool _ws_try_aperio(wholeslide_t *wsd, const char* filename);
bool _ws_try_hamamatsu(wholeslide_t *wsd, const char* filename);
bool _ws_try_generic_jp2k(wholeslide_t *wsd, const char* filename);

/* TIFF support */
struct _ws_tiff_tilereader;

void _ws_add_tiff_ops(wholeslide_t *wsd,
		      TIFF *tiff,
		      uint32_t overlap_count,
		      uint32_t *overlaps,
		      uint32_t layer_count,
		      uint32_t *layers,
		      struct _ws_tiff_tilereader *(*tilereader_create)(TIFF *tiff),
		      void (*tilereader_read)(struct _ws_tiff_tilereader *wtt,
					      uint32_t *dest,
					      uint32_t x, uint32_t y),
		      void (*tilereader_destroy)(struct _ws_tiff_tilereader *wtt));

struct _ws_tiff_tilereader *_ws_generic_tiff_tilereader_create(TIFF *tiff);
void _ws_generic_tiff_tilereader_read(struct _ws_tiff_tilereader *wtt,
				      uint32_t *dest,
				      uint32_t x, uint32_t y);
void _ws_generic_tiff_tilereader_destroy(struct _ws_tiff_tilereader *wtt);


/* JPEG support */
struct _ws_jpeg_fragment {
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
  uint32_t x;
  uint32_t y;

  // this value starts from 0 at the largest layer
  uint32_t z;
};

// note: fragments MUST be sorted by z, then x, then y
void _ws_add_jpeg_ops(wholeslide_t *wsd,
		      uint32_t count,
		      struct _ws_jpeg_fragment **fragments);

void _ws_jpeg_fancy_src(j_decompress_ptr cinfo, FILE *infile,
			int64_t *start_positions,
			uint64_t start_positions_count,
			uint64_t topleft,
			uint32_t width, uint32_t stride);
int64_t _ws_jpeg_fancy_src_get_filepos(j_decompress_ptr cinfo);


/* JPEG-2000 support */
void _ws_add_jp2k_ops(wholeslide_t *wsd,
		      FILE *f,
		      uint32_t w, uint32_t h);

#endif
