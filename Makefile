PREFIX=/usr
BINDIR = ${PREFIX}/bin
LIBDIR = ${PREFIX}/lib

FUSE_FLAGS=`pkg-config --cflags --libs fuse`

GROOT_WARN_CFLAGS=-Wall -Werror
GROOT_CFLAGS=-D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 $(GROOT_WARN_CFLAGS)

all: groot libgroot.so

groot: groot.c grootfs.c grootfs.h groot-ns.c groot-ns.h utils.h utils.c
	$(CC) groot.c grootfs.c groot-ns.c utils.c \
		$(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(GROOT_CFLAGS) $(FUSE_FLAGS) \
		-o groot

libgroot.so: groot-preload.c grootfs.c grootfs.h groot-ns.c groot-ns.h utils.h utils.c
	$(CC) groot-preload.c grootfs.c  groot-ns.c utils.c \
		$(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(GROOT_CFLAGS)  $(FUSE_FLAGS) \
		-fvisibility=hidden -Bsymbolic-functions -Bgroup -fPIC -shared -o libgroot.so

fuse-grootfs: fuse-grootfs.c grootfs.c grootfs.h utils.h utils.c
	$(CC) fuse-grootfs.c grootfs.c utils.c \
		$(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(GROOT_CFLAGS) $(FUSE_FLAGS) \
		-o fuse-grootfs

install: groot libgroot.so
	mkdir -p $(DESTDIR)$(BINDIR)
	install groot $(DESTDIR)$(BINDIR)/
	mkdir -p $(DESTDIR)$(LIBDIR)
	install libgroot.so $(DESTDIR)$(LIBDIR)/

clean:
	rm -f fuse-grootfs groot libgroot.so
