/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
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

#include <glib.h>
#include "openslide-common.h"

static char **fixed_argv;

static void free_argv(void) {
  g_strfreev(fixed_argv);
}

static gboolean parse_fail(GOptionContext* octx G_GNUC_UNUSED,
                           GOptionGroup* grp G_GNUC_UNUSED,
                           gpointer data G_GNUC_UNUSED,
                           GError** err) {
  g_set_error(err, G_OPTION_ERROR, G_OPTION_ERROR_FAILED, "boo!");
  return false;
}

void common_fix_argv(int *argc, char ***argv) {
  if (fixed_argv == NULL) {
#ifdef _WIN32
    fixed_argv = g_win32_get_command_line();
#else
    fixed_argv = g_strdupv(*argv);
#endif
    *argc = g_strv_length(fixed_argv);
    *argv = fixed_argv;
    atexit(free_argv);

    // Set prgname for g_get_prgname().  g_option_context_parse() has a bunch
    // of infrastructure to do this, which cannot be called independently.
    // Call g_option_context_parse() in a way that fails immediately after
    // setting prgname.
    g_autoptr(GOptionContext) octx = g_option_context_new(NULL);
    GOptionGroup *grp = g_option_group_new("", "", "", NULL, NULL);
    g_option_group_set_parse_hooks(grp, parse_fail, NULL);
    g_option_context_set_main_group(octx, grp);
    g_option_context_parse(octx, argc, argv, NULL);
  }
}

bool common_parse_options(GOptionContext *ctx,
                          int *argc, char ***argv,
                          GError **err) {
  // properly handle Unicode arguments on Windows
  common_fix_argv(argc, argv);
  bool ret = g_option_context_parse_strv(ctx, argv, err);
  *argc = g_strv_length(*argv);
  return ret;
}
