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
#include "grootfs.h"

#include <fuse.h>

static char *base_path = NULL;

enum {
  KEY_HELP,
};

static void
usage (const char *progname)
{
  fprintf (stdout,
           "usage: %s basepath mountpoint [options]\n"
           "\n"
           "general options:\n"
           "   -o opt,[opt...]     mount options\n"
           "   -h  --help          print help\n"
           "\n", progname);
}

static int
grootfs_opt_proc (void *data,
                  const char *arg,
                  int key,
                  struct fuse_args *outargs)
{
  (void) data;

  switch (key)
    {
    case FUSE_OPT_KEY_NONOPT:
      if (base_path == NULL)
        {
          base_path = xstrdup (arg);
          return 0;
        }
      return 1;
    case FUSE_OPT_KEY_OPT:
      return 1;
    case KEY_HELP:
      usage (outargs->argv[0]);
      exit (EXIT_SUCCESS);
    default:
      fprintf (stderr, "see `%s -h' for usage\n", outargs->argv[0]);
      exit (EXIT_FAILURE);
    }
  return 1;
}

struct grootfs_config {
  int dummy;
};

#define GROOTFS_OPT(t, p, v) { t, offsetof(struct grootfs_config, p), v }

static struct fuse_opt grootfs_opts[] = {

  FUSE_OPT_KEY ("-h", KEY_HELP),
  FUSE_OPT_KEY ("--help", KEY_HELP),
  FUSE_OPT_END
};

int
main (int argc, char *argv[])
{
  struct fuse_args args = FUSE_ARGS_INIT (argc, argv);
  int res;
  int dirfd;
  struct grootfs_config conf = { 0 };

  res = fuse_opt_parse (&args, &conf, grootfs_opts, grootfs_opt_proc);
  if (res != 0)
    {
      fprintf (stderr, "Invalid arguments\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  if (base_path == NULL)
    {
      fprintf (stderr, "Missing basepath\n");
      fprintf (stderr, "see `%s -h' for usage\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  dirfd = openat (AT_FDCWD, base_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
  if (dirfd == -1)
    {
      perror ("opening basepath: ");
      exit (EXIT_FAILURE);
    }

  if (start_grootfs (args.argc, args.argv, dirfd) != 0)
    die ("Unable to start fuse filesystem");

  return 0;
}
