/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2012-2013 Carnegie Mellon University
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

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "openslide.h"

#define MAX_FDS 128

static gchar *vendor_check;
static gchar **prop_checks;
static gchar **region_checks;

static gboolean have_error = FALSE;

static void fail(const char *str, ...) {
  va_list ap;

  if (!have_error) {
    va_start(ap, str);
    vfprintf(stderr, str, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    have_error = TRUE;
  }
}

static void print_log(const gchar *domain G_GNUC_UNUSED,
                      GLogLevelFlags level G_GNUC_UNUSED,
                      const gchar *message, void *data G_GNUC_UNUSED) {
  fprintf(stderr, "[log] %s\n", message);
  have_error = TRUE;
}

static void check_error(openslide_t *osr) {
  const char *error = openslide_get_error(osr);
  if (error != NULL) {
    fail("%s", error);
  }
}

static void check_props(openslide_t *osr) {
  for (gchar **check = prop_checks; !have_error && check && *check; check++) {
    gchar **args = g_strsplit(*check, "=", 2);
    if (g_strv_length(args) != 2) {
      fail("Invalid property check: %s", *check);
      g_strfreev(args);
      return;
    }
    gchar *key = args[0];
    gchar *expected_value = args[1];

    const char *value = openslide_get_property_value(osr, key);
    check_error(osr);

    if (!*expected_value) {
      // value should be missing
      if (value != NULL) {
        fail("Property %s exists; should be missing", key);
      }
    } else {
      if (value == NULL) {
        fail("Property %s does not exist", key);
      } else if (strcmp(value, expected_value)) {
        fail("Property %s is %s; should be %s", key, value, expected_value);
      }
    }
    g_strfreev(args);
  }
}

static void check_regions(openslide_t *osr) {
  for (gchar **check = region_checks; !have_error && check && *check;
       check++) {
    gchar **args = g_strsplit(*check, " ", 5);
    if (g_strv_length(args) != 5) {
      fail("Invalid region check: %s", *check);
      g_strfreev(args);
      return;
    }
    int64_t x = g_ascii_strtoll(args[0], NULL, 10);
    int64_t y = g_ascii_strtoll(args[1], NULL, 10);
    int32_t level = g_ascii_strtoll(args[2], NULL, 10);
    int64_t w = g_ascii_strtoll(args[3], NULL, 10);
    int64_t h = g_ascii_strtoll(args[4], NULL, 10);
    g_strfreev(args);

    uint32_t *buf = g_slice_alloc(w * h * 4);
    openslide_read_region(osr, buf, x, y, level, w, h);
    check_error(osr);
    g_slice_free1(w * h * 4, buf);
  }
}

static GOptionEntry options[] = {
  {"vendor", 'n', 0, G_OPTION_ARG_STRING, &vendor_check,
   "Check for specified vendor (\"none\" for NULL)", "\"VENDOR\""},
  {"property", 'p', 0, G_OPTION_ARG_STRING_ARRAY, &prop_checks,
   "Check for specified property value", "\"NAME=VALUE\""},
  {"region", 'r', 0, G_OPTION_ARG_STRING_ARRAY, &region_checks,
   "Read specified region", "\"X Y LEVEL W H\""},
  {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

int main(int argc, char **argv) {
  GError *tmp_err = NULL;

  // Parse arguments
  GOptionContext *ctx =
    g_option_context_new("SLIDE - try opening a slide file");
  g_option_context_add_main_entries(ctx, options, NULL);
  if (!g_option_context_parse(ctx, &argc, &argv, &tmp_err)) {
    fail("%s", tmp_err->message);
    g_clear_error(&tmp_err);
    g_option_context_free(ctx);
    return 2;
  }
  g_option_context_free(ctx);
  if (argc != 2) {
    fail("No slide specified");
    return 2;
  }
  const char *filename = argv[1];

  // Record preexisting file descriptors
  GHashTable *fds = g_hash_table_new(g_direct_hash, g_direct_equal);
  for (int i = 0; i < MAX_FDS; i++) {
    struct stat st;
    if (!fstat(i, &st)) {
      g_hash_table_insert(fds, GINT_TO_POINTER(i), GINT_TO_POINTER(1));
    }
  }

  g_log_set_handler("Openslide", (GLogLevelFlags)
      (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING),
      print_log, NULL);

  const char *vendor = openslide_detect_vendor(filename);
  openslide_t *osr = openslide_open(filename);

  // Check vendor if requested
  if (vendor_check) {
    const char *expected_vendor = vendor_check;
    if (!strcmp(expected_vendor, "none")) {
      expected_vendor = NULL;
    }
    if ((expected_vendor == NULL) != (vendor == NULL) ||
        (vendor && strcmp(vendor, expected_vendor))) {
      fail("Detected vendor %s, expected %s",
           vendor ? vendor : "NULL",
           expected_vendor ? expected_vendor : "NULL");
    }
  }

  // Check for open errors
  if (osr != NULL) {
    check_error(osr);
  } else if (!have_error) {
    // openslide_open returned NULL but logged nothing
    have_error = TRUE;
  }

  if (osr != NULL) {
    // Check properties and regions
    check_props(osr);
    check_regions(osr);

    // Close
    openslide_close(osr);
  }

  // Check for file descriptor leaks
  for (int i = 0; i < MAX_FDS; i++) {
    struct stat st;
    if (!fstat(i, &st) && !g_hash_table_lookup(fds, GINT_TO_POINTER(i))) {
      // leaked
      char *link_path = g_strdup_printf("/proc/%d/fd/%d", getpid(), i);
      char *target = g_file_read_link(link_path, NULL);
      if (target == NULL) {
        target = g_strdup("<unknown>");
      }
      fprintf(stderr, "Leaked file descriptor to %s\n", target);
      have_error = TRUE;
      g_free(target);
      g_free(link_path);
    }
  }
  g_hash_table_destroy(fds);

  return have_error ? 1 : 0;
}
