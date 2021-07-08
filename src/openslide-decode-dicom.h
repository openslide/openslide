/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2015 Mathieu Malaterre
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

#ifndef OPENSLIDE_OPENSLIDE_DECODE_DICOM_H_
#define OPENSLIDE_OPENSLIDE_DECODE_DICOM_H_

#include "openslide-private.h"
#include "openslide-hash.h"

#include <stdio.h>
#include <stdint.h>
#include <glib.h>

/* DICOM container support for VL Whole Slide Microscopy Image Storage (1.2.840.10008.5.1.4.1.1.77.1.6) */
/* Thread-safe. */

bool _openslide_dicom_is_dicomdir(const char *filename, GError **err);

struct _openslide_dicom *_openslide_dicom_create(const char *filename,
                                                       GError **err);

void _openslide_dicom_destroy(struct _openslide_dicom *d);

bool _openslide_dicom_readindex(struct _openslide_dicom *instance, const char * dirname, char ***datafile_paths);

enum image_format {
  FORMAT_UNKNOWN,
  FORMAT_JPEG,
};

struct tile {
  int64_t start_in_file;
  int64_t length;
};

struct _openslide_dicom_level {
  int64_t image_w;
  int64_t image_h;
  int64_t tile_w;
  int64_t tile_h;
  int64_t tiles_across;
  int64_t tiles_down;
  bool is_icon;
  char hash[512];
  enum image_format image_format;
  int fileno;
  struct tile *tiles;
};

bool _openslide_dicom_level_init(struct _openslide_dicom *instance,
                                struct _openslide_level *level,
                                struct _openslide_dicom_level *dicoml,
                                GError **err);
#endif
