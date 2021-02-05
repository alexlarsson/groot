Groot is a tool that uses modern kernel features to emulate running a
process as root, without actually being root. It is similar to other
tools like fakeroot or pseudo.

It is used like sudo:

```
$ groot whoami
root
```

This is achieved using unprivileged user namespaces, which maps the
current users uid/gid to 0. In order to use uids other than 0 it uses
newuidmap/newgidmap, which means that the user needs to have blocks
allocated in `/etc/subuid` and `/etc/subgid` for non-0 uids and gids
to be usable in the groot.

The most common usecase for groot is to run some kind of distro
installation into a chroot, and to later use the files in the chroot
as a basis for an install. Normally a user cannot create files owned
by other users or with special permissions, so to support this groot
is able to wrap locations in the filesystem with a fuse-base
filesystem that fakes permissions and ownership of the files, storing
the faked metadata in xattrs on the underlying filesystem.

For example, on my Fedora system I am able to create a minimal chroot
and turn it into a tar like this:

```
$ mkdir rootfs
$ groot -w rootfs dnf -y --releasever 33 --repo fedora --installroot=`pwd`/rootfs install bash
$ groot -w rootfs dnf --installroot=`pwd`/rootfs clean all
$ groot -w rootfs tar cvf rootfs.tar.gz -C rootfs .
```

This will produce a directory `rootfs` where all the files are owned by the
current user, and a `rootfs.tar.gz` that records whatever the permissions
were set during the actual install.
