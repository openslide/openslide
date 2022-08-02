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

#include <config.h>

#ifdef WIN32
#define _WIN32_WINNT 0x0600
#include <windows.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <glib.h>

#ifdef HAVE_PROC_PIDFDINFO
// Mac OS X
#include <sys/param.h>  // MAXPATHLEN
#include <libproc.h>
#endif

#include "openslide-common.h"

char *common_get_fd_path(int fd) {
  struct stat st;
  if (fstat(fd, &st)) {
    return NULL;
  }

#if defined WIN32
  // Windows
  HANDLE hdl = (HANDLE) _get_osfhandle(fd);
  if (hdl != INVALID_HANDLE_VALUE) {
    DWORD size = GetFinalPathNameByHandle(hdl, NULL, 0, 0);
    if (size) {
      g_autofree char *path = g_malloc(size);
      DWORD ret = GetFinalPathNameByHandle(hdl, path, size - 1, 0);
      if (ret > 0 && ret <= size) {
        return g_steal_pointer(&path);
      }
    }
  }
#elif defined HAVE_PROC_PIDFDINFO
  // Mac OS X
  // Ignore kqueues, since they can be opened behind our back for
  // Grand Central Dispatch
  struct kqueue_fdinfo kqi;
  if (proc_pidfdinfo(getpid(), fd, PROC_PIDFDKQUEUEINFO, &kqi, sizeof(kqi))) {
    return NULL;
  }
  g_autofree char *path = g_malloc(MAXPATHLEN);
  if (!fcntl(fd, F_GETPATH, path)) {
    return g_steal_pointer(&path);
  }
#else
  // Fallback; works only on Linux
  g_autofree char *link_path = g_strdup_printf("/proc/%d/fd/%d", getpid(), fd);
  char *path = g_file_read_link(link_path, NULL);
  if (path) {
    return path;
  }
#endif

  return g_strdup("<unknown>");
}
