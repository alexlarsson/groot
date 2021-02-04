/*
 * Copyright (C) 2020 Alexander Larsson <alexl@redhat.com>
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

#include <sys/socket.h>

void
report (const char *format, ...)
{
  va_list args;

  fprintf (stderr, "groot: ");

  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);

  fprintf (stderr, "\n");
}

void
die_with_error (const char *format, ...)
{
  va_list args;
  int errsv;

  errsv = errno;

  fprintf (stderr, "groot: ");

  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);

  fprintf (stderr, ": %s\n", strerror (errsv));

  exit (1);
}

void
die (const char *format, ...)
{
  va_list args;

  fprintf (stderr, "groot: ");

  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);

  fprintf (stderr, "\n");

  exit (1);
}

void
die_oom (void)
{
  fputs ("Out of memory\n", stderr);
  exit (1);
}

void *
xmalloc (size_t size)
{
  void *res = malloc (size);

  if (res == NULL)
    die_oom ();
  return res;
}

void *
xcalloc (size_t size)
{
  void *res = calloc (1, size);

  if (res == NULL)
    die_oom ();
  return res;
}

void *
xrealloc (void *ptr, size_t size)
{
  void *res = realloc (ptr, size);

  if (size != 0 && res == NULL)
    die_oom ();
  return res;
}

char *
xstrdup (const char *str)
{
  char *res;

  assert (str != NULL);

  res = strdup (str);
  if (res == NULL)
    die_oom ();

  return res;
}


void
strfreev (char **str_array)
{
  if (str_array)
    {
      int i;

      for (i = 0; str_array[i] != NULL; i++)
        free (str_array[i]);

      free (str_array);
    }
}

size_t
strv_length (char  **str_array)
{
  size_t i = 0;

  if (str_array != NULL)
    {
      while (str_array[i])
        ++i;
    }

  return i;
}


char *
xasprintf (const char *format,
           ...)
{
  char *buffer = NULL;
  va_list args;

  va_start (args, format);
  if (vasprintf (&buffer, format, args) == -1)
    die_oom ();
  va_end (args);

  return buffer;
}

int
has_prefix (const char *str,
            const char *prefix)
{
  return strncmp (str, prefix, strlen (prefix)) == 0;
}

/* Sets errno on error (== NULL),
 * Always ensures terminating zero */
char *
load_file_data (int     fd,
                size_t *size)
{
  autofree char *data = NULL;
  ssize_t data_read;
  ssize_t data_len;
  ssize_t res;

  data_read = 0;
  data_len = 4080;
  data = xmalloc (data_len);

  do
    {
      if (data_len == data_read + 1)
        {
          data_len *= 2;
          data = xrealloc (data, data_len);
        }

      do
        res = read (fd, data + data_read, data_len - data_read - 1);
      while (res < 0 && errno == EINTR);

      if (res < 0)
        return NULL;

      data_read += res;
    }
  while (res > 0);

  data[data_read] = 0;

  if (size)
    *size = (size_t) data_read;

  return steal_pointer (&data);
}

/* Sets errno on error (== NULL),
 * Always ensures terminating zero */
char *
load_file_at (int         dirfd,
              const char *path)
{
  int fd;
  char *data;
  int errsv;

  fd = openat (dirfd, path, O_CLOEXEC | O_RDONLY);
  if (fd == -1)
    return NULL;

  data = load_file_data (fd, NULL);

  errsv = errno;
  close (fd);
  errno = errsv;

  return data;

}

int
send_fd (int socket,
         int fd)
{
  struct msghdr msg = { 0 };
  struct cmsghdr *cmsg;
  char iobuf[1];
  struct iovec io = {
    .iov_base = iobuf,
    .iov_len = sizeof(iobuf)
  };
  char buf[CMSG_ALIGN(CMSG_SPACE(sizeof(int)))];

  msg.msg_iov = &io;
  msg.msg_iovlen = 1;
  msg.msg_control = buf;
  msg.msg_controllen = sizeof(buf);
  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

  return sendmsg (socket, &msg, 0);
}

int
recv_fd (int socket)
{
  ssize_t res;
  char iobuf[1];
  struct iovec io = {
    .iov_base = iobuf,
    .iov_len = sizeof(iobuf)
  };
  char buf[CMSG_ALIGN(CMSG_SPACE(sizeof(int)))];
  struct msghdr msg = {
    .msg_iov = &io,
    .msg_iovlen = 1,
    .msg_control = &buf,
    .msg_controllen = sizeof(buf),
  };
  struct cmsghdr *cmsg;
  int received_fd = -1;

  res = recvmsg (socket, &msg, 0);
  if (res < 0)
    return -1;

  for (cmsg = CMSG_FIRSTHDR(&msg);
       cmsg != NULL;
       cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
      if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        {
          memcpy (&received_fd, CMSG_DATA(cmsg), sizeof(int));
          break;
        }
    }

  if (received_fd < 0)
    {
      errno = ENOENT;
      return -1;
    }

  return received_fd;
}
