/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2010 Carnegie Mellon University
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

#include "openslide.h"

static void usage(const char *progname) {
  printf("Usage: %s FILE...\n"
	 "Print OpenSlide quickhash-1 (256-bit) checksums.\n",
	 progname);
}

static void process(const char *progname, const char *file) {
  openslide_t *osr = openslide_open(file);
  if (osr == NULL) {
    fprintf(stderr, "%s: %s: Not a file that OpenSlide can recognize\n",
	    progname, file);
    fflush(stderr);
    return;
  }

  const char *err = openslide_get_error(osr);
  if (err) {
    fprintf(stderr, "%s: %s: %s\n", progname, file, err);
    fflush(stderr);
    openslide_close(osr);
    return;
  }

  const char *hash = openslide_get_property_value(osr,
        "openslide.quickhash-1");
  if (hash != NULL) {
    printf("%s  %s\n", hash, file);
  } else {
    fprintf(stderr, "%s: %s: No quickhash-1 available\n", progname, file);
    fflush(stderr);
  }

  openslide_close(osr);
}

int main (int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  for (int i = 1; i < argc; i++) {
    process(argv[0], argv[i]);
  }

  return 0;
}
