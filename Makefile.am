EXTRA_DIST = README.txt TODO.txt lgpl-2.1.txt LICENSE.txt CHANGELOG.txt

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = openslide.pc

ACLOCAL_AMFLAGS=-I m4


lib_LTLIBRARIES = src/libopenslide.la

src_libopenslide_la_LIBADD = $(GLIB2_LIBS) $(CAIRO_LIBS)

src_libopenslide_la_SOURCES = src/openslide.c \
	src/openslide-cache.c src/openslide-tilehelper.c \
	src/openslide-hash.c src/openslide-error.c \
	src/openslide-ops-jpeg.c \
	src/openslide-vendor-hamamatsu.c \
	src/openslide-ops-tiff.c \
	src/openslide-vendor-trestle.c src/openslide-vendor-aperio.c \
	src/openslide-vendor-mirax.c src/openslide-vendor-generic-tiff.c

if WINDOWS_RESOURCES
src_libopenslide_la_SOURCES += src/openslide-dll.rc
src/openslide-dll.lo: src/openslide-dll.manifest
endif

.rc.lo:
	$(LIBTOOL) $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS) --tag=RC --mode=compile $(RC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) -i "$<" -o "$@"


src_libopenslide_la_CPPFLAGS = -pedantic -D_OPENSLIDE_BUILDING_DLL $(GLIB2_CFLAGS) $(CAIRO_CFLAGS) -DG_LOG_DOMAIN=\"Openslide\" -I$(top_srcdir)/src

src_libopenslide_la_LDFLAGS = -version-info 1:11:1 -no-undefined -Wl,$(WIN32_DLL_TRICKERY)


pkginclude_HEADERS = src/openslide.h src/openslide-features.h

noinst_HEADERS = src/openslide-private.h src/openslide-cache.h src/openslide-tilehelper.h src/openslide-hash.h src/valgrind.h src/callgrind.h



noinst_PROGRAMS = src/test

src_test_CPPFLAGS = $(GLIB2_CFLAGS) -I$(top_srcdir)/src
src_test_LDADD = src/libopenslide.la $(GLIB2_LIBS)


bin_PROGRAMS = tools/openslide-show-properties
if BUILD_HASHPROGS
  bin_PROGRAMS += tools/openslide-quickhash1sum
endif

tools_openslide_quickhash1sum_CPPFLAGS = -I$(top_srcdir)/src
tools_openslide_quickhash1sum_LDADD = src/libopenslide.la

tools_openslide_show_properties_CPPFLAGS = -I$(top_srcdir)/src
tools_openslide_show_properties_LDADD = src/libopenslide.la
