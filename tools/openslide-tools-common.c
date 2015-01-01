/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
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
#include <string.h>
#include <glib.h>
#include "openslide.h"
#include "openslide-tools-common.h"
#include "config.h"

static const char *version_format = "%s " SUFFIXED_VERSION ", "
"using OpenSlide %s\n"
"Copyright (C) 2007-2015 Carnegie Mellon University and others\n"
"\n"
"OpenSlide is free software: you can redistribute it and/or modify it under\n"
"the terms of the GNU Lesser General Public License, version 2.1.\n"
"<http://gnu.org/licenses/lgpl-2.1.html>\n"
"\n"
"OpenSlide comes with NO WARRANTY, to the extent permitted by law.  See the\n"
"GNU Lesser General Public License for more details.\n";


static gboolean show_version;

static const GOptionEntry options[] = {
  {"version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show version", NULL},
  {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};


static GOptionContext *make_option_context(const struct openslide_tools_usage_info *info) {
  GOptionContext *octx = g_option_context_new(info->parameter_string);
  g_option_context_set_summary(octx, info->summary);
  g_option_context_add_main_entries(octx, options, NULL);
  return octx;
}

void _openslide_tools_parse_commandline(const struct openslide_tools_usage_info *info,
                                        int *argc,
                                        char ***argv) {
  GError *err = NULL;

  GOptionContext *octx = make_option_context(info);
  g_option_context_parse(octx, argc, argv, &err);
  g_option_context_free(octx);

  if (err) {
    fprintf(stderr, "%s: %s\n\n", g_get_prgname(), err->message);
    g_error_free(err);
    _openslide_tools_usage(info);

  } else if (show_version) {
    fprintf(stderr, version_format, g_get_prgname(), openslide_get_version());
    exit(0);
  }

  // Remove "--" arguments; g_option_context_parse() doesn't
  for (int i = 0; i < *argc; i++) {
    if (!strcmp((*argv)[i], "--")) {
      for (int j = i + 1; j <= *argc; j++) {
        (*argv)[j - 1] = (*argv)[j];
      }
      --*argc;
      --i;
    }
  }
}

void _openslide_tools_usage(const struct openslide_tools_usage_info *info) {
  GOptionContext *octx = make_option_context(info);

  gchar *help = g_option_context_get_help(octx, TRUE, NULL);
  fprintf(stderr, "%s", help);
  g_free(help);

  g_option_context_free(octx);
  exit(2);
}
