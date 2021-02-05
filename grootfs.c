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

#include <arpa/inet.h>
#include <dirent.h>
#include <err.h>
#include <fcntl.h>
#include <fuse.h>
#include <fuse_lowlevel.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/xattr.h>

typedef struct {
  int basefd;
} GRootFS;

#define ST_MODE_PERM_MASK (S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX)

static GRootFS *
get_grootfs (void)
{
  struct fuse_context *ctx = fuse_get_context ();
  return (GRootFS * )ctx->private_data;
}

static inline const char *
ensure_relpath (const char *path)
{
  path = path + strspn (path, "/");
  if (*path == 0)
    return ".";
  return path;
}

static int
open_dirfd (const char *path)
{
  GRootFS *fs = get_grootfs ();
  path = ensure_relpath (path);
  int dirfd = openat (fs->basefd, path, O_DIRECTORY | O_RDONLY, 0);
  if (dirfd == -1)
    return -errno;
  return dirfd;
}

static int
open_parent_dirfd (const char *path,
                   char **out_basename)
{
  int dirfd = -1;
  autofree char *dirpath = xstrdup (ensure_relpath (path));
  char *end = dirpath + strlen (dirpath);
  const char *dir;
  const char *base;

  /* Remove any slashes at the end */
  while (*end == '/' && end > dirpath)
    {
      *end = 0;
      end--;
    }

  /* Find first slash (not at end), if any */
  while (*end != '/' && end > dirpath)
    end--;

  if (end == dirpath)
    {
      base = dirpath;
      dir = ".";
    }
  else
    {
      base = end + 1;
      *end = 0;
      dir = dirpath;
    }

  dirfd = open_dirfd (dir);
  if (dirfd >= 0)
    *out_basename = xstrdup (base);
  return dirfd;
}

typedef enum {
  GROOTFS_FLAGS_UID_SET = 1<<0,
  GROOTFS_FLAGS_GID_SET = 1<<1,
  GROOTFS_FLAGS_MODE_SET = 1<<2,
} GrootFSFlags;

typedef struct {
  uint32_t flags;
  uint32_t uid;
  uint32_t gid;
  uint32_t mode;
} GRootFSData;

static char *
get_proc_fd_path (int dirfd,
                  const char *opt_file)
{
  if (opt_file)
    return xasprintf ("/proc/self/fd/%d/%s", dirfd, opt_file);
  else
    return xasprintf ("/proc/self/fd/%d", dirfd);
}

static int
get_fake_data (int dirfd,
               const char *file,
               int allow_noent,
               GRootFSData *data)
{
  autofree char *proc_file = get_proc_fd_path (dirfd, file);
  ssize_t res;

  res = lgetxattr (proc_file, "user.grootfs", data, sizeof(GRootFSData));
  if (res == -1)
    {
      int errsv = errno;

      /* Handle the case were there was no data set by returning all zeros */
      if ((allow_noent && errsv == ENOENT) ||
          errsv == ENODATA ||
          errsv == ENOTSUP)
        {
          GRootFSData zero = {0};
          *data = zero;
          return 0;
        }

      if (errsv == ERANGE)
        report ("Internal error: Wrong xattr size for file %s", file);
      else
        report ("Internal error: lgetxattr %s returned %s", file, strerror (errsv));

      return -errsv;
    }

  if (res != sizeof (GRootFSData))
    {
      report ("Internal error: Wrong xattr size for file %s", file);
      return -ERANGE;
    }

  data->flags = ntohl(data->flags);
  data->mode = ntohl(data->mode);
  data->uid = ntohl(data->uid);
  data->gid = ntohl(data->gid);

  return 0;
}

static int
get_fake_dataf (int fd,
               GRootFSData *data)
{
  ssize_t res;

  res = fgetxattr (fd, "user.grootfs", data, sizeof(GRootFSData));
  if (res == -1)
    {
      int errsv = errno;

      /* Handle the case were there was no data set by returning all zeros */
      if (errsv == ENODATA ||
          errsv == ENOTSUP)
        {
          GRootFSData zero = {0};
          *data = zero;
          return 0;
        }

      if (errsv == ERANGE)
        report ("Internal error: Wrong xattr size for fd %d", fd);
      else
        report ("Internal error: fgetxattr %d returned %s", fd, strerror (errsv));

      return -errsv;
    }

  if (res != sizeof (GRootFSData))
    {
      report ("Internal error: Wrong xattr size for fd %d", fd);
      return -ERANGE;
    }

  data->flags = ntohl(data->flags);
  data->mode = ntohl(data->mode);
  data->uid = ntohl(data->uid);
  data->gid = ntohl(data->gid);

  return 0;
}

static int
set_fake_data (int dirfd,
               const char *file,
               int allow_noent,
               GRootFSData *data)
{
  autofree char *proc_file = get_proc_fd_path (dirfd, file);
  ssize_t res;
  GRootFSData data2;

  data2.flags = htonl(data->flags);
  data2.mode = htonl(data->mode);
  data2.uid = htonl(data->uid);
  data2.gid = htonl(data->gid);

  res = lsetxattr (proc_file, "user.grootfs", &data2, sizeof(GRootFSData), 0);
  if (res == -1)
    {
      int errsv = errno;

      if (errsv == EPERM)
        {
          /* Symlinks cannot store user xattrs, so we don't store extra data for these */
          report ("Ignoring setxattr failing on symlink");
          return 0;
        }

      report ("Internal error: lsetxattr %s returned %s", file, strerror (errsv));
      return -errsv;
    }

  return 0;
}

static int
set_fake_dataf (int fd,
                GRootFSData *data)
{
  ssize_t res;
  GRootFSData data2;

  data2.flags = htonl(data->flags);
  data2.mode = htonl(data->mode);
  data2.uid = htonl(data->uid);
  data2.gid = htonl(data->gid);

  res = fsetxattr (fd, "user.grootfs", &data2, sizeof(GRootFSData), 0);
  if (res == -1)
    {
      int errsv = errno;

      if (errsv == EPERM)
        {
          /* Symlinks cannot store user xattrs, so we don't store extra data for these */
          report ("Ignoring setxattr failing on symlink");
          return 0;
        }

      report ("Internal error: fsetxattr %d returned %s", fd, strerror (errsv));
      return -errsv;
    }

  return 0;
}

static int
grootfs_getattr (const char *path, struct stat *st_data)
{
  __debug__ (("getattr %s\n", path));

  GRootFSData data = { 0 };
  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  if (fstatat(dirfd, basename, st_data, AT_SYMLINK_NOFOLLOW) == -1)
    return -errno;

  if (get_fake_data (dirfd, basename, TRUE, &data) != 0)
    return -EIO;

  if (data.flags & GROOTFS_FLAGS_UID_SET)
    st_data->st_uid = data.uid;

  if (data.flags & GROOTFS_FLAGS_GID_SET)
    st_data->st_gid = data.gid;

  if (data.flags & GROOTFS_FLAGS_MODE_SET)
    st_data->st_mode = (st_data->st_mode & (~ST_MODE_PERM_MASK)) | (data.mode & ST_MODE_PERM_MASK);

  return 0;
}

static int
grootfs_fgetattr (const char *path, struct stat *st_data, struct fuse_file_info *fi)
{
  __debug__ (("fgetattr %s\n", path));

  GRootFSData data = { 0 };
  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  if (fstat(fi->fh, st_data) == -1)
    return -errno;

  if (get_fake_dataf (fi->fh, &data) != 0)
    return -EIO;

  if (data.flags & GROOTFS_FLAGS_UID_SET)
    st_data->st_uid = data.uid;

  if (data.flags & GROOTFS_FLAGS_GID_SET)
    st_data->st_gid = data.gid;

  if (data.flags & GROOTFS_FLAGS_MODE_SET)
    st_data->st_mode = (st_data->st_mode & (~ST_MODE_PERM_MASK)) | (data.mode & ST_MODE_PERM_MASK);

  return 0;
}

/* In order to correctly do things like set permissions and
 * execute/search the files we set everything to rw for the user,
 * r-only for rest. For dirs we always set x, and mirror the user x
 * bit for other files. */
static mode_t
get_real_mode (int is_dir,
               int executable_default)
{
  mode_t real_mode = S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR;
  if (is_dir || executable_default)
    real_mode |= S_IXUSR | S_IXGRP | S_IXOTH;
  return real_mode;
}

static int
set_fake_chmod (int dirfd,
               const char *basename,
               mode_t mode)
{
  int res;
  GRootFSData data = { 0 };

  res = get_fake_data (dirfd, basename, FALSE, &data);
  if (res != 0)
    return res;

  data.mode = mode & ST_MODE_PERM_MASK;
  data.flags |= GROOTFS_FLAGS_MODE_SET;

  res = set_fake_data (dirfd, basename, FALSE, &data);
  if (res != 0)
    return res;

  return 0;
}

static int
grootfs_chmod (const char *path, mode_t mode)
{
  __debug__ (("chmod %s %x\n", path, mode));

  mode_t real_mode;
  struct stat st_data;
  int res;

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  if (fstatat (dirfd, basename, &st_data, AT_SYMLINK_NOFOLLOW) == -1)
    return -errno;

  real_mode = get_real_mode (S_ISDIR (st_data.st_mode), (mode & S_IXUSR) != 0);

  /* For permissions like execute to work for others, we set all the
     permissions, to the users perms and strip out any extra perms. */

  /* Note we can't use AT_SYMLINK_NOFOLLOW yet;
   * https://marc.info/?l=linux-kernel&m=148830147803162&w=2
   * https://marc.info/?l=linux-fsdevel&m=149193779929561&w=2
   */
  if (fchmodat (dirfd, basename, real_mode, 0) != 0)
    return -errno;

  res = set_fake_chmod (dirfd, basename, mode);
  if (res != 0)
    return res;

  return 0;
}

static int
grootfs_chown (const char *path, uid_t uid, gid_t gid)
{
  __debug__ (("chown %s to %d %d\n", path, uid, gid));
  GRootFSData data = { 0 };
  int res;

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  res = get_fake_data (dirfd, basename, FALSE, &data);
  if (res != 0)
    return res; /* This handles the ENOENT case */

  if (uid != -1)
    {
      data.uid = uid;
      data.flags |= GROOTFS_FLAGS_UID_SET;
    }

  if (gid != -1)
    {
      data.gid = gid;
      data.flags |= GROOTFS_FLAGS_GID_SET;
    }

  res = set_fake_data (dirfd, basename, FALSE, &data);
  if (res != 0)
    return res;

  return 0;
}

static int
grootfs_readlink (const char *path, char *buf, size_t size)
{
  GRootFS *fs = get_grootfs ();
  int r;

  __debug__ (("readlink %s\n", path));

  path = ensure_relpath (path);

  /* Note FUSE wants the string to be always nul-terminated, even if
   * truncated.
   */
  r = readlinkat (fs->basefd, path, buf, size - 1);
  if (r == -1)
    return -errno;
  buf[r] = '\0';
  return 0;
}

static int
grootfs_readdir (const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t offset, struct fuse_file_info *fi)
{
  GRootFS *fs = get_grootfs ();
  DIR *dp;
  struct dirent *de;
  int dfd;

  __debug__ (("readdir %s\n", path));

  path = ensure_relpath (path);

  if (!*path)
    {
      dfd = fcntl (fs->basefd, F_DUPFD_CLOEXEC, 3);
      if (dfd < 0)
        return -errno;
      lseek (dfd, 0, SEEK_SET);
    }
  else
    {
      dfd = openat (fs->basefd, path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
      if (dfd == -1)
        return -errno;
    }

  /* Transfers ownership of fd */
  dp = fdopendir (dfd);
  if (dp == NULL)
    return -errno;

  while ((de = readdir (dp)) != NULL)
    {
      struct stat st;
      memset (&st, 0, sizeof (st));
      st.st_ino = de->d_ino;
      // TODO: Ensure right mode if fake devnode/socket
      st.st_mode = de->d_type << 12;
      if (filler (buf, de->d_name, &st, 0))
        break;
    }

  (void) closedir (dp);
  return 0;
}

static int
grootfs_mknod (const char *path, mode_t mode, dev_t rdev)
{
  __debug__ (("mknod %s %ld %ld\n", path, (long)mode, (long)rdev));
  // TODO: Implement
  return -EROFS;
}

static int
grootfs_mkdir (const char *path, mode_t mode)
{
  GRootFS *fs = get_grootfs ();
  mode_t real_mode;
  int res;

  __debug__ (("mkdir %s %x\n", path, mode));

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  path = ensure_relpath (path);

  real_mode = get_real_mode (TRUE, FALSE);

  if (mkdirat (fs->basefd, path, real_mode) == -1)
    return -errno;

  res = set_fake_chmod (dirfd, basename, mode);
  if (res != 0)
    return res;

  return 0;
}

static int
grootfs_unlink (const char *path)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("unlink %s\n", path));

  path = ensure_relpath (path);

  if (unlinkat (fs->basefd, path, 0) == -1)
    return -errno;

  return 0;
}

static int
grootfs_rmdir (const char *path)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("rmdir %s\n", path));
  path = ensure_relpath (path);

  if (unlinkat (fs->basefd, path, AT_REMOVEDIR) == -1)
    return -errno;

  return 0;
}

static int
grootfs_symlink (const char *from, const char *to)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("symlink  %s %s\n", from, to));
  to = ensure_relpath (to);

  if (symlinkat (from, fs->basefd, to) == -1)
    return -errno;

  return 0;
}

static int
grootfs_rename (const char *from, const char *to)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("rename %s %s\n", from, to));
  from = ensure_relpath (from);
  to = ensure_relpath (to);

  if (renameat (fs->basefd, from, fs->basefd, to) == -1)
    return -errno;

  return 0;
}

static int
grootfs_link (const char *from, const char *to)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("link %s %s\n", from, to));
  from = ensure_relpath (from);
  to = ensure_relpath (to);

  if (linkat (fs->basefd, from, fs->basefd, to, 0) == -1)
    return -errno;

  return 0;
}

static int
grootfs_truncate (const char *path, off_t size)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("truncate %s\n", path));
  path = ensure_relpath (path);

  autofd int fd = openat (fs->basefd, path, O_NOFOLLOW|O_WRONLY);
  if (fd == -1)
    return -errno;

  if (ftruncate (fd, size) == -1)
    return -errno;

  return 0;
}

static int
grootfs_ftruncate (const char *path, off_t size, struct fuse_file_info *fi)
{
  __debug__ (("ftruncate %s\n", path));

  if (ftruncate (fi->fh, size) == -1)
    return -errno;

  return 0;
}

static int
grootfs_utimens (const char *path, const struct timespec tv[2])
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("utimens %s\n", path));
  path = ensure_relpath (path);

  if (utimensat (fs->basefd, path, tv, AT_SYMLINK_NOFOLLOW) == -1)
    return -errno;

  return 0;
}

static int
do_open (const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  GRootFS *fs = get_grootfs ();
  int fd;
  mode_t real_mode;
  int o_creat = (O_CREAT & finfo->flags) != 0;
  int o_excl = (O_EXCL & finfo->flags) != 0;
  int created_file = o_creat;
  int flags;

  // TODO: Rewrite path for fake devnodes, etc

  path = ensure_relpath (path);

  real_mode = get_real_mode (FALSE, (mode & S_IXUSR) != 0);

  flags = finfo->flags;
  if (o_creat && !o_excl)
    {
      flags |= O_EXCL; /* We really need to know if the file was created or not, so we try EXCL first */
    }

  fd = openat (fs->basefd, path, flags, real_mode);
  if (fd == -1 && o_creat && !o_excl && errno == EEXIST)
    {
      created_file = FALSE; /* We know the file existed */
      /* We faked the o_excl, and it exists, retry again witout forced o_excl */
      fd = openat (fs->basefd, path, finfo->flags, real_mode);
    }

  if (fd == -1)
    return -errno;

  if (created_file)
    {
      GRootFSData data = { 0 };
      int res;
      struct fuse_context *ctx = fuse_get_context ();

      data.mode = mode & ST_MODE_PERM_MASK;
      data.uid = ctx->uid;
      data.gid = ctx->gid;
      data.flags = GROOTFS_FLAGS_MODE_SET | GROOTFS_FLAGS_UID_SET | GROOTFS_FLAGS_GID_SET;

      res = set_fake_dataf (fd, &data);
      if (res != 0)
        {
          close (fd);
          return res;
        }
    }


  finfo->fh = fd;

  return 0;
}

static int
grootfs_open (const char *path, struct fuse_file_info *finfo)
{
  __debug__ (("open %s\n", path));
  return do_open (path, 0, finfo);
}

static int
grootfs_create(const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  __debug__ (("create %s\n", path));
  return do_open (path, mode, finfo);
}

static int
grootfs_read (const char *path, char *buf, size_t size, off_t offset,
               struct fuse_file_info *finfo)
{
  int r;

  r = pread (finfo->fh, buf, size, offset);
  if (r == -1)
    return -errno;
  return r;
}

static int
grootfs_write (const char *path, const char *buf, size_t size, off_t offset,
                struct fuse_file_info *finfo)
{
  int r;

  r = pwrite (finfo->fh, buf, size, offset);
  if (r == -1)
    return -errno;

  return r;
}

static int
grootfs_statfs (const char *path, struct statvfs *st_buf)
{
  GRootFS *fs = get_grootfs ();

  if (fstatvfs (fs->basefd, st_buf) == -1)
    return -errno;

  return 0;
}

static int
grootfs_release (const char *path, struct fuse_file_info *finfo)
{
  (void) close (finfo->fh);
  return 0;
}

static int
grootfs_fsync (const char *path, int crap, struct fuse_file_info *finfo)
{
  if (fsync (finfo->fh) == -1)
    return -errno;
  return 0;
}

static int
grootfs_access (const char *path, int mode)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("access %s\n", path));
  path = ensure_relpath (path);

  // TODO: Rewrite path for fake devnodes, etc

  /* Apparently at least GNU coreutils rm calls `faccessat(W_OK)`
   * before trying to do an unlink.  So...we'll just lie about
   * writable access here.
   */
  if (faccessat (fs->basefd, path, mode, AT_SYMLINK_NOFOLLOW) == -1)
    return -errno;
  return 0;
}

static int
grootfs_setxattr (const char *path, const char *name, const char *value,
                   size_t size, int flags)
{
  __debug__ (("setxattr %s %s\n", path, name));

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  autofree char *proc_file = get_proc_fd_path (dirfd, basename);
  autofree char *fake_name = xasprintf ("user.grootfs.%s", name);

  if (lsetxattr (proc_file, fake_name, value, size, flags) != 0)
    return -errno;

  return 0;
}

static int
grootfs_getxattr (const char *path, const char *name, char *value,
                   size_t size)
{
  __debug__ (("getxattr %s %s\n", path, name));

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  autofree char *proc_file = get_proc_fd_path (dirfd, basename);
  autofree char *fake_name = xasprintf ("user.grootfs.%s", name);

  if (lgetxattr (proc_file, fake_name, value, size) != 0)
    return -errno;

  return 0;
}

/*
 * List the supported extended attributes.
 */
static int
grootfs_listxattr (const char *path, char *list, size_t size)
{
  __debug__ (("listxattr %s\n", path));

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  autofree char *proc_file = get_proc_fd_path (dirfd, basename);
  char buf_data[4096];
  autofree char *buf_free = NULL;
  char *buf = buf_data;
  size_t buf_size = sizeof(buf_data);
  char *real_list, *real_list_end;
  ssize_t res;
  size_t fake_size;

  while (1)
    {
      res = llistxattr (proc_file, buf, buf_size);
      if (res != 0)
        {
          int errsv = errno;

          if (errsv == ERANGE)
            {
              if (buf_free)
                free (buf_free);
              buf_size *= 2;
              buf_free = xmalloc (buf_size);
              buf = buf_free;
              continue;
            }

          return -errsv;
        }

      break;
    }

  fake_size = 0;
  real_list = buf;
  real_list_end = real_list + res;

  while (real_list < real_list_end)
    {
      const char *name = real_list;
      real_list = real_list + strlen (name);

      if (has_prefix (name, "user.grootfs."))
        {
          name = name + strlen ("user.grootfs.");
          fake_size += strlen (name + 1);
        }
    }

  if (size == 0)
    return fake_size;

  if (size < fake_size)
    return -ERANGE;

  real_list = buf;
  while (real_list < real_list_end)
    {
      const char *name = real_list;
      real_list = real_list + strlen (name);

      if (has_prefix (name, "user.grootfs."))
        {
          name = name + strlen ("user.grootfs.");
          memcpy (list, name, strlen (name) + 1);
          list += strlen (name) + 1;
        }
    }

  return fake_size;
}

/*
 * Remove an extended attribute.
 */
static int
grootfs_removexattr (const char *path, const char *name)
{
  __debug__ (("removexattr %s %s\n", path, name));

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  autofree char *proc_file = get_proc_fd_path (dirfd, basename);
  autofree char *fake_name = xasprintf ("user.grootfs.%s", name);

  if (lremovexattr (proc_file, fake_name) != 0)
    return -errno;

  return 0;
}

static void *
grootfs_init (struct fuse_conn_info *conn)
{
  struct fuse_context *ctx = fuse_get_context ();
  GRootFS *fs = ctx->private_data;

  return fs;
}

static void
grootfs_destroy (void *private_data)
{
  GRootFS *fs = private_data;

  close (fs->basefd);
  free (fs);
}

struct fuse_operations grootfs_oper = {
  .init = grootfs_init,
  .destroy = grootfs_destroy,
  .getattr = grootfs_getattr,
  .fgetattr = grootfs_fgetattr,
  .readlink = grootfs_readlink,
  .readdir = grootfs_readdir,
  .mknod = grootfs_mknod,
  .mkdir = grootfs_mkdir,
  .symlink = grootfs_symlink,
  .unlink = grootfs_unlink,
  .rmdir = grootfs_rmdir,
  .rename = grootfs_rename,
  .link = grootfs_link,
  .chmod = grootfs_chmod,
  .chown = grootfs_chown,
  .truncate = grootfs_truncate,
  .ftruncate = grootfs_ftruncate,
  .utimens = grootfs_utimens,
  .create = grootfs_create,
  .open = grootfs_open,
  .read = grootfs_read,
  .write = grootfs_write,
  .statfs = grootfs_statfs,
  .release = grootfs_release,
  .fsync = grootfs_fsync,
  .access = grootfs_access,
  .setxattr = grootfs_setxattr,
  .getxattr = grootfs_getxattr,
  .listxattr = grootfs_listxattr,
  .removexattr = grootfs_removexattr,
};

static GRootFS *
new_grootfs (int basefd)
{
  GRootFS *fs = xmalloc (sizeof (GRootFS));
  fs->basefd = basefd;
  return fs;
}

int
start_grootfs (int argc, char *argv[], int dirfd)
{
  GRootFS *fs = new_grootfs (dirfd);

  return fuse_main (argc, argv, &grootfs_oper, fs);
}

static int
dev_fuse_chan_receive (struct fuse_chan **chp,
                       char *buf,
                       size_t size)
{
  struct fuse_chan *ch = *chp;
  struct fuse_session *se = fuse_chan_session (ch);
  ssize_t res;
  int err;

 restart:
  res = read (fuse_chan_fd (ch), buf, size);
  err = errno;

  if (fuse_session_exited (se))
    return 0;

  if (res == -1)
    {
      /* ENOENT means the operation was interrupted, it's safe to restart */
      if (err == ENOENT)
        goto restart;

      if (err == ENODEV) /* Happens on unmount */
        {
          fuse_session_exit (se);
          return 0;
        }
      if (err != EINTR && err != EAGAIN)
        report ("reading fuse device");
      return -err;
    }

  return res;
}

static int
dev_fuse_chan_send (struct fuse_chan *ch,
                    const struct iovec iov[],
                    size_t count)
{
  if (iov)
    {
      ssize_t res = writev (fuse_chan_fd (ch), iov, count);
      int err = errno;

      if (res == -1)
        {
          struct fuse_session *se = fuse_chan_session(ch);
          if (!fuse_session_exited(se) && err != ENOENT)
            report ("writing fuse device");
          return -err;
        }
    }

  return 0;
}

static void
dev_fuse_chan_destroy (struct fuse_chan *ch)
{
  int fd = fuse_chan_fd (ch);

  if (fd != -1)
    close (fd);
}

#define MIN_BUFSIZE 0x21000

struct fuse_chan *
dev_fuse_chan_new (int fd)
{
  struct fuse_chan_ops op = {
    .receive = dev_fuse_chan_receive,
    .send = dev_fuse_chan_send,
    .destroy = dev_fuse_chan_destroy,
  };

  size_t bufsize = getpagesize() + 0x1000;
  bufsize = bufsize < MIN_BUFSIZE ? MIN_BUFSIZE : bufsize;
  return fuse_chan_new (&op, fd, bufsize, NULL);
}


int
start_grootfs_lowlevel (int dirfd,
                        int dev_fuse,
                        char *mountpoint)
{
  char *argv[] = { mountpoint };
  struct fuse_args args = FUSE_ARGS_INIT(N_ELEMENTS (argv), argv);
  int status_pipes[2];
  char pipe_buf = 'x';
  pid_t pid;
  int res;

  if (pipe (status_pipes) != 0)
    {
      report ("failed to create status_pipes");
      return -1;
    }

  pid = fork ();
  if (pid == -1)
    {
      report ("failed to fork fuse process");
      return -1;
    }

  if (pid != 0)
    {
      ssize_t s;

      close (status_pipes[1]); /* Close write side */
      close (dirfd); /* Not needed on this side */
      close (dev_fuse); /* Not needed on this side */

      /* Wait for child process and report status */

      do
        s = read (status_pipes[0], &pipe_buf, 1);
      while (s == -1 && errno == EINTR);

      if (s == -1)
        {
          report ("Failed to read grootfs pipe");
          return -1;
        }

      if (s == 0)
        {
          // Short read, something failed and the child exited
          // If known error we already printed something
          return -1;
        }

      return 0;
    }

  /* Continue in child process */

  close (status_pipes[0]); /* Close read side */

  struct fuse_chan *ch = dev_fuse_chan_new (dev_fuse);
  if (ch == NULL)
    die ("Unable to create fuse channel");

  GRootFS *fs = new_grootfs (dirfd);
  struct fuse *fuse = fuse_new (ch, &args, &grootfs_oper, sizeof (grootfs_oper), fs);

  res = fuse_set_signal_handlers (fuse_get_session (fuse));
  if (res == -1)
    die ("Failed to set fuse cleanup signal handlers");

  (void) write (status_pipes[1], &pipe_buf, 1);

  res = fuse_loop (fuse);

  fuse_teardown (fuse, mountpoint);

  if (res == -1)
    die ("Error handling fuse requests");

  exit (0);
}
