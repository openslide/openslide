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

#define CACHE_BLOCKS 2
#define CACHE_BLOCK_SIZE 8192
#define CACHE_BLOCK_MIN_RELOAD 1024
//#define CACHE_DEBUG

#ifdef CACHE_DEBUG
#define cache_debug(args...) g_debug(args)
#else
#define cache_debug(args...) do {} while (0)
#endif

// implements DcmIO
struct _openslide_dicom_io {
  DcmIOMethods *methods;
  char *filename;
  struct _openslide_file *file;  // may not be present without ensure_file()
  int64_t virt_offset;
  int64_t phys_offset;
  int64_t size;

  uint8_t cache[CACHE_BLOCKS][CACHE_BLOCK_SIZE];
  int64_t cache_offset[CACHE_BLOCKS];
  int32_t cache_len[CACHE_BLOCKS];
  int cache_mru_block;
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
  g_assert(dio->phys_offset == 0);
  dio->file = _openslide_fopen(dio->filename, err);
  return dio->file != NULL;
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

// try to fill from cache, at least partially
// the cache exists to reduce seeks, which incur syscall overhead and flush
// libc's I/O buffer
static int64_t cache_read(struct _openslide_dicom_io *dio,
                          char *buffer, int64_t length) {
  for (int block = 0; block < CACHE_BLOCKS; block++) {
    if (dio->virt_offset >= dio->cache_offset[block] &&
        dio->virt_offset < dio->cache_offset[block] + dio->cache_len[block]) {
      int64_t count =
        MIN(length,
            dio->cache_offset[block] + dio->cache_len[block] - dio->virt_offset);
      cache_debug("cache read [%d] %"PRId64" %"PRId64,
                  block, dio->virt_offset, count);
      memcpy(buffer,
             &dio->cache[block][dio->virt_offset - dio->cache_offset[block]],
             count);
      dio->virt_offset += count;
      dio->cache_mru_block = block;
      return count;
    }
  }
  return 0;
}

static int64_t vfs_read(DcmError **dcm_error, DcmIO *io,
                        char *buffer, int64_t length) {
  struct _openslide_dicom_io *dio = (struct _openslide_dicom_io *) io;
  GError *tmp_err = NULL;
  if (!length) {
    return 0;
  }
  // fill from cache if possible
  int64_t count = cache_read(dio, buffer, length);
  if (count) {
    return count;
  }
  // nope, need a file handle
  if (!ensure_file(dio, &tmp_err)) {
    propagate_gerror(dcm_error, tmp_err);
    return -1;
  }
  // seek if needed
  // if we're going to fill from cache, allow loading part of the cache block
  // with data before the virt_offset rather than emitting a seek
  G_STATIC_ASSERT(CACHE_BLOCK_MIN_RELOAD > 0 &&
                  CACHE_BLOCK_MIN_RELOAD <= CACHE_BLOCK_SIZE);
  if (dio->virt_offset != dio->phys_offset &&
      (length >= CACHE_BLOCK_SIZE ||
       dio->virt_offset < dio->phys_offset ||
       dio->virt_offset - dio->phys_offset > CACHE_BLOCK_SIZE - CACHE_BLOCK_MIN_RELOAD)) {
    cache_debug("seek %s%"PRId64,
                dio->virt_offset > dio->phys_offset ? "+" : "",
                dio->virt_offset - dio->phys_offset);
    if (!_openslide_fseek(dio->file, dio->virt_offset, SEEK_SET, &tmp_err)) {
      propagate_gerror(dcm_error, tmp_err);
      return -1;
    }
    dio->phys_offset = dio->virt_offset;
  }
  // for short reads, load cache and fill from there
  if (length < CACHE_BLOCK_SIZE) {
    G_STATIC_ASSERT(CACHE_BLOCKS == 2);
    int block = !dio->cache_mru_block;
    dio->cache_mru_block = block;
    dio->cache_offset[block] = dio->phys_offset;
    dio->cache_len[block] =
      _openslide_fread(dio->file, dio->cache[block], CACHE_BLOCK_SIZE,
                       &tmp_err);
    if (tmp_err) {
      propagate_gerror(dcm_error, tmp_err);
      return -1;
    }
    cache_debug("cache fill [%d] %"PRId64" %u",
                block, dio->cache_offset[block], dio->cache_len[block]);
    dio->phys_offset += dio->cache_len[block];
    return cache_read(dio, buffer, length);
  }
  // long read; fill directly
  count = _openslide_fread(dio->file, buffer, length, &tmp_err);
  if (tmp_err) {
    propagate_gerror(dcm_error, tmp_err);
    return -1;
  }
  cache_debug("direct read %"PRId64" %"PRId64, dio->virt_offset, count);
  dio->virt_offset += count;
  dio->phys_offset = dio->virt_offset;
  return count;
}

static int64_t vfs_seek(DcmError **dcm_error G_GNUC_UNUSED, DcmIO *io,
                        int64_t offset, int whence) {
  struct _openslide_dicom_io *dio = (struct _openslide_dicom_io *) io;
  dio->virt_offset =
    _openslide_compute_seek(dio->virt_offset, dio->size, offset, whence);
  return dio->virt_offset;
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
    dio->phys_offset = 0;
  }
}
