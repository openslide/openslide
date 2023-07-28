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

// legacy commands are frozen; do not extend
const GOptionEntry legacy_opts[] = {
  {"version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show version", NULL},
  {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

static const struct command root_subcmds[] = {
  {
    .command = &assoc_cmd,
  },
  {
    .command = &prop_cmd,
  },
  {
    .command = &region_cmd,
  },
  {
    .command = &slide_cmd,
  },
  {}
};

static const GOptionEntry root_opts[] = {
  {"version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show version", NULL},
  {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}
};

static const struct command root_cmd = {
  .description = "Retrieve data from whole slide images.",
  .options = root_opts,
  .subcommands = root_subcmds,
};

static int usage(GOptionContext *octx) {
  g_autofree gchar *help = g_option_context_get_help(octx, true, NULL);
  fprintf(stderr, "%s", help);
  return 2;
}

static const struct command *canonicalize(const struct command *cmd) {
  while (cmd->command) {
    cmd = cmd->command;
  }
  return cmd;
}

static int invoke_cmdline(const struct command *cmd, char *parents,
                          int argc, char **argv) {
  GError *err = NULL;

  g_autofree char *param_str =
    g_strdup_printf("%s%s", parents,
                    cmd->parameter_string ? cmd->parameter_string : "");
  g_autoptr(GOptionContext) octx = g_option_context_new(param_str);
  if (cmd->options) {
    g_option_context_add_main_entries(octx, cmd->options, NULL);
  }
  g_autoptr(GString) summary = g_string_new(cmd->description);
  if (!cmd->description) {
    g_string_append_printf(summary, "%s.", cmd->summary);
  }
  if (cmd->subcommands) {
    g_string_append(summary, "\n\nSubcommands:");
    for (const struct command *subcmd_ = cmd->subcommands;
         canonicalize(subcmd_)->name; subcmd_++) {
      const struct command *subcmd = canonicalize(subcmd_);
      g_string_append_printf(summary, "\n  %-16s %s",
                             subcmd->name, subcmd->summary);
    }
    // stop at first non-option argument so we can invoke the subcommand
    // handler
    g_option_context_set_strict_posix(octx, true);
  }
  g_option_context_set_summary(octx, summary->str);

  common_parse_options(octx, &argc, &argv, &err);
  if (err) {
    common_warn("%s\n", err->message);
    g_error_free(err);
    return usage(octx);
  }

  if (show_version) {
    fprintf(stderr, version_format, g_get_prgname(), openslide_get_version());
    return 0;
  }

  if (cmd->subcommands) {
    if (argc < 2) {
      return usage(octx);
    }
    for (const struct command *subcmd_ = cmd->subcommands;
         canonicalize(subcmd_)->name; subcmd_++) {
      const struct command *subcmd = canonicalize(subcmd_);
      if (g_str_equal(subcmd->name, argv[1])) {
        g_autofree char *new_parents =
          g_strdup_printf("%s%s ", parents, argv[1]);
        g_free(argv[1]);
        // argc + 1 so we also move trailing NULL
        for (int i = 2; i < argc + 1; i++) {
          argv[i - 1] = argv[i];
        }
        return invoke_cmdline(subcmd, new_parents, argc - 1, argv);
      }
    }
    return usage(octx);
  }

  if (cmd->handler) {
    // Remove "--" arguments; g_option_context_parse() doesn't
    for (int i = 0; i < argc; i++) {
      if (g_str_equal(argv[i], "--")) {
        g_free(argv[i]);
        for (int j = i + 1; j <= argc; j++) {
          argv[j - 1] = argv[j];
        }
        --argc;
        --i;
      }
    }

    // drop argv[0]
    argc--;
    argv++;
    if (cmd->min_positional && argc < cmd->min_positional) {
      return usage(octx);
    }
    if (cmd->max_positional && argc > cmd->max_positional) {
      return usage(octx);
    }
    return cmd->handler(argc, argv);
  }

  g_assert_not_reached();
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
  const struct command *cmd = &root_cmd;
  if (g_str_equal(cmd_name, "openslide-quickhash1sum")) {
    cmd = &quickhash1sum_cmd;
  } else if (g_str_equal(cmd_name, "openslide-show-properties")) {
    cmd = &show_properties_cmd;
  } else if (g_str_equal(cmd_name, "openslide-write-png")) {
    cmd = &write_png_cmd;
  }
  return invoke_cmdline(cmd, "", argc, argv);
}
