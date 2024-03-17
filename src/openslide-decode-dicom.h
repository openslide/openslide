/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2022-2023 Benjamin Gilbert
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

#include <glib.h>
#include <dicom/dicom.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(DcmFilehandle, dcm_filehandle_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(DcmFrame, dcm_frame_destroy)

DcmFilehandle *_openslide_dicom_open(const char *filename, GError **err);
void _openslide_dicom_propagate_error(GError **err, DcmError *dcm_error);

#endif
