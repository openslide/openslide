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

static void *vfs_open(DcmError **dcm_error, void *client) {
  const char *filename = (const char *) client;

  GError *err = NULL;
  struct _openslide_file *file = _openslide_fopen(filename, &err);
  if (!file) {
    propagate_gerror(dcm_error, err);
    return NULL;
  }

  return file;
}

static int vfs_close(DcmError **dcm_error G_GNUC_UNUSED, void *data) {
  struct _openslide_file *file = (struct _openslide_file *) data;
  _openslide_fclose(file);
  return 0;
}

static int64_t vfs_read(DcmError **dcm_error G_GNUC_UNUSED, void *data,
                        char *buffer, int64_t length) {
  struct _openslide_file *file = (struct _openslide_file *) data;
  // openslide VFS has no error return for read()
  return _openslide_fread(file, buffer, length);
}

static int64_t vfs_seek(DcmError **dcm_error, void *data,
                        int64_t offset, int whence) {
  struct _openslide_file *file = (struct _openslide_file *) data;

  GError *err = NULL;
  if (!_openslide_fseek(file, offset, whence, &err)) {
    propagate_gerror(dcm_error, err);
    return -1;
  }

  // libdicom uses lseek(2) semantics, so it must always return the new file
  // pointer
  off_t new_position = _openslide_ftell(file, &err);
  if (new_position < 0) {
    propagate_gerror(dcm_error, err);
  }

  return new_position;
}

static const DcmIO io_funcs = {
  .open = vfs_open,
  .close = vfs_close,
  .read = vfs_read,
  .seek = vfs_seek,
};

DcmFilehandle *_openslide_dicom_open(const char *filename, GError **err) {
  DcmError *dcm_error = NULL;
  DcmFilehandle *result =
    dcm_filehandle_create(&dcm_error, &io_funcs, (void *) filename);
  if (!result) {
    _openslide_dicom_propagate_error(err, dcm_error);
    return NULL;
  }
  return result;
}
