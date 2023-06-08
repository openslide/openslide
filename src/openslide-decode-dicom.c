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

#include "openslide-private.h"
#include "openslide-decode-dicom.h"

// implements DcmIO
struct _dicom_io {
  DcmIOMethods *methods;
  struct _openslide_file *file;
};

void _openslide_dicom_propagate_error(GError **err, DcmError *dcm_error) {
  g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "libdicom %s: %s - %s",
              dcm_error_code_str(dcm_error_get_code(dcm_error)),
              dcm_error_get_summary(dcm_error),
              dcm_error_get_message(dcm_error));
  dcm_error_clear(&dcm_error);
}

static void propagate_gerror(DcmError **dcm_error, GError *err) {
  dcm_error_set(dcm_error, DCM_ERROR_CODE_INVALID,
                g_quark_to_string(err->domain),
                "%s", err->message);
  g_error_free(err);
}

static DcmIO *vfs_open(DcmError **dcm_error, void *client) {
  const char *filename = (const char *) client;
  struct _dicom_io *dio = g_new(struct _dicom_io, 1);

  GError *err = NULL;
  dio->file = _openslide_fopen(filename, &err);
  if (!dio->file) {
    g_free(dio);
    propagate_gerror(dcm_error, err);
    return NULL;
  }

  return (DcmIO *) dio;
}

static void vfs_close(DcmIO *io) {
  struct _dicom_io *dio = (struct _dicom_io *) io;

  _openslide_fclose(dio->file);
  g_free(dio);
}

static int64_t vfs_read(DcmError **dcm_error G_GNUC_UNUSED, DcmIO *io,
                        char *buffer, int64_t length) {
  struct _dicom_io *dio = (struct _dicom_io *) io;

  // openslide VFS has no error return for read()
  return _openslide_fread(dio->file, buffer, length);
}

static int64_t vfs_seek(DcmError **dcm_error, DcmIO *io,
                        int64_t offset, int whence) {
  struct _dicom_io *dio = (struct _dicom_io *) io;

  GError *err = NULL;
  if (!_openslide_fseek(dio->file, offset, whence, &err)) {
    propagate_gerror(dcm_error, err);
    return -1;
  }

  // libdicom uses lseek(2) semantics, so it must always return the new file
  // pointer
  off_t new_position = _openslide_ftell(dio->file, &err);
  if (new_position < 0) {
    propagate_gerror(dcm_error, err);
  }

  return new_position;
}

static const DcmIOMethods dicom_open_methods = {
  .open = vfs_open,
  .close = vfs_close,
  .read = vfs_read,
  .seek = vfs_seek,
};

DcmFilehandle *_openslide_dicom_open(const char *filename, GError **err) {

  DcmError *dcm_error = NULL;
  DcmIO *io = dcm_io_create(&dcm_error, &dicom_open_methods, (void *) filename);
  if (!io) {
    _openslide_dicom_propagate_error(err, dcm_error);
    return NULL;
  }

  DcmFilehandle *filehandle = dcm_filehandle_create(&dcm_error, io);
  if (!filehandle) {
    _openslide_dicom_propagate_error(err, dcm_error);
    dcm_io_close(io);
    return NULL;
  }

  return filehandle;
}
