/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2026 Aaron Boxer
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

/*
 * Test the Grok JP2K backend via the synthetic test slide.
 *
 * Opens the synthetic slide with OPENSLIDE_JP2K_BACKEND=grok and reads
 * a region to verify the Grok decode path works end-to-end.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include "openslide.h"

#define REGION_SIZE 16

static bool test_backend(const char *backend_name) {
  printf("Testing with JP2K backend: %s\n", backend_name);

  if (backend_name) {
    g_setenv("OPENSLIDE_JP2K_BACKEND", backend_name, TRUE);
  } else {
    g_unsetenv("OPENSLIDE_JP2K_BACKEND");
  }

  /* the synthetic slide is opened via openslide_open("") when
     OPENSLIDE_DEBUG=synthetic is set */
  openslide_t *osr = openslide_open("");
  if (!osr) {
    fprintf(stderr, "  FAIL: openslide_open(\"\") returned NULL\n");
    return false;
  }

  const char *error = openslide_get_error(osr);
  if (error) {
    fprintf(stderr, "  FAIL: openslide error: %s\n", error);
    openslide_close(osr);
    return false;
  }

  /* read a small region */
  uint32_t *buf = g_new0(uint32_t, REGION_SIZE * REGION_SIZE);
  openslide_read_region(osr, buf, 0, 0, 0, REGION_SIZE, REGION_SIZE);

  error = openslide_get_error(osr);
  if (error) {
    fprintf(stderr, "  FAIL: read_region error: %s\n", error);
    g_free(buf);
    openslide_close(osr);
    return false;
  }

  /* verify we got non-zero pixels */
  bool has_nonzero = false;
  for (int i = 0; i < REGION_SIZE * REGION_SIZE; i++) {
    if (buf[i] != 0) {
      has_nonzero = true;
      break;
    }
  }
  if (!has_nonzero) {
    fprintf(stderr, "  FAIL: all pixels are zero\n");
    g_free(buf);
    openslide_close(osr);
    return false;
  }

  printf("  OK: read %dx%d region successfully\n", REGION_SIZE, REGION_SIZE);
  g_free(buf);
  openslide_close(osr);
  return true;
}

int main(void) {
  /* Note: OPENSLIDE_DEBUG=synthetic must be set BEFORE the library
     constructor runs.  Set it on the command line or via the test harness.
     The env var is parsed in the library's constructor. */

  /* test default (OpenJPEG) backend */
  if (!test_backend(NULL)) {
    return 1;
  }

  /* test Grok backend */
  if (!test_backend("grok")) {
    return 1;
  }

  printf("PASS: both backends work\n");
  return 0;
}
