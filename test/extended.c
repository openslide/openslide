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

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <inttypes.h>

#ifndef WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

#include <glib.h>
#include <openslide.h>
#include "test-common.h"

static void fail(const char *str, ...) {
  va_list ap;

  va_start(ap, str);
  vfprintf(stderr, str, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  exit(1);
}

static void test_image_fetch(openslide_t *osr,
			     int64_t x, int64_t y,
			     int64_t w, int64_t h) {
  uint32_t *buf = g_new(uint32_t, w * h);
  for (int32_t level = 0; level < openslide_get_level_count(osr); level++) {
    openslide_read_region(osr, buf, x, y, level, w, h);
  }
  g_free(buf);

  const char *err = openslide_get_error(osr);
  if (err) {
    fail("Read failed: %"PRId64" %"PRId64" %"PRId64" %"PRId64": %s",
         x, y, w, h, err);
  }
}

#ifndef WIN32
static gint leak_test_running;  /* atomic ops only */

static gpointer cloexec_thread(const gpointer prog) {
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal,
        g_free, NULL);
  gchar *argv[] = {prog, "--leak-check--", NULL};

  while (g_atomic_int_get(&leak_test_running)) {
    gchar *out;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_LEAVE_DESCRIPTORS_OPEN |
          G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
          &out, NULL, NULL, NULL)) {
      g_assert_not_reached();
    }

    gchar **lines = g_strsplit(out, "\n", 0);
    for (gchar **line = lines; *line != NULL; line++) {
      if (**line == 0) {
        continue;
      }
      if (g_hash_table_lookup(seen, *line) == NULL) {
        fprintf(stderr, "Exec child received leaked fd to %s\n", *line);
        g_hash_table_insert(seen, g_strdup(*line), (void *) 1);
      }
    }
    g_strfreev(lines);
    g_free(out);
  }

  g_hash_table_destroy(seen);
  return NULL;
}

static void child_check_open_fds(void) {
  for (int i = 3; i < 128; i++) {
    gchar *path = get_fd_path(i);
    if (path != NULL) {
      printf("%s\n", path);
      g_free(path);
    }
  }
}

static void check_cloexec_leaks(const char *slide, void *prog,
                                int64_t x, int64_t y) {
  g_atomic_int_set(&leak_test_running, 1);
  GThread *thr = g_thread_create(cloexec_thread, prog, TRUE, NULL);
  g_assert(thr != NULL);
  guint32 buf[512 * 512];
  GTimer *timer = g_timer_new();
  while (g_timer_elapsed(timer, NULL) < 2) {
    openslide_t *osr = openslide_open(slide);
    openslide_read_region(osr, buf, x, y, 0, 512, 512);
    openslide_close(osr);
  }
  g_timer_destroy(timer);
  g_atomic_int_set(&leak_test_running, 0);
  g_thread_join(thr);
}
#else /* WIN32 */
static void child_check_open_fds(void) {}

static void check_cloexec_leaks(const char *slide G_GNUC_UNUSED,
                                void *prog G_GNUC_UNUSED,
                                int64_t x G_GNUC_UNUSED,
                                int64_t y G_GNUC_UNUSED) {}
#endif /* WIN32 */


int main(int argc, char **argv) {
  if (!g_thread_supported()) {
    g_thread_init(NULL);
  }

  if (argc != 2) {
    fail("No file specified");
  }
  const char *path = argv[1];

  if (g_str_equal(path, "--leak-check--")) {
    child_check_open_fds();
    return 0;
  }

  openslide_get_version();

  if (!openslide_detect_vendor(path)) {
    fail("No vendor for %s", path);
  }

  openslide_t *osr = openslide_open(path);
  if (!osr) {
    fail("Couldn't open %s", path);
  }
  const char *err = openslide_get_error(osr);
  if (err) {
    fail("Open failed: %s", err);
  }
  openslide_close(osr);

  osr = openslide_open(path);
  if (!osr || openslide_get_error(osr)) {
    fail("Reopen failed");
  }

  int64_t w, h;
  openslide_get_level0_dimensions(osr, &w, &h);

  int32_t levels = openslide_get_level_count(osr);
  for (int32_t i = -1; i < levels + 1; i++) {
    int64_t ww, hh;
    openslide_get_level_dimensions(osr, i, &ww, &hh);
    openslide_get_level_downsample(osr, i);
  }

  openslide_get_best_level_for_downsample(osr, 0.8);
  openslide_get_best_level_for_downsample(osr, 1.0);
  openslide_get_best_level_for_downsample(osr, 1.5);
  openslide_get_best_level_for_downsample(osr, 2.0);
  openslide_get_best_level_for_downsample(osr, 3.0);
  openslide_get_best_level_for_downsample(osr, 3.1);
  openslide_get_best_level_for_downsample(osr, 10);
  openslide_get_best_level_for_downsample(osr, 20);
  openslide_get_best_level_for_downsample(osr, 25);
  openslide_get_best_level_for_downsample(osr, 100);
  openslide_get_best_level_for_downsample(osr, 1000);
  openslide_get_best_level_for_downsample(osr, 10000);

  // NULL buffer
  openslide_read_region(osr, NULL, 0, 0, 0, 1000, 1000);

  // empty region
  openslide_read_region(osr, NULL, 0, 0, 0, 0, 0);

  // read properties
  const char * const *property_names = openslide_get_property_names(osr);
  while (*property_names) {
    const char *name = *property_names;
    openslide_get_property_value(osr, name);
    property_names++;
  }

  // read associated images
  const char * const *associated_image_names =
    openslide_get_associated_image_names(osr);
  while (*associated_image_names) {
    int64_t w, h;
    const char *name = *associated_image_names;
    openslide_get_associated_image_dimensions(osr, name, &w, &h);

    uint32_t *buf = g_new(uint32_t, w * h);
    openslide_read_associated_image(osr, name, buf);
    g_free(buf);

    associated_image_names++;
  }

  test_image_fetch(osr, -10, -10, 200, 200);
  test_image_fetch(osr, w/2, h/2, 500, 500);
  test_image_fetch(osr, w - 200, h - 100, 500, 400);
  test_image_fetch(osr, w*2, h*2, 400, 400);
  test_image_fetch(osr, w - 20, 0, 40, 100);
  test_image_fetch(osr, 0, h - 20, 100, 40);

  // active region
  const char *bounds_x = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BOUNDS_X);
  const char *bounds_y = openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_BOUNDS_Y);
  int64_t bounds_xx = 0;
  int64_t bounds_yy = 0;
  if (bounds_x && bounds_y) {
    bounds_xx = g_ascii_strtoll(bounds_x, NULL, 10);
    bounds_yy = g_ascii_strtoll(bounds_y, NULL, 10);
    test_image_fetch(osr, bounds_xx, bounds_yy, 200, 200);
  }

  openslide_close(osr);

  check_cloexec_leaks(path, argv[0], bounds_xx, bounds_yy);

  return 0;
}
