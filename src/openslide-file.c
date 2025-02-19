/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2015 Carnegie Mellon University
 *  Copyright (c) 2015 Benjamin Gilbert
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

#include <config.h>

#include "openslide-private.h"
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <emscripten.h>
#include <glib.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

struct _openslide_file {
  FILE *fp;
  char *path;
  bool isRemote;
};

struct _openslide_dir {
  GDir *dir;
  char *path;
};

static void wrap_fclose(FILE *fp) {
  fclose(fp);  // ci-allow
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FILE, wrap_fclose)

static void io_error(GError **err, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void io_error(GError **err, const char *fmt, ...) {
  int my_errno = errno;
  va_list ap;

  va_start(ap, fmt);
  g_autofree char *msg = g_strdup_vprintf(fmt, ap);
  g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(my_errno),
              "%s: %s", msg, g_strerror(my_errno));
  va_end(ap);
}


/** Remote read emscripten callbacks **/
EM_ASYNC_JS(int, remote_open, (void* path, int* error), {
    const BLOCK_SIZE = 150000;
    if (!Module.fileHandleMap) Module.fileHandleMap = {};
    const handle = Object.keys(Module.fileHandleMap).length + 1;
    let url = "";
    let i = 0;
    while (Module.HEAP8[path + i] !== 0) {
        url += String.fromCharCode(Module.HEAP8[path + i]);
        i++;
    }
    const res = await fetch(url, {
      method: "HEAD"
    });
    let contentLength = res.headers.get("Content-Length");
    contentLength = contentLength ? parseInt(contentLength, 10) : null;
    if (!res.ok || !contentLength) {
      Module.HEAP32[error / 4] = -1;
      return -1;
    }
    const numBlocks = Math.ceil(contentLength / BLOCK_SIZE);
    const blocks = [];
    for (let i = 0; i < numBlocks; i++) blocks.push(null);
    Module.fileHandleMap[handle] = {
      url,
      position: 0,
      size: contentLength,
      blocks
    };
    Module.HEAP32[error / 4] = 0;
    return handle;
});

bool has_http_prefix(const char *str) {
    return strncmp(str, "http", 4) == 0;
}

int do_fopen(const char *path, const char *mode, GError **err) {
  int* status = malloc(sizeof(int));
  int result = remote_open(path, status);
  
  if (result <= 0 || *status < 0) {
    io_error(err, "Couldn't open %s %s", path, mode);
    free(status);
    return 0;
  }
  free(status);
  return result;
}

struct _openslide_file *_openslide_fopen(const char *path, GError **err)
{
  char *new_path = strdup(path);
  int f = do_fopen(new_path, "rb" FOPEN_CLOEXEC_FLAG, err);
  struct _openslide_file *file = calloc(1, sizeof(struct _openslide_file));
  file->fp = (FILE*)f;
  file->path = strdup(new_path);
  file->isRemote = true;
  free(new_path);
  return file;

}


EM_ASYNC_JS(size_t, remote_read, (int handle, bool exact, void* buf, size_t size, int* error), {
    const BLOCK_SIZE = 150000;
    const file = Module.fileHandleMap[handle];
    if (!file) {
      Module.HEAP32[error / 4] = -1;
      return;
    }
    if ((file.position + size) > file.size) {
      size = file.size - file.position;
    }

    const startBlock = Math.floor(file.position / BLOCK_SIZE);
    const endBlock = Math.floor((file.position + size) / BLOCK_SIZE);
    const blocksToFetch = [];
    for (let i = startBlock; i <= endBlock; i++) {
      if (file.blocks[i] === null) {
        blocksToFetch.push(i);
      }
    }

    const blockPromises = blocksToFetch.map((d) => {
      return fetch(file.url, {
        method: "GET",
        headers: {
          "Range": `bytes=${d * BLOCK_SIZE}-${( d + 1 ) * BLOCK_SIZE - 1}`
        }
      })
    });

    const results = await Promise.all(blockPromises);

    for(let i = 0; i < results.length; i++) {
      if (!results[i].ok) {
        Module.HEAP32[error / 4] = -2;
        return;
      } else {
        file.blocks[blocksToFetch[i]] = {
          data: await results[i].arrayBuffer()
        }
      }
    }

    const arrayBuffer = new ArrayBuffer(size);
    const arrayView = new Uint8Array(arrayBuffer);
    // copy in the data from the blocks
    for (let i = startBlock; i <= endBlock; i++) {
      const posOfBlock = i * BLOCK_SIZE;
      const posOfStart = file.position;
      const startReadIdx = Math.max(0, posOfStart - posOfBlock);
      const stopReadIdx = Math.min (file.position + size - posOfBlock, BLOCK_SIZE);
      const subset = file.blocks[i].data.slice(startReadIdx, stopReadIdx);
      const positionOfReadInBlock = (posOfBlock + startReadIdx);
      const posInArr = positionOfReadInBlock - file.position;
      arrayView.set(new Uint8Array(subset), posInArr);
    }

    if (exact && arrayBuffer.byteLength !== size) {
      Module.HEAP32[error / 4] = -3;
      return;
    }
    const bufferView = new Uint8Array(arrayBuffer);
    const newBuffer = new Uint8Array(Module.HEAPU8.buffer, buf, size);
    newBuffer.set(bufferView);
    file.position += size;
    Module.HEAP32[error / 4] = 0;
    return size;
});


// returns 0/NULL on EOF and 0/non-NULL on I/O error
size_t _openslide_fread(struct _openslide_file *file, void *buf, size_t size,
                        GError **err) {
  
  if (file->isRemote) {
    int* e = malloc(sizeof(int));
    size_t total = remote_read((int)file->fp, false, buf, size, e);
    if (total == 0 && *e < 0) {
      g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_IO,
                  "I/O error reading file %s", file->path);
    }
    free(e);
    return total;
  } else {
    char *bufp = buf;
    size_t total = 0;
    while (total < size) {
      size_t count = fread(bufp + total, 1, size - total, file->fp);  // ci-allow
      if (count == 0) {
        break;
      }
      total += count;
    }
    if (total == 0 && ferror(file->fp)) {
      g_set_error(err, G_FILE_ERROR, G_FILE_ERROR_IO,
                  "I/O error reading file %s", file->path);
    }
    return total;
  }
}

bool _openslide_fread_exact(struct _openslide_file *file,
                            void *buf, size_t size, GError **err) {
  GError *tmp_err = NULL;
  size_t count = _openslide_fread(file, buf, size, &tmp_err);
  if (tmp_err) {
    g_propagate_error(err, tmp_err);
    return false;
  } else if (count < size) {
    g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                "Short read of file %s: %"PRIu64" < %"PRIu64,
                file->path, (uint64_t) count, (uint64_t) size);
    return false;
  }
  return true;
}

EM_JS(void, remote_seek, (int handle, size_t offset, int whence, int* error, int set, int end, int cur), {
  const file = Module.fileHandleMap[handle];
  if (!file) {
    Module.HEAP32[error/4] = -1;
    return;
  }
  let newPos;
  if (whence === set) {
    newPos = offset;
  } else if (whence === end) {
    if (offset > file.size) {
      Module.HEAP32[error/4] = -3;
      return;
    }
    newPos = file.size + offset;
  } else if (whence === cur) {
    newPos = file.position + offset;
  } else {
    Module.HEAP32[error/4] = -2;
    return;
  }
  if (newPos < 0 || newPos > file.size) {
    Module.setValue(errorPtr, -3, 'i32'); // Out of range
    return;
  }
  file.position = newPos;
  Module.HEAP32[error/4] = 0;
});

bool _openslide_fseek(struct _openslide_file *file, off_t offset, int whence,
                      GError **err) {
  if (file->isRemote) {
    int* e = malloc(sizeof(int));
    remote_seek((int)file->fp, offset, whence, e, SEEK_SET, SEEK_END, SEEK_CUR);
    if (e < 0) {  // ci-allow
      io_error(err, "Couldn't seek file %s", file->path);
      return false;
    }
    return true;
  } else {
    if (fseeko(file->fp, offset, whence)) {  // ci-allow
      io_error(err, "Couldn't seek file %s", file->path);
      return false;
    }
    return true;
  }

}

EM_JS(size_t, remote_tell, (int handle, int* error), {
  const file = Module.fileHandleMap[handle];
  if (!file) {
    Module.HEAP32[error/4] = -1;
    return;
  }

  Module.HEAP32[error/4] = 0;
  return file.position;
});


off_t _openslide_ftell(struct _openslide_file *file, GError **err) {
  if (file->isRemote) {
    int e;
    off_t ret = remote_tell((int)file->fp, &e);  // ci-allow
    if (ret < 0) {
      io_error(err, "Couldn't get offset of %s", file->path);
    }
    return ret;
  } else {
    off_t ret = ftello(file->fp);  // ci-allow
    if (ret == -1) {
      io_error(err, "Couldn't get offset of %s", file->path);
    }
    return ret;
  }
}

EM_JS(size_t, remote_size, (int handle, int* error), {
    const file = Module.fileHandleMap[handle];
  if (!file) {
    Module.HEAP32[error/4] = -1;
    return;
  }

  Module.HEAP32[error/4] = 0;
  return file.size;
});

off_t _openslide_fsize(struct _openslide_file *file, GError **err) {
  off_t orig = _openslide_ftell(file, err);
  if (orig == -1) {
    g_prefix_error(err, "Couldn't get size: ");
    return -1;
  }
  if (!_openslide_fseek(file, 0, SEEK_END, err)) {
    g_prefix_error(err, "Couldn't get size: ");
    return -1;
  }
  off_t ret = _openslide_ftell(file, err);
  if (ret == -1) {
    g_prefix_error(err, "Couldn't get size: ");
    return -1;
  }
  if (!_openslide_fseek(file, orig, SEEK_SET, err)) {
    g_prefix_error(err, "Couldn't get size: ");
    return -1;
  }
  return ret;
}

EM_JS(void, remote_close, (int handle, int* error), {
  const file = Module.fileHandleMap[handle];
  if (!file) {
    Module.HEAP32[error/4] = -1;
    return;
  }
  delete Module.fileHandleMap[handle];
});

EM_JS(void, remote_exists, (const char* path, int* error), {
  console.log('(remote_exists)')
});

/** End emscripten callbacks  **/

void _openslide_fclose(struct _openslide_file *file) {
  if (file->isRemote) {
    int e;
    remote_close((int) file->fp, &e);
  } else {
    fclose(file->fp);  // ci-allow
    g_free(file->path);
    g_free(file);
  }
}

bool _openslide_fexists(const char *path, GError **err G_GNUC_UNUSED) {
  if (has_http_prefix(path)) {
    int e;
    remote_open(path, &e);
    return e > 0;
  } else {
    return g_file_test(path, G_FILE_TEST_EXISTS); 
  }
}

struct _openslide_dir *_openslide_dir_open(const char *dirname, GError **err) {
  g_autoptr(_openslide_dir) d = g_new0(struct _openslide_dir, 1);
  d->dir = g_dir_open(dirname, 0, err);
  if (!d->dir) {
    return NULL;
  }
  d->path = g_strdup(dirname);
  return g_steal_pointer(&d);
}

const char *_openslide_dir_next(struct _openslide_dir *d, GError **err) {
  errno = 0;
  const char *ret = g_dir_read_name(d->dir);
  if (!ret && errno) {
    io_error(err, "Reading directory %s", d->path);
  }
  return ret;
}

void _openslide_dir_close(struct _openslide_dir *d) {
  if (d->dir) {
    g_dir_close(d->dir);
  }
  g_free(d->path);
  g_free(d);
}
