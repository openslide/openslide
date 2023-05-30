/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
 *  Copyright (c) 2015-2023 Benjamin Gilbert
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
#include "openslide-common.h"
#include "slidetool.h"
#include "config.h"

static const char *version_format = "%s " SUFFIXED_VERSION ", "
"using OpenSlide %s\n"
"Copyright (C) 2007-2023 Carnegie Mellon University and others\n"
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

static GOptionContext *make_option_context(const struct command *cmd) {
  GOptionContext *octx = g_option_context_new(cmd->parameter_string);
  g_option_context_set_summary(octx, cmd->summary);
  g_option_context_add_main_entries(octx, options, NULL);
  return octx;
}

void usage(const struct command *cmd) {
  g_autoptr(GOptionContext) octx = make_option_context(cmd);

  g_autofree gchar *help = g_option_context_get_help(octx, true, NULL);
  fprintf(stderr, "%s", help);

  exit(2);
}

void parse_commandline(const struct command *cmd, int *argc, char ***argv) {
  GError *err = NULL;

  g_autoptr(GOptionContext) octx = make_option_context(cmd);
  common_parse_options(octx, argc, argv, &err);

  if (err) {
    common_warn("%s\n", err->message);
    g_error_free(err);
    usage(cmd);

  } else if (show_version) {
    fprintf(stderr, version_format, g_get_prgname(), openslide_get_version());
    exit(0);
  }

  // Remove "--" arguments; g_option_context_parse() doesn't
  for (int i = 0; i < *argc; i++) {
    if (!strcmp((*argv)[i], "--")) {
      free((*argv)[i]);
      for (int j = i + 1; j <= *argc; j++) {
        (*argv)[j - 1] = (*argv)[j];
      }
      --*argc;
      --i;
    }
  }
}

static char *get_progname(void) {
  const char *prgname = g_get_prgname();
#ifdef _WIN32
  size_t len = strlen(prgname);
  char *ret = g_ascii_strdown(prgname, len);
  if (g_str_has_suffix(ret, ".exe")) {
    ret[len - 4] = 0;
  }
  return ret;
#else
  return g_strdup(prgname);
#endif
}

int main(int argc, char **argv) {
  // properly handle Unicode arguments on Windows; set prgname
  common_fix_argv(&argc, &argv);
  g_autofree char *cmd_name = get_progname();
  if (g_str_equal(cmd_name, "openslide-quickhash1sum")) {
    return do_quickhash1sum(argc, argv);
  } else if (g_str_equal(cmd_name, "openslide-write-png")) {
    return do_write_png(argc, argv);
  } else {
    return do_show_properties(argc, argv);
  }
}
