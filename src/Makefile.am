lib_LTLIBRARIES = libopenslide.la

libopenslide_la_LIBADD = $(GLIB2_LIBS) $(CAIRO_LIBS)

libopenslide_la_SOURCES = openslide.c \
	openslide-cache.c openslide-tilehelper.c \
	openslide-ops-tiff.c \
	openslide-vendor-trestle.c openslide-vendor-aperio.c \
	openslide-ops-jpeg.c \
	openslide-vendor-hamamatsu.c openslide-vendor-mirax.c


libopenslide_la_CPPFLAGS = -pedantic -DBUILDING_DLL $(GLIB2_CFLAGS) $(CAIRO_CFLAGS) -DG_LOG_DOMAIN=\"Openslide\"

libopenslide_la_LDFLAGS = -version-info 0:2:0 -no-undefined


pkginclude_HEADERS = openslide.h openslide-features.h

noinst_HEADERS = openslide-private.h openslide-cache.h openslide-tilehelper.h



noinst_PROGRAMS = test

test_CPPFLAGS = $(GLIB2_CFLAGS)

test_LDADD = libopenslide.la $(GLIB2_LIBS)
