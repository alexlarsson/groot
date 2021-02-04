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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#define TRUE 1
#define FALSE 0

#ifdef DEBUGLOG
#define __debug__(x) printf x
#else
#define __debug__(x)
#endif

#define N_ELEMENTS(arr) (sizeof (arr) / sizeof ((arr)[0]))

void   die_with_error (const char  *format,
                       ...);
void   die            (const char  *format,
                       ...);
void   report         (const char  *format,
                       ...);
void   die_oom        (void);
void * xmalloc        (size_t       size);
void * xcalloc        (size_t       size);
void * xrealloc       (void        *ptr,
                       size_t       size);
void   strfreev       (char       **str_array);
size_t strv_length    (char       **str_array);
char * xstrdup        (const char  *str);
int    has_prefix     (const char  *str,
                       const char  *prefix);
char * xasprintf      (const char  *format,
                       ...);
char * load_file_data (int          fd,
                       size_t      *size);
char * load_file_at   (int          dirfd,
                       const char  *path);
int    send_fd        (int          socket,
                       int          fd);
int    recv_fd        (int          socket);

static inline int
steal_fd (int *fdp)
{
  int fd = *fdp;
  *fdp = -1;
  return fd;
}

static inline void *
steal_pointer (void *pp)
{
  void **ptr = (void **) pp;
  void *ref;

  ref = *ptr;
  *ptr = NULL;

  return ref;
}

/* type safety */
#define steal_pointer(pp) \
  (0 ? (*(pp)) : (steal_pointer) (pp))


static inline void
cleanup_fdp (int *fdp)
{
  int errsv;

  assert (fdp);

  int fd = steal_fd (fdp);
  if (fd >= 0)
    {
      errsv = errno;
      if (close (fd) < 0)
        assert (errno != EBADF);
      errno = errsv;
    }
}

static inline void
cleanup_strvp (void *p)
{
  void **pp = (void **) p;

  strfreev (*pp);
}

static inline void
cleanup_freep (void *p)
{
  void **pp = (void **) p;

  if (*pp)
    free (*pp);
}

#define autofd __attribute__((cleanup(cleanup_fdp)))
#define autofree __attribute__((cleanup(cleanup_freep)))
#define autofreev __attribute__((cleanup (cleanup_strvp)))
