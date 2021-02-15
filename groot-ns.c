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

#include "utils.h"
#include "groot-ns.h"
#include "grootfs.h"

#include <pwd.h>
#include <sched.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <sys/eventfd.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/types.h>
#include <sys/wait.h>

static void
launch_newidmap (const char *bin, char **idmapping, pid_t main_pid)
{
  pid_t pid = fork ();
  int status;

  if (pid == -1)
    die_with_error ("fork failed");

  if (pid == 0)
    {
      autofree char **argv = xmalloc ((2 + strv_length (idmapping) + 1) * sizeof (char *));
      autofree char *pid_str = xasprintf ("%ld", main_pid);
      size_t i;

      argv[0] = (char *)bin;
      argv[1] = pid_str;
      for (i = 0; idmapping[i] != NULL; i++)
        argv[2+i] = idmapping[i];
      argv[2+i] = NULL;

      if (execvp (argv[0], argv) == -1)
        die_with_error ("exec newuidmap failed");

      exit (1);
    }

  if (waitpid (pid, &status, 0) == -1)
    die_with_error ("waitpid failed");

  if (!WIFEXITED (status))
    die_with_error ("%s did not exit", bin);

  if (WEXITSTATUS (status) != 0)
    die_with_error ("%s exited with status %d", bin, WEXITSTATUS (status));
}

static int
double_fork_with_socket (int *socket_out)
{
  pid_t pid, pid2;
  int status;
  int status_sockets[2] = { -1, -1 };

  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, status_sockets) != 0)
    die_with_error ("socketpair");

  pid = fork ();
  if (pid == -1)
    die_with_error ("fork failed");

  if (pid != 0)
    {
      close (status_sockets[1]);

      waitpid (pid, &status, 0); /* Don't leave zombies */

      *socket_out = status_sockets[0];
      return 1; /* In parent */
    }

  pid2 = fork ();
  if (pid2 == -1)
    die_with_error ("fork failed");

  if (pid2 != 0)
    exit (0);

  if (setsid () == -1)
    die_with_error ("setsid");

  close (status_sockets[0]);

  *socket_out = status_sockets[1];
  return 0; /* In grandchild */
}

static int
start_uidmap_process (pid_t main_pid,
                      char **uid_mapping,
                      char **gid_mapping)
{
  char buf = 'x';
  ssize_t s;
  int status_socket;

  if (double_fork_with_socket (&status_socket) != 0)
    return status_socket;

  /* Block until namespace started */
  do
    s = read (status_socket, &buf, 1);
  while (s == -1 && errno == EINTR);

  if (s == 1)
    {
      launch_newidmap ("newuidmap", uid_mapping, main_pid);
      launch_newidmap ("newgidmap", gid_mapping, main_pid);

      /* Signal that uidmaps are set up */
      if (write (status_socket, &buf, 1) < 0)
        report ("Failed write to status pipe");
    }

  exit (0);
}

static int
start_fuse_process (const char **wrapdirs,
                    int num_wrapdirs)
{
  char buf = 'x';
  int status_socket;
  int *wrapdir_fds;

  wrapdir_fds = xmalloc (sizeof (int) * num_wrapdirs);
  for (int i = 0; i < num_wrapdirs; i++)
    wrapdir_fds[i] = -1;

  /* We do the opens before forking for nicer error reporting */
  for (int i = 0; i < num_wrapdirs; i++)
    {
      const char *wrapdir = wrapdirs[i];

      wrapdir_fds[i] = openat (AT_FDCWD, wrapdir, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
      if (wrapdir_fds[i] == -1)
        wrapdirs[i] = NULL; /* Ensure we don't try to mount this */
    }

  if (double_fork_with_socket (&status_socket) != 0)
    {
      /* Close dirfds in parent */
      for (int i = 0; i < num_wrapdirs; i++)
        if (wrapdir_fds[i] != -1)
          close (wrapdir_fds[i]);
      return status_socket;
    }

  for (int i = 0; i < num_wrapdirs; i++)
    {
      const char *wrapdir = wrapdirs[i];
      int wrapdir_fd = wrapdir_fds[i];

      if (wrapdir_fd == -1)
        continue;

      int dev_fuse_fd = recv_fd (status_socket);
      if (dev_fuse_fd == -1)
        die_with_error ("no /dev/fuse fd recieved");

      if (start_grootfs_lowlevel (wrapdir_fd, dev_fuse_fd, wrapdir) != 0)
        die ("start_grootfs_lowlevel");
    }

  if (write (status_socket, &buf, 1) == -1)
    die ("fuse proc write socket_fd");

  exit (0);
}

static void
keep_caps (void)
{
  struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct data[2] = {{ 0 }};
  uint64_t effective, cap;

  if (capget (&header, data) < 0)
    die_with_error ("capget failed");

  effective = ((uint64_t)data[1].effective << 32) |  (uint64_t)data[0].effective;

  /* Make all caps inheritable */
  data[0].inheritable = data[0].permitted;
  data[1].inheritable = data[1].permitted;
  if (capset (&header, data) < 0)
    die_with_error ("capset failed");

  /* Make caps ambient */
  for (cap = 0; cap <= CAP_LAST_CAP; cap++)
    {
      if ((effective & (1 << cap)) &&
          prctl (PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0) != 0)
       {
         if (errno != EINVAL)
           die_with_error ("Adding ambient capability %ld", cap);
       }
    }
}

static char **
make_idmap (const char *username, const char *filename, long base_id)
{
  autofree char *content = NULL;
  autofreev char **mapping = NULL;
  size_t mapping_size;
  long next_id;

  mapping_size = 3;
  next_id = 1;
  mapping = xmalloc ((mapping_size + 1) * sizeof (char *));
  mapping[0] = xstrdup ("0");
  mapping[1] = xasprintf("%ld", base_id);
  mapping[2] = xstrdup ("1");
  mapping[3] = NULL;

  content = load_file_at (AT_FDCWD, filename);
  if (content)
    {
      char *line_iterator = content;

      while (line_iterator)
        {
          char *line = strsep (&line_iterator, "\n");
          char *end, *colon;

          if (!has_prefix (line, username) || line[strlen (username)] != ':')
            continue;
          line = line + strlen (username) + 1;

          colon = strchr (line, ':');
          if (colon == NULL)
            {
              report ("WARNING: Invalid format of %s", filename);
              continue; // Error parsing int
            }
          *colon = 0;

          long subid_base = strtol (line, &end, 10);
          if (*end != 0)
            {
              report ("WARNING: Invalid format of %s", filename);
              continue; // Error parsing int
            }

          long subid_count = strtol (colon + 1, &end, 10);
          if (*end != 0)
            {
              report ("WARNING: Invalid format of %s", filename);
              continue; // Error parsing int
            }

          mapping = xrealloc (mapping, (mapping_size + 3 + 1) * sizeof (char *));
          mapping[mapping_size++] = xasprintf("%ld", next_id);
          mapping[mapping_size++] = xasprintf("%ld", subid_base);
          mapping[mapping_size++] = xasprintf("%ld", subid_count);
          mapping[mapping_size] = NULL;
          next_id += subid_count;
        }
    }

  if (next_id == 1)
    report ("Warning: no defined ids for user %s in %s, limited user/group support", username, filename);

  return steal_pointer (&mapping);
}


static int
mount_fuse_fd_at (const char *mountpoint)
{
  autofree char *mountopts = NULL;
  int dev_fuse_fd;
  int res;

  dev_fuse_fd = open ("/dev/fuse", O_RDWR);
  if (dev_fuse_fd == -1)
    die_with_error ("Failed to open /dev/fuse");

  mountopts = xasprintf ("fd=%i,rootmode=%o,user_id=%u,group_id=%u,allow_other",
                         dev_fuse_fd, 0x4000, 0, 0);

  res = mount("fuse-grootfs", mountpoint, "fuse.fuse-grootfs", MS_NOSUID|MS_NODEV, mountopts);
  if (res != 0)
    die_with_error ("mount fuse");

  return dev_fuse_fd;
}

int
groot_setup_ns (const char **wrapdirs, int num_wrapdirs)
{
  autofd int fuse_status_socket = -1;
  autofd int uidmap_status_socket = -1;
  ssize_t s;
  struct passwd *passwd;
  const char *username = NULL;
  autofreev char **uid_mapping = NULL;
  autofreev char **gid_mapping = NULL;
  uid_t real_uid;
  gid_t real_gid;
  pid_t main_pid;
  int res;
  char buf = 'x';

  real_uid = getuid ();
  real_gid = getgid ();
  main_pid = getpid ();


  /* Avoid calling getpwuid() and thus nss, etc in a preload init constructor if possible */
  username = getenv ("GROOT_USER");
  if (username == NULL)
    {
      passwd = getpwuid (real_uid);
      if (passwd != NULL)
        username = xstrdup (passwd->pw_name);
    }

  uid_mapping = make_idmap (username, "/etc/subuid", real_uid);
  gid_mapping = make_idmap (username, "/etc/subgid", real_gid);

  if (num_wrapdirs > 0)
    fuse_status_socket = start_fuse_process (wrapdirs, num_wrapdirs);

  uidmap_status_socket = start_uidmap_process (main_pid, uid_mapping, gid_mapping);

  /* Never gain any more privs during exec */
  if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    die_with_error ("prctl(PR_SET_NO_NEW_PRIVS) failed");

  if (unshare(CLONE_NEWNS | CLONE_NEWUSER) != 0)
    die_with_error ("unshare failed");

  /* Wake up uidmap process */
  s = write (uidmap_status_socket, &buf, 1);
  if (s == -1)
    die ("write to status socket");

  /* Wait on uidmap process */
  do
    s = read (uidmap_status_socket, &buf, 1);
  while (s == -1 && errno == EINTR);

  if (s == 0)
    die ("Failed to setup uid/gid mappings");

  /* Then set up fuse mounts for the wraps, if needed */

  if (num_wrapdirs > 0)
    {
      for (int i = 0; i < num_wrapdirs; i++)
        {
          const char *wrapdir = wrapdirs[i];
          int dev_fuse_fd;

          if (wrapdir == NULL)
            continue; // We failed to open the dir, ignore

          dev_fuse_fd = mount_fuse_fd_at (wrapdir);

          res = send_fd (fuse_status_socket, dev_fuse_fd);
          if (res < 0)
            die_with_error ("send fd");

          close (dev_fuse_fd); /* Not used more on this side */
        }

      res = read (fuse_status_socket, &buf, sizeof (buf));
      if (res == -1)
        die_with_error ("read fs_socket");

      if (res == 0)
        die ("Fuse setup failed, exiting");
    }

  keep_caps ();

  return 0;
}
