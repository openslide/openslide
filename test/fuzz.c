#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include "openslide.h"

int delete_file(const char*);
char *buf_to_file(const uint8_t*, size_t);

int delete_file(const char *pathname) {
  int ret = unlink(pathname);
  free((void *)pathname);
  return ret;
}

char *buf_to_file(const uint8_t *buf, size_t size) {
  char *pathname = strdup("/dev/shm/fuzz-XXXXXX");
  if (pathname == NULL) {
    return NULL;
  }

  int fd = mkstemp(pathname);
  if (fd == -1) {
    free(pathname);
    return NULL;
  }

  size_t pos = 0;
  while (pos < size) {
    int nbytes = write(fd, &buf[pos], size - pos);
    if (nbytes <= 0) {
      if (nbytes == -1 && errno == EINTR) {
        continue;
      }
      goto err;
    }
    pos += nbytes;
  }

  if (close(fd) == -1) {
    goto err;
  }

  return pathname;

err:
  delete_file(pathname);
  return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char *filename = buf_to_file(data, size);
    int w, h;
    if (!filename)
        exit(EXIT_FAILURE);

    openslide_t *slide = openslide_open(filename);
    if (slide) {
        const char *err = openslide_get_error(slide);
        if (!err) {
            openslide_get_level_count(slide);
            openslide_get_level0_dimensions(slide, &w, &h);
        }
    }

    // Cleanup
    openslide_close(slide);
    delete_file(filename);
    return 0;
}


