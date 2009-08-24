EXTRA_DIST = README.txt TODO.txt gpl-2.0.txt LICENSE.txt

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = openslide.pc

ACLOCAL_AMFLAGS=-I m4


lib_LTLIBRARIES = src/libopenslide.la

src_libopenslide_la_LIBADD = $(GLIB2_LIBS) $(CAIRO_LIBS)

src_libopenslide_la_SOURCES = src/openslide.c \
	src/openslide-cache.c src/openslide-tilehelper.c \
	src/openslide-ops-jpeg.c \
	src/openslide-vendor-hamamatsu.c \
	src/openslide-ops-tiff.c \
	src/openslide-vendor-trestle.c src/openslide-vendor-aperio.c \
	src/openslide-vendor-mirax.c


src_libopenslide_la_CPPFLAGS = -pedantic -DBUILDING_DLL $(GLIB2_CFLAGS) $(CAIRO_CFLAGS) -DG_LOG_DOMAIN=\"Openslide\" -I$(top_srcdir)/src

src_libopenslide_la_LDFLAGS = -version-info 1:1:1 -no-undefined


pkginclude_HEADERS = src/openslide.h src/openslide-features.h

noinst_HEADERS = src/openslide-private.h src/openslide-cache.h src/openslide-tilehelper.h



noinst_PROGRAMS = src/test

src_test_CPPFLAGS = $(GLIB2_CFLAGS) -I$(top_srcdir)/src

src_test_LDADD = src/libopenslide.la $(GLIB2_LIBS)
