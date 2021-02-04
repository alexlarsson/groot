PREFIX=/usr
BINDIR = ${PREFIX}/bin

FUSE_FLAGS=`pkg-config --cflags --libs fuse`

GROOT_CFLAGS=-D_GNU_SOURCE -D_FILE_OFFSET_BITS=64

groot: groot.c grootfs.c grootfs.h utils.h utils.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(GROOT_CFLAGS) $(FUSE_FLAGS) \
		groot.c grootfs.c grootfs.h utils.h utils.c \
		-o groot

fuse-grootfs: fuse-grootfs.c grootfs.c grootfs.h utils.h utils.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(GROOT_CFLAGS) $(FUSE_FLAGS) \
		fuse-grootfs.c grootfs.c grootfs.h utils.h utils.c  \
		-o fuse-grootfs

install: groot
	mkdir -p $(DESTDIR)$(BINDIR)
	install groot $(DESTDIR)$(BINDIR)/

clean:
	rm -f fuse-grootfs groot
