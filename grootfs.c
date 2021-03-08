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
#include <limits.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/xattr.h>

typedef struct {
  int basefd;
  long max_uid;
  long max_gid;
} GRootFS;

#define ST_MODE_PERM_MASK (S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX)
#define GROOT_CUSTOM_XATTR_PREFIX "user.grootfs."
#define GROOT_DATA_XATTR "user.grootfs"

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

static char *
get_proc_fd_path (int dirfd,
                  const char *opt_file)
{
  if (opt_file)
    return xasprintf ("/proc/self/fd/%d/%s", dirfd, opt_file);
  else
    return xasprintf ("/proc/self/fd/%d", dirfd);
}

/* Computes the real file pemissions for a a faked file.
 * In order to correctly do things like set permissions and
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

static void
fake_data_htonl (const GRootFSData *data,
                 GRootFSData *data_dst)
{
  data_dst->flags = htonl (data->flags);
  data_dst->mode = htonl (data->mode);
  data_dst->uid = htonl (data->uid);
  data_dst->gid = htonl (data->gid);
}

static void
fake_data_ntohl (const GRootFSData *data,
                 GRootFSData *data_dst)
{
  data_dst->flags = ntohl (data->flags);
  data_dst->mode = ntohl (data->mode);
  data_dst->uid = ntohl (data->uid);
  data_dst->gid = ntohl (data->gid);
}

static void
apply_fake_data (struct stat *st_data,
                 const GRootFSData *data)
{
  GRootFS *fs = get_grootfs ();

  if (data->flags & GROOTFS_FLAGS_UID_SET)
    st_data->st_uid = data->uid;

  if (data->flags & GROOTFS_FLAGS_GID_SET)
    st_data->st_gid = data->gid;

  if (data->flags & GROOTFS_FLAGS_MODE_SET)
    st_data->st_mode = (st_data->st_mode & (~ST_MODE_PERM_MASK)) | (data->mode & ST_MODE_PERM_MASK);

  /* Don't expose nobody user or other weird things not useful in the namespace */

  if (st_data->st_uid > fs->max_uid)
    st_data->st_uid = 0;

  if (st_data->st_gid > fs->max_gid)
    st_data->st_gid = 0;
}

static int
get_fake_data (int dirfd,
               const char *file,
               int allow_noent,
               GRootFSData *data)
{
  autofree char *proc_file = get_proc_fd_path (dirfd, file);
  ssize_t res;

  res = lgetxattr (proc_file, "user.grootfs", data, sizeof (GRootFSData));
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

  fake_data_ntohl (data, data);

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

  fake_data_ntohl (data, data);

  return 0;
}

static int
set_fake_data (int dirfd,
               const char *file,
               int ensure_exist,
               const GRootFSData *data)
{
  autofree char *proc_file = get_proc_fd_path (dirfd, file);
  ssize_t res;
  GRootFSData data2;

  fake_data_htonl (data, &data2);

  if (ensure_exist)
    {
      int fd = openat (dirfd, file, O_CREAT | O_EXCL | O_WRONLY, 0666);
      if (fd == -1)
        {
          if (errno != EEXIST)
            return -errno;
        }

      close (fd);
    }

  res = lsetxattr (proc_file, "user.grootfs", &data2, sizeof(GRootFSData), 0);
  if (res == -1)
    {
      int errsv = errno;
      report ("Internal error: lsetxattr %s returned %s", file, strerror (errsv));
      return -errsv;
    }

  return 0;
}

static int
set_fake_dataf (int fd,
                const GRootFSData *data)
{
  ssize_t res;
  GRootFSData data2;

  fake_data_htonl (data, &data2);

  res = fsetxattr (fd, "user.grootfs", &data2, sizeof(GRootFSData), 0);
  if (res == -1)
    {
      int errsv = errno;
      report ("Internal error: fsetxattr %d returned %s", fd, strerror (errsv));
      return -errsv;
    }

  return 0;
}

typedef struct {
  const char *path;
  int fd;
  int dirfd;
  char *basename;
  char *datafile;
  bool exists;
  struct stat st_data;
  GRootFSData fake_data;
} GRootPathInfo;

#define GROOT_PATH_INFO_INIT { NULL, -1, -1 }

static void
groot_path_info_cleanup (GRootPathInfo *info)
{
  if (info->dirfd != -1)
    close (info->dirfd);

  if (info->basename)
    free (info->basename);

  if (info->datafile)
    free (info->datafile);
}

DEFINE_AUTO_CLEANUP_CLEAR_FUNC(GRootPathInfo, groot_path_info_cleanup);

static int
_groot_path_info_init_base (GRootPathInfo *info, bool allow_noent)
{
  info->exists = TRUE;

  if (S_ISLNK (info->st_data.st_mode))
    {
      GRootFS *fs = get_grootfs ();
      info->datafile = xasprintf (".groot.symlink.%lx_%lx",
                                  info->st_data.st_dev, info->st_data.st_ino);

      if (get_fake_data (fs->basefd, info->datafile, TRUE, &info->fake_data) != 0)
        return -EIO;
    }
  else if (info->fd != -1)
    {
      if (get_fake_dataf (info->fd, &info->fake_data) != 0)
        return -EIO;
    }
  else
    {
      if (get_fake_data (info->dirfd, info->basename, allow_noent, &info->fake_data) != 0)
        return -EIO;
    }

  apply_fake_data (&info->st_data, &info->fake_data);

  return 0;
}

static int
groot_path_info_init_path (GRootPathInfo *info, const char *path, bool allow_noent)
{
  info->path = ensure_relpath (path);
  info->dirfd = open_parent_dirfd (info->path, &info->basename);
  if (info->dirfd < 0)
    return -errno;

  if (fstatat (info->dirfd, info->basename, &info->st_data, AT_SYMLINK_NOFOLLOW) == -1)
    {
      if (allow_noent)
        return 0;

      return -errno;
    }

  return _groot_path_info_init_base (info, allow_noent);
}

static int
groot_path_info_init_fd (GRootPathInfo *info, int fd)
{
  info->fd = fd;

  if (fstat (info->fd, &info->st_data) == -1)
    return -errno;

  return _groot_path_info_init_base (info, FALSE);
}

static int
groot_path_info_update_data (GRootPathInfo *info)
{
  assert (info->exists); // TODO: Later handle non-exist for e.g. symlinks

  if (info->datafile) /* A symlink with separate data file, could be path *or* fd backed */
    {
      GRootFS *fs = get_grootfs ();
      if (set_fake_data (fs->basefd, info->datafile, TRUE, &info->fake_data) != 0)
        return -EIO;
    }
  else if (info->fd != -1)
    {
      if (set_fake_dataf (info->fd, &info->fake_data) != 0)
        return -EIO;
    }
  else
    {
      if (set_fake_data (info->dirfd, info->basename, FALSE, &info->fake_data) != 0)
        return -EIO;
    }

  return 0;
}

static int
grootfs_getattr (const char *path, struct stat *st_data)
{
  __debug__ (("getattr %s", path));

  int res;
  auto(GRootPathInfo) info = GROOT_PATH_INFO_INIT;
  res = groot_path_info_init_path (&info, path, FALSE);
  if (res != 0)
    return res;

  *st_data = info.st_data;

  return 0;
}

static int
grootfs_fgetattr (const char *path, struct stat *st_data, struct fuse_file_info *fi)
{
  __debug__ (("fgetattr %s", path));

  int res;
  auto(GRootPathInfo) info = GROOT_PATH_INFO_INIT;
  res = groot_path_info_init_fd (&info, fi->fh);
  if (res != 0)
    return res;

  *st_data = info.st_data;

  return 0;
}

static int
grootfs_chmod (const char *path, mode_t mode)
{
  __debug__ (("chmod %s %x", path, mode));

  int res;
  auto(GRootPathInfo) info = GROOT_PATH_INFO_INIT;
  res = groot_path_info_init_path (&info, path, FALSE);
  if (res != 0)
    return res;

  mode_t real_mode = get_real_mode (S_ISDIR (info.st_data.st_mode), (mode & S_IXUSR) != 0);

  /* For permissions like execute to work for others, we set all the
     permissions, to the users perms and strip out any extra perms. */

  /* Note we can't use AT_SYMLINK_NOFOLLOW yet;
   * https://marc.info/?l=linux-kernel&m=148830147803162&w=2
   * https://marc.info/?l=linux-fsdevel&m=149193779929561&w=2
   * Fuse always resolves the symlink and calls us on the target
   */
  if (fchmodat (info.dirfd, info.basename, real_mode, 0) != 0)
    return -errno;

  info.fake_data.mode = mode & ST_MODE_PERM_MASK;
  info.fake_data.flags |= GROOTFS_FLAGS_MODE_SET;

  res = groot_path_info_update_data (&info);
  if (res != 0)
    return res;

  return 0;
}

static int
grootfs_chown (const char *path, uid_t uid, gid_t gid)
{
  __debug__ (("chown %s to %d %d", path, uid, gid));

  int res;
  auto(GRootPathInfo) info = GROOT_PATH_INFO_INIT;
  res = groot_path_info_init_path (&info, path, FALSE);
  if (res != 0)
    return res;

  if (uid != -1)
    {
      info.fake_data.uid = uid;
      info.fake_data.flags |= GROOTFS_FLAGS_UID_SET;
    }

  if (gid != -1)
    {
      info.fake_data.gid = gid;
      info.fake_data.flags |= GROOTFS_FLAGS_GID_SET;
    }

  res = groot_path_info_update_data (&info);
  if (res != 0)
    return res;

  return 0;
}

static int
grootfs_readlink (const char *path, char *buf, size_t size)
{
  GRootFS *fs = get_grootfs ();
  int r;

  __debug__ (("readlink %s", path));

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

  __debug__ (("readdir %s", path));

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

      if (has_prefix (de->d_name, ".groot."))
        continue;

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
  __debug__ (("mknod %s %ld %ld", path, (long)mode, (long)rdev));
  // TODO: Implement
  return -EROFS;
}

static int
grootfs_mkdir (const char *path, mode_t mode)
{
  __debug__ (("mkdir %s %x", path, mode));

  int res;
  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  mode_t real_mode = get_real_mode (TRUE, FALSE);

  if (mkdirat (dirfd, basename, real_mode) == -1)
    return -errno;

  /* mkdir succeeded, so its guaranteed to be a not-previously
     existing dir, just set the fake data */
  GRootFSData data = { 0 };
  struct fuse_context *ctx = fuse_get_context ();

  data.mode = mode & ST_MODE_PERM_MASK;
  data.uid = ctx->uid;
  data.gid = ctx->gid;
  data.flags = GROOTFS_FLAGS_MODE_SET | GROOTFS_FLAGS_UID_SET | GROOTFS_FLAGS_GID_SET;

  res = set_fake_data (dirfd, basename, FALSE, &data);
  if (res != 0)
    return res;

  return 0;
}

static int
grootfs_unlink (const char *path)
{
  __debug__ (("unlink %s", path));

  int res;
  auto(GRootPathInfo) info = GROOT_PATH_INFO_INIT;
  res = groot_path_info_init_path (&info, path, FALSE);
  if (res != 0)
    return res;

  if (unlinkat (info.dirfd, info.basename, 0) == -1)
    return -errno;

  /* When unlinking a symlink, also unlink symlink datafile.
   * This should be fine because we can't hardlink symlinks, so its the last reference. */
  if (info.datafile)
    {
      GRootFS *fs = get_grootfs ();
      unlinkat (fs->basefd, info.datafile, 0);
    }

  return 0;
}

static int
grootfs_rmdir (const char *path)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("rmdir %s", path));
  path = ensure_relpath (path);

  if (unlinkat (fs->basefd, path, AT_REMOVEDIR) == -1)
    return -errno;

  return 0;
}

static int
grootfs_symlink (const char *from, const char *to)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("symlink  %s %s", from, to));
  to = ensure_relpath (to);

  if (symlinkat (from, fs->basefd, to) == -1)
    return -errno;

  /* We created a new symlink file, set default ownership */
  auto(GRootPathInfo) info = GROOT_PATH_INFO_INIT;
  if (groot_path_info_init_path (&info, to, FALSE) == 0)
    {
      struct fuse_context *ctx = fuse_get_context ();

      info.fake_data.uid = ctx->uid;
      info.fake_data.gid = ctx->gid;
      info.fake_data.flags = GROOTFS_FLAGS_UID_SET | GROOTFS_FLAGS_GID_SET;

      groot_path_info_update_data (&info);
    }

  return 0;
}

static int
grootfs_rename (const char *from, const char *to)
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("rename %s %s", from, to));
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

  __debug__ (("link %s %s", from, to));
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

  __debug__ (("truncate %s", path));
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
  __debug__ (("ftruncate %s", path));

  if (ftruncate (fi->fh, size) == -1)
    return -errno;

  return 0;
}

static int
grootfs_utimens (const char *path, const struct timespec tv[2])
{
  GRootFS *fs = get_grootfs ();

  __debug__ (("utimens %s", path));
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
  __debug__ (("open %s", path));
  return do_open (path, 0, finfo);
}

static int
grootfs_create(const char *path, mode_t mode, struct fuse_file_info *finfo)
{
  __debug__ (("create %s", path));
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

  __debug__ (("access %s", path));
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
  __debug__ (("setxattr %s %s", path, name));

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  autofree char *proc_file = get_proc_fd_path (dirfd, basename);
  autofree char *fake_name = xasprintf (GROOT_CUSTOM_XATTR_PREFIX"%s", name);

  if (lsetxattr (proc_file, fake_name, value, size, flags) != 0)
    return -errno;

  return 0;
}

static int
grootfs_getxattr (const char *path, const char *name, char *value,
                   size_t size)
{
  __debug__ (("getxattr %s %s", path, name));

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  ssize_t res;
  if (dirfd < 0)
    return dirfd;

  autofree char *proc_file = get_proc_fd_path (dirfd, basename);
  autofree char *fake_name = xasprintf (GROOT_CUSTOM_XATTR_PREFIX"%s", name);

  res = lgetxattr (proc_file, fake_name, value, size);
  if (res == -1)
    return -errno;

  return res;
}

/*
 * List the supported extended attributes.
 */
static int
grootfs_listxattr (const char *path, char *list, size_t size)
{
  __debug__ (("listxattr %s", path));

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
      if (res < 0)
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
      real_list = real_list + strlen (name) + 1;

      if (has_prefix (name, GROOT_CUSTOM_XATTR_PREFIX))
        {
          name = name + strlen (GROOT_CUSTOM_XATTR_PREFIX);
          fake_size += strlen (name) + 1;
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
      real_list = real_list + strlen (name) + 1;

      if (has_prefix (name, GROOT_CUSTOM_XATTR_PREFIX))
        {
          name = name + strlen (GROOT_CUSTOM_XATTR_PREFIX);
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
  __debug__ (("removexattr %s %s", path, name));

  autofree char *basename = NULL;
  autofd int dirfd = open_parent_dirfd (path, &basename);
  if (dirfd < 0)
    return dirfd;

  autofree char *proc_file = get_proc_fd_path (dirfd, basename);
  autofree char *fake_name = xasprintf (GROOT_CUSTOM_XATTR_PREFIX"%s", name);

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
new_grootfs (int basefd,
             long max_uid,
             long max_gid)
{
  GRootFS *fs = xmalloc (sizeof (GRootFS));
  fs->basefd = basefd;
  fs->max_uid = max_uid;
  fs->max_gid = max_gid;
  return fs;
}

int
start_grootfs (int argc,
               char *argv[],
               int dirfd)
{
  GRootFS *fs = new_grootfs (dirfd, LONG_MAX, LONG_MAX);

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


static struct fuse_session *fuse_instance;

static void
exit_handler (int sig)
{
  __debug__ (("grootfs got signal %d", sig));

  (void) sig;
  if (fuse_instance)
    fuse_session_exit (fuse_instance);
}

static void
set_one_signal_handler (int sig,
                        void (*handler)(int),
                        int remove)
{
  struct sigaction sa;
  struct sigaction old_sa;

  memset (&sa, 0, sizeof(struct sigaction));
  sa.sa_handler = remove ? SIG_DFL : handler;
  sigemptyset (&(sa.sa_mask));
  sa.sa_flags = 0;

  if (sigaction (sig, NULL, &old_sa) == -1)
    die ("cannot get old signal handler");

  if (old_sa.sa_handler == (remove ? handler : SIG_DFL) &&
      sigaction (sig, &sa, NULL) == -1)
    die("cannot set signal handler");
}

static void
set_signal_handlers (struct fuse_session *se)
{
  set_one_signal_handler (SIGHUP, exit_handler, 0);
  set_one_signal_handler (SIGINT, exit_handler, 0);
  set_one_signal_handler (SIGTERM, exit_handler, 0);
  set_one_signal_handler (SIGPIPE, SIG_IGN, 0);
  fuse_instance = se;
}

int
start_grootfs_lowlevel (int dirfd,
                        int dev_fuse,
                        const char *mountpoint,
                        long max_uid,
                        long max_gid)
{
  const char *argv[] = { mountpoint };
  struct fuse_args args = FUSE_ARGS_INIT(N_ELEMENTS (argv), (char **)argv);
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

  GRootFS *fs = new_grootfs (dirfd, max_uid, max_gid);
  struct fuse *fuse = fuse_new (ch, &args, &grootfs_oper, sizeof (grootfs_oper), fs);

  set_signal_handlers (fuse_get_session (fuse));

  if (write (status_pipes[1], &pipe_buf, 1) < 0)
    report ("Failed write to status pipe");

  res = fuse_loop (fuse);

  /* Unmount even on failure */
  fuse_unmount (mountpoint, ch);

  if (res == -1)
    die ("Error handling fuse requests");

  fuse_destroy (fuse);

  __debug__ (("exiting grootfs"));

  exit (0);
}
