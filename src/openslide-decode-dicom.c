/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  Copyright (c) 2022-2024 Benjamin Gilbert
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
struct _openslide_dicom_io {
  DcmIOMethods *methods;
  char *filename;
  struct _openslide_file *file;  // may not be present without ensure_file()
  int64_t offset;
  int64_t size;
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

static void dicom_io_free(struct _openslide_dicom_io *dio) {
  if (dio->file) {
    _openslide_fclose(dio->file);
  }
  g_free(dio->filename);
  g_free(dio);
}
typedef struct _openslide_dicom_io _openslide_dicom_io;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(_openslide_dicom_io, dicom_io_free);

// ensure dcm->file is available
static bool ensure_file(struct _openslide_dicom_io *dio, GError **err) {
  if (dio->file) {
    return true;
  }
  g_autoptr(_openslide_file) f = _openslide_fopen(dio->filename, err);
  if (!f) {
    return false;
  }
  if (dio->offset && !_openslide_fseek(f, dio->offset, SEEK_SET, err)) {
    return false;
  }
  dio->file = g_steal_pointer(&f);
  return true;
}

static DcmIO *vfs_open(DcmError **dcm_error, void *client) {
  g_autoptr(_openslide_dicom_io) dio = g_new0(struct _openslide_dicom_io, 1);
  dio->filename = g_strdup(client);

  GError *tmp_err = NULL;
  if (!ensure_file(dio, &tmp_err)) {
    propagate_gerror(dcm_error, tmp_err);
    return NULL;
  }
  dio->size = _openslide_fsize(dio->file, &tmp_err);
  if (dio->size == -1) {
    propagate_gerror(dcm_error, tmp_err);
    return NULL;
  }

  return (DcmIO *) g_steal_pointer(&dio);
}

static void vfs_close(DcmIO *io) {
  dicom_io_free((struct _openslide_dicom_io *) io);
}

static int64_t vfs_read(DcmError **dcm_error, DcmIO *io,
                        char *buffer, int64_t length) {
  struct _openslide_dicom_io *dio = (struct _openslide_dicom_io *) io;
  GError *tmp_err = NULL;
  if (!ensure_file(dio, &tmp_err)) {
    propagate_gerror(dcm_error, tmp_err);
    return 0;
  }
  // openslide VFS has no error return for read()
  int64_t count = _openslide_fread(dio->file, buffer, length);
  dio->offset += count;
  return count;
}

static int64_t vfs_seek(DcmError **dcm_error, DcmIO *io,
                        int64_t offset, int whence) {
  struct _openslide_dicom_io *dio = (struct _openslide_dicom_io *) io;
  int64_t new_offset =
    _openslide_compute_seek(dio->offset, dio->size, offset, whence);
  if (dio->file) {
    GError *tmp_err = NULL;
    if (!_openslide_fseek(dio->file, new_offset, SEEK_SET, &tmp_err)) {
      propagate_gerror(dcm_error, tmp_err);
      return -1;
    }
  }
  dio->offset = new_offset;
  return new_offset;
}

static const DcmIOMethods dicom_open_methods = {
  .open = vfs_open,
  .close = vfs_close,
  .read = vfs_read,
  .seek = vfs_seek,
};

// returns a borrowed reference in *dio_OUT
DcmFilehandle *_openslide_dicom_open(const char *filename,
                                     struct _openslide_dicom_io **dio_OUT,
                                     GError **err) {
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

  *dio_OUT = (struct _openslide_dicom_io *) io;
  return filehandle;
}

void _openslide_dicom_io_suspend(struct _openslide_dicom_io *dio) {
  if (dio->file) {
    _openslide_fclose(g_steal_pointer(&dio->file));
  }
}
