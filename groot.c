/*
 * Copyright (C) 2021 Alexander Larsson <alexl@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.0+
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#define FUSE_USE_VERSION 26

#include "utils.h"
#include "groot-ns.h"

#include <fuse.h>

enum {
  KEY_HELP,
  KEY_WRAP,
  KEY_DEBUG
};


struct groot_config {
  char **wrapdirs;
  int num_wrapdirs;
  bool debug;
};

static void
usage (const char *progname)
{
  fprintf (stdout,
           "usage: %s [options] command [args..]\n"
           "\n"
           "options:\n"
           "   -h  --help          print help\n"
           "   -w DIR              wrap directory\n"
           "   -d                  log debug info\n"
           "\n", progname);
}

int opt_got_command = FALSE;

static void
add_wrap_dir (struct groot_config *conf,
              const char *path)
{
  conf->num_wrapdirs++;
  conf->wrapdirs = xrealloc (conf->wrapdirs, conf->num_wrapdirs * sizeof (char *));
  conf->wrapdirs[conf->num_wrapdirs-1] = xstrdup (path);
}

static int
groot_opt_proc (void *data,
                const char *arg,
                int key,
                struct fuse_args *outargs)
{
  struct groot_config *conf = data;

  /* Ignore everything after first command */
  if (opt_got_command)
    return 1;

  switch (key)
    {
    case FUSE_OPT_KEY_NONOPT:
      opt_got_command = TRUE;
      return 1;

    case KEY_HELP:
      usage (outargs->argv[0]);
      exit (EXIT_SUCCESS);

    case KEY_WRAP:
      add_wrap_dir (conf, arg + 2);
      return 0;

    case KEY_DEBUG:
      conf->debug = TRUE;
      return 0;

    default:
      fprintf (stderr, "see `%s -h' for usage\n", outargs->argv[0]);
      exit (EXIT_FAILURE);
    }

  return 1;
}


#define GROOT_OPT(t, p, v) { t, offsetof(struct groot_config, p), v }

static struct fuse_opt groot_opts[] = {

  FUSE_OPT_KEY ("-h", KEY_HELP),
  FUSE_OPT_KEY ("--help", KEY_HELP),
  FUSE_OPT_KEY ("-w ", KEY_WRAP),
  FUSE_OPT_KEY ("-d", KEY_DEBUG),
  FUSE_OPT_END
};

int
main (int argc, char *argv[])
{
  char **argv_clone;
  int res;
  struct fuse_args args = FUSE_ARGS_INIT (argc, argv);
  struct groot_config conf = { 0 };
  const char *env_wrap = NULL;

  res = fuse_opt_parse (&args, &conf, groot_opts, groot_opt_proc);
  if (res != 0)
    {
      fprintf (stderr, "Invalid arguments\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  if (!opt_got_command)
    {
      fprintf (stderr, "No command specified\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  env_wrap = getenv ("GROOT_WRAPFS");
  if (env_wrap)
    {
      autofree char *data = xstrdup (env_wrap);
      char *iterator = data;

      while (iterator)
        {
          char *path = strsep (&iterator, ":");
          add_wrap_dir (&conf, path);
        }
    }

  if (conf.debug)
    enable_debuglog ();

  groot_setup_ns ((const char **)conf.wrapdirs, conf.num_wrapdirs);

  argv_clone = xmalloc (sizeof(char *) * args.argc);
  for (int i = 1; i < args.argc; i++)
    argv_clone[i-1] = args.argv[i];
  argv_clone[args.argc-1] = NULL;

  if (execvp (argv_clone[0], argv_clone) == -1)
    die_with_error ("exec failed");

  return 1;
}
