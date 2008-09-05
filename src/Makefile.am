lib_LTLIBRARIES = libwholeslide.la

libwholeslide_la_LIBADD = $(GLIB2_LIBS)

libwholeslide_la_SOURCES = wholeslide.c \
	wholeslide-vendor-trestle.c wholeslide-vendor-hamamatsu.c \
	wholeslide-vendor-aperio.c \
	wholeslide-ops-tiff.c wholeslide-ops-jpeg.c

libwholeslide_la_CPPFLAGS = -DBUILDING_DLL -I/usr/include/openjpeg $(GLIB2_CFLAGS)

libwholeslide_la_LDFLAGS = -version-info 0:0:0


pkginclude_HEADERS = wholeslide.h wholeslide-features.h

noinst_HEADERS = wholeslide-private.h



noinst_PROGRAMS = test

test_CPPFLAGS = $(GLIB2_CFLAGS)

test_LDADD = libwholeslide.la $(GLIB2_LIBS)
