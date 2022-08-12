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
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include "openslide.h"
#include "openslide-common.h"
#include "config.h"

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#endif

#define MAX_FDS 128
#define TIME_ITERATIONS 5

static gchar *vendor_check;
static gchar **prop_checks;
static gchar **region_checks;
static gboolean time_check;

static bool have_error = false;

static void fail(const char *str, ...) {
  va_list ap;

  if (!have_error) {
    va_start(ap, str);
    vfprintf(stderr, str, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    have_error = true;
  }
}

static void print_log(const gchar *domain G_GNUC_UNUSED,
                      GLogLevelFlags level G_GNUC_UNUSED,
                      const gchar *message, void *data G_GNUC_UNUSED) {
  fprintf(stderr, "[log] %s\n", message);
  have_error = true;
}

static void check_error(openslide_t *osr) {
  const char *error = openslide_get_error(osr);
  if (error != NULL) {
    fail("%s", error);
  }
}

static bool in_valgrind(void) {
#ifdef HAVE_VALGRIND
  return RUNNING_ON_VALGRIND;
#else
  return false;
#endif
}

#define CHECK_RET(call, result)			\
  do {						\
    if (call != result) {			\
      fail("%s != %s", #call, #result);		\
    }						\
  } while (0)

#define CHECK_EMPTY_PTR_ARRAY(call)			\
  do {							\
    const void **ret = (const void **) call;		\
    if (ret == NULL || ret[0] != NULL) {		\
      fail("%s didn't return an empty array", #call);	\
    }							\
  } while (0)

#define CHECK_W_H(call, expected_w, expected_h)			\
  do {								\
    call;							\
    if (w != expected_w || h != expected_h) {			\
      fail("%s != (%s, %s)", #call, #expected_w, #expected_h);	\
    }								\
  } while (0)

static void check_api_failures(openslide_t *osr) {
  int64_t w, h;

  CHECK_RET(openslide_get_level_count(osr), -1);
  CHECK_W_H(openslide_get_level0_dimensions(osr, &w, &h), -1, -1);
  CHECK_W_H(openslide_get_level_dimensions(osr, 0, &w, &h), -1, -1);
  CHECK_W_H(openslide_get_level_dimensions(osr, 27, &w, &h), -1, -1);
  CHECK_W_H(openslide_get_level_dimensions(osr, -3, &w, &h), -1, -1);
  CHECK_RET(openslide_get_level_downsample(osr, 0), -1);
  CHECK_RET(openslide_get_level_downsample(osr, 27), -1);
  CHECK_RET(openslide_get_level_downsample(osr, -3), -1);
  CHECK_RET(openslide_get_best_level_for_downsample(osr, 0.8), -1);
  CHECK_RET(openslide_get_best_level_for_downsample(osr, 2), -1);
  CHECK_RET(openslide_get_best_level_for_downsample(osr, 4096), -1);
  CHECK_EMPTY_PTR_ARRAY(openslide_get_property_names(osr));
  CHECK_RET(openslide_get_property_value(osr, OPENSLIDE_PROPERTY_NAME_VENDOR),
            NULL);
  CHECK_EMPTY_PTR_ARRAY(openslide_get_associated_image_names(osr));
  CHECK_W_H(openslide_get_associated_image_dimensions(osr, "label", &w, &h),
            -1, -1);
  CHECK_W_H(openslide_get_associated_image_dimensions(osr, "macro", &w, &h),
            -1, -1);

  openslide_read_region(osr, NULL, 0, 0, 0, 10, 10);
  openslide_read_associated_image(osr, "label", NULL);
  openslide_read_associated_image(osr, "macro", NULL);
}

static void check_props(openslide_t *osr) {
  for (gchar **check = prop_checks; !have_error && check && *check; check++) {
    g_auto(GStrv) args = g_strsplit(*check, "=", 2);
    if (g_strv_length(args) != 2) {
      fail("Invalid property check: %s", *check);
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
  }
}

static void check_regions(openslide_t *osr) {
  for (gchar **check = region_checks; !have_error && check && *check;
       check++) {
    g_auto(GStrv) args = g_strsplit(*check, " ", 5);
    if (g_strv_length(args) != 5) {
      fail("Invalid region check: %s", *check);
      return;
    }
    int64_t x = g_ascii_strtoll(args[0], NULL, 10);
    int64_t y = g_ascii_strtoll(args[1], NULL, 10);
    int32_t level = g_ascii_strtoll(args[2], NULL, 10);
    int64_t w = g_ascii_strtoll(args[3], NULL, 10);
    int64_t h = g_ascii_strtoll(args[4], NULL, 10);

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
  {"time", 't', 0, G_OPTION_ARG_NONE, &time_check,
   "Report open time", NULL},
  {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

int main(int argc, char **argv) {
  GError *tmp_err = NULL;

  // Parse arguments
  g_autoptr(GOptionContext) ctx =
    g_option_context_new("SLIDE - try opening a slide file");
  g_option_context_add_main_entries(ctx, options, NULL);
  if (!common_parse_options(ctx, &argc, &argv, &tmp_err)) {
    fail("%s", tmp_err->message);
    g_clear_error(&tmp_err);
    return 2;
  }
  if (argc != 2) {
    fail("No slide specified");
    return 2;
  }
  const char *filename = argv[1];

  // Record preexisting file descriptors
  g_autoptr(GHashTable) fds = g_hash_table_new(g_direct_hash, g_direct_equal);
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
  bool can_open = openslide_can_open(filename);
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

  // Check can_open
  bool did_open = osr && openslide_get_error(osr) == NULL;
  if (can_open != did_open) {
    fail("openslide_can_open returned %d but openslide_open %s",
         can_open, did_open ? "succeeded" : "failed");
  }

  // Check for open errors
  if (osr != NULL) {
    const char *error = openslide_get_error(osr);
    if (error != NULL) {
      check_api_failures(osr);
      fail("%s", error);
    }
  } else if (!have_error) {
    // openslide_open returned NULL but logged nothing
    have_error = true;
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
    if (!g_hash_table_lookup(fds, GINT_TO_POINTER(i))) {
      g_autofree char *path = common_get_fd_path(i);
      if (path != NULL) {
        if (in_valgrind() && g_str_has_prefix(path, "pipe:")) {
          // valgrind likes to open pipes
          continue;
        }
        // leaked
        fprintf(stderr, "Leaked file descriptor to %s\n", path);
        have_error = true;
      }
    }
  }

  // Do timing run.  The earlier openslide_open() doesn't count because
  // it reads the slide data into the page cache.
  if (time_check && !have_error) {
    g_autoptr(GTimer) timer = NULL;

    // Average of TIME_ITERATIONS runs
    for (int i = 0; i < TIME_ITERATIONS; i++) {
      // Try open
      if (timer) {
        g_timer_continue(timer);
      } else {
        timer = g_timer_new();
      }
      osr = openslide_open(filename);
      g_timer_stop(timer);

      // Check for errors and clean up
      if (osr != NULL) {
        check_error(osr);
        openslide_close(osr);
      } else {
        fail("openslide_open() returned NULL during timing loop");
      }
      if (have_error) {
        break;
      }
    }

    // Report results
    if (!have_error) {
      printf("%d ms\n",
             (int) (1000 * g_timer_elapsed(timer, NULL) / TIME_ITERATIONS));
    }
  }

  return have_error ? 1 : 0;
}
