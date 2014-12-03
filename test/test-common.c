/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
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

#ifdef WIN32
#define _WIN32_WINNT 0x0600
#include <windows.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>

#ifdef F_GETPATH
// Mac OS X: MAXPATHLEN
#include <sys/param.h>
#endif

#include "test-common.h"

char *get_fd_path(int fd) {
  struct stat st;
  char *path = NULL;

  if (fstat(fd, &st)) {
    return NULL;
  }

#if defined WIN32
  // Windows
  HANDLE hdl = (HANDLE) _get_osfhandle(fd);
  if (hdl != INVALID_HANDLE_VALUE) {
    DWORD size = GetFinalPathNameByHandle(hdl, NULL, 0, 0);
    if (size) {
      path = g_malloc(size);
      DWORD ret = GetFinalPathNameByHandle(hdl, path, size - 1, 0);
      if (!ret || ret > size) {
        g_free(path);
        path = NULL;
      }
    }
  }
#elif defined F_GETPATH
  // Mac OS X
  path = g_malloc(MAXPATHLEN);
  if (fcntl(fd, F_GETPATH, path)) {
    g_free(path);
    path = NULL;
  }
#else
  // Fallback; works only on Linux
  char *link_path = g_strdup_printf("/proc/%d/fd/%d", getpid(), fd);
  path = g_file_read_link(link_path, NULL);
  g_free(link_path);
#endif

  if (!path) {
    path = g_strdup("<unknown>");
  }
  return path;
}
