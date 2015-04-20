EXTRA_DIST = README.txt lgpl-2.1.txt LICENSE.txt CHANGELOG.txt doc/Doxyfile CONTRIBUTING.txt

# doxygen
dist-hook:
	cd $(distdir)/doc; doxygen

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = openslide.pc

ACLOCAL_AMFLAGS=-I m4


lib_LTLIBRARIES = src/libopenslide.la

src_libopenslide_la_LIBADD = $(GLIB2_LIBS) $(CAIRO_LIBS) $(SQLITE3_LIBS) \
	$(LIBXML2_LIBS) $(OPENJPEG_LIBS) $(LIBTIFF_LIBS) $(LIBPNG_LIBS) \
	$(GDKPIXBUF_LIBS) $(ZLIB_LIBS)

src_libopenslide_la_SOURCES = \
	src/openslide.c \
	src/openslide-cache.c \
	src/openslide-decode-gdkpixbuf.c \
	src/openslide-decode-jp2k.c \
	src/openslide-decode-jpeg.c \
	src/openslide-decode-png.c \
	src/openslide-decode-sqlite.c \
	src/openslide-decode-tiff.c \
	src/openslide-decode-tifflike.c \
	src/openslide-decode-xml.c \
	src/openslide-error.c \
	src/openslide-grid.c \
	src/openslide-hash.c \
	src/openslide-jdatasrc.c \
	src/openslide-tables.c \
	src/openslide-util.c \
	src/openslide-vendor-aperio.c \
	src/openslide-vendor-generic-tiff.c \
	src/openslide-vendor-hamamatsu.c \
	src/openslide-vendor-leica.c \
	src/openslide-vendor-mirax.c \
	src/openslide-vendor-philips.c \
	src/openslide-vendor-sakura.c \
	src/openslide-vendor-trestle.c \
	src/openslide-vendor-ventana.c

EXTRA_PROGRAMS = src/make-tables
CLEANFILES = src/make-tables
MAINTAINERCLEANFILES = src/openslide-tables.c
# Depending directly on src/make-tables causes needless regeneration of
# openslide-tables.c.  As the lesser of evils, recursively invoke make.
src/openslide-tables.c: src/make-tables.c
	@$(MAKE) $(AM_MAKEFLAGS) src/make-tables
	$(AM_V_GEN)src/make-tables$(EXEEXT) "$@"

if WINDOWS_RESOURCES
src_libopenslide_la_SOURCES += src/openslide-dll.rc
src/openslide-dll.lo: src/openslide-dll.manifest
endif

.rc.lo:
	$(AM_V_GEN)$(LIBTOOL) $(AM_V_lt) $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS) --tag=RC --mode=compile $(RC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) -i "$<" -o "$@"

src_libopenslide_la_CPPFLAGS = -pedantic -D_OPENSLIDE_BUILDING_DLL \
	$(GLIB2_CFLAGS) $(CAIRO_CFLAGS) $(SQLITE3_CFLAGS) $(LIBXML2_CFLAGS) \
	$(OPENJPEG_CFLAGS) $(LIBTIFF_CFLAGS) $(LIBPNG_CFLAGS) \
	$(GDKPIXBUF_CFLAGS) $(ZLIB_CFLAGS) -DG_LOG_DOMAIN=\"Openslide\" \
	-I$(top_srcdir)/src

src_libopenslide_la_LDFLAGS = -version-info 4:1:4 -no-undefined


pkginclude_HEADERS = \
	src/openslide.h \
	src/openslide-features.h

noinst_HEADERS = \
	src/openslide-cairo.h \
	src/openslide-decode-gdkpixbuf.h \
	src/openslide-decode-jp2k.h \
	src/openslide-decode-jpeg.h \
	src/openslide-decode-png.h \
	src/openslide-decode-sqlite.h \
	src/openslide-decode-tiff.h \
	src/openslide-decode-tifflike.h \
	src/openslide-decode-xml.h \
	src/openslide-error.h \
	src/openslide-hash.h \
	src/openslide-private.h \
	test/test-common.h \
	tools/openslide-tools-common.h



noinst_PROGRAMS = test/test test/try_open test/parallel test/query \
	test/extended test/mosaic test/profile
noinst_SCRIPTS = test/driver
CLEANFILES += test/driver
EXTRA_DIST += test/driver.in

test_test_CPPFLAGS = $(GLIB2_CFLAGS) $(CAIRO_CFLAGS) $(VALGRIND_CFLAGS) -I$(top_srcdir)/src
# VALGRIND_LIBS not needed
test_test_LDADD = src/libopenslide.la $(GLIB2_LIBS) $(CAIRO_LIBS)

test_try_open_SOURCES = test/test-common.c test/try_open.c
test_try_open_CPPFLAGS = $(GLIB2_CFLAGS) -I$(top_srcdir)/src
test_try_open_LDADD = src/libopenslide.la $(GLIB2_LIBS)

test_parallel_CPPFLAGS = $(GLIB2_CFLAGS) -I$(top_srcdir)/src
test_parallel_LDADD = src/libopenslide.la $(GLIB2_LIBS)

test_query_CPPFLAGS = $(GLIB2_CFLAGS) -I$(top_srcdir)/src
test_query_LDADD = src/libopenslide.la $(GLIB2_LIBS)

test_extended_SOURCES = test/test-common.c test/extended.c
test_extended_CPPFLAGS = $(GLIB2_CFLAGS) -I$(top_srcdir)/src
test_extended_LDADD = src/libopenslide.la $(GLIB2_LIBS)

test_mosaic_CPPFLAGS = $(GLIB2_CFLAGS) $(CAIRO_CFLAGS) -I$(top_srcdir)/src
test_mosaic_LDADD = src/libopenslide.la $(GLIB2_LIBS) $(CAIRO_LIBS)

test_profile_CPPFLAGS = $(GLIB2_CFLAGS) $(VALGRIND_CFLAGS) -I$(top_srcdir)/src
test_profile_LDADD = src/libopenslide.la $(GLIB2_LIBS)

if CYGWIN_CROSS_TEST
noinst_PROGRAMS += test/symlink
test_symlink_LDADD = -lkernel32
endif

test/driver: test/driver.in Makefile
	$(AM_V_GEN)sed -e 's:!!SRCDIR!!:$(abs_srcdir)/test:g' \
		-e 's:!!BUILDDIR!!:$(abs_builddir)/test:g' \
		-e 's:!!VERSION!!:$(VERSION):g' \
		-e 's:!!CYGWIN_CROSS_TEST!!:$(CYGWIN_CROSS_TEST):g' \
		-e 's:!!FEATURES!!:$(FEATURE_FLAGS):g' "$<" > "$@" && \
		chmod +x "$@"


# tools

# show-properties
bin_PROGRAMS = tools/openslide-show-properties
man_MANS = tools/openslide-show-properties.1
tools_openslide_show_properties_SOURCES = tools/openslide-tools-common.c tools/openslide-show-properties.c
tools_openslide_show_properties_CPPFLAGS = -I$(top_srcdir)/src $(GLIB2_CFLAGS)
tools_openslide_show_properties_LDADD = src/libopenslide.la $(GLIB2_LIBS)

# quickhash1sum
bin_PROGRAMS += tools/openslide-quickhash1sum
man_MANS += tools/openslide-quickhash1sum.1
tools_openslide_quickhash1sum_SOURCES = tools/openslide-tools-common.c tools/openslide-quickhash1sum.c
tools_openslide_quickhash1sum_CPPFLAGS = -I$(top_srcdir)/src $(GLIB2_CFLAGS)
tools_openslide_quickhash1sum_LDADD = src/libopenslide.la $(GLIB2_LIBS)

# write-png
bin_PROGRAMS += tools/openslide-write-png
man_MANS += tools/openslide-write-png.1
tools_openslide_write_png_SOURCES = tools/openslide-tools-common.c tools/openslide-write-png.c
tools_openslide_write_png_CPPFLAGS = -I$(top_srcdir)/src $(LIBPNG_CFLAGS) $(GLIB2_CFLAGS)
tools_openslide_write_png_LDADD = src/libopenslide.la $(LIBPNG_LIBS) $(GLIB2_LIBS)

# man pages
EXTRA_DIST += $(man_MANS:=.in)
