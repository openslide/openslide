lib_LTLIBRARIES = libopenslide.la

libopenslide_la_LIBADD = $(GLIB2_LIBS) ../libopenjpeg/libopenjpeg.la

libopenslide_la_SOURCES = openslide.c \
	openslide-cache.c openslide-tilehelper.c \
	openslide-vendor-trestle.c openslide-vendor-hamamatsu.c \
	openslide-vendor-aperio.c openslide-vendor-generic-jp2k.c \
	openslide-ops-tiff.c openslide-ops-jpeg.c openslide-ops-jp2k.c

libopenslide_la_CPPFLAGS = -pedantic -DBUILDING_DLL -I$(srcdir)/../libopenjpeg $(GLIB2_CFLAGS) -DG_LOG_DOMAIN=\"Openslide\"

libopenslide_la_LDFLAGS = -version-info 0:0:0


pkginclude_HEADERS = openslide.h openslide-features.h

noinst_HEADERS = openslide-private.h openslide-cache.h openslide-tilehelper.h



noinst_PROGRAMS = test

test_CPPFLAGS = $(GLIB2_CFLAGS)

test_LDADD = libopenslide.la $(GLIB2_LIBS)
