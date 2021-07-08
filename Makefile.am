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
	$(LIBJPEG_LIBS) $(GDKPIXBUF_LIBS) $(ZLIB_LIBS)

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
	src/openslide-decode-dicom.c \
	src/openslide-decode-xml.c \
	src/openslide-error.c \
	src/openslide-grid.c \
	src/openslide-hash.c \
	src/openslide-jdatasrc.c \
	src/openslide-tables.c \
	src/openslide-util.c \
	src/openslide-vendor-aperio.c \
	src/openslide-vendor-generic-tiff.c \
	src/openslide-vendor-dicom.c \
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
	common/openslide-common.h \
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
	src/openslide-private.h


# common program code

noinst_LIBRARIES = common/libopenslide-common.a

common_libopenslide_common_a_CPPFLAGS = $(COMMON_CPPFLAGS)
common_libopenslide_common_a_SOURCES = \
	common/openslide-common-cmdline.c \
	common/openslide-common-fail.c \
	common/openslide-common-fd.c

COMMON_CPPFLAGS = $(GLIB2_CFLAGS) -I$(top_srcdir)/src -I$(top_srcdir)/common
COMMON_LDADD = common/libopenslide-common.a src/libopenslide.la $(GLIB2_LIBS)


# test

noinst_PROGRAMS = test/test test/try_open test/parallel test/query \
	test/extended test/mosaic test/profile
noinst_SCRIPTS = test/driver
CLEANFILES += test/driver
EXTRA_DIST += test/driver.in

test_test_CPPFLAGS = $(COMMON_CPPFLAGS) $(CAIRO_CFLAGS) $(VALGRIND_CFLAGS)
test_test_CFLAGS = $(AM_CFLAGS) $(TEST_CFLAGS)
# VALGRIND_LIBS not needed
test_test_LDADD = $(COMMON_LDADD) $(CAIRO_LIBS)

test_try_open_CPPFLAGS = $(COMMON_CPPFLAGS)
test_try_open_CFLAGS = $(AM_CFLAGS) $(TEST_CFLAGS)
test_try_open_LDADD = $(COMMON_LDADD)

test_parallel_CPPFLAGS = $(COMMON_CPPFLAGS)
test_parallel_CFLAGS = $(AM_CFLAGS) $(TEST_CFLAGS)
test_parallel_LDADD = $(COMMON_LDADD)

test_query_CPPFLAGS = $(COMMON_CPPFLAGS)
test_query_CFLAGS = $(AM_CFLAGS) $(TEST_CFLAGS)
test_query_LDADD = $(COMMON_LDADD)

test_extended_CPPFLAGS = $(COMMON_CPPFLAGS)
test_extended_CFLAGS = $(AM_CFLAGS) $(TEST_CFLAGS)
test_extended_LDADD = $(COMMON_LDADD)

test_mosaic_CPPFLAGS = $(COMMON_CPPFLAGS) $(CAIRO_CFLAGS)
test_mosaic_CFLAGS = $(AM_CFLAGS) $(TEST_CFLAGS)
test_mosaic_LDADD = $(COMMON_LDADD) $(CAIRO_LIBS)

test_profile_CPPFLAGS = $(COMMON_CPPFLAGS) $(VALGRIND_CFLAGS)
test_profile_CFLAGS = $(AM_CFLAGS) $(TEST_CFLAGS)
test_profile_LDADD = $(COMMON_LDADD)

if CYGWIN_CROSS_TEST
noinst_PROGRAMS += test/symlink
test_symlink_CFLAGS = $(AM_CFLAGS) -municode
test_symlink_LDFLAGS = $(AM_LDFLAGS) -municode
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
tools_openslide_show_properties_CPPFLAGS = $(COMMON_CPPFLAGS)
tools_openslide_show_properties_LDADD = $(COMMON_LDADD)

# quickhash1sum
bin_PROGRAMS += tools/openslide-quickhash1sum
man_MANS += tools/openslide-quickhash1sum.1
tools_openslide_quickhash1sum_CPPFLAGS = $(COMMON_CPPFLAGS)
tools_openslide_quickhash1sum_LDADD = $(COMMON_LDADD)

# write-png
bin_PROGRAMS += tools/openslide-write-png
man_MANS += tools/openslide-write-png.1
tools_openslide_write_png_CPPFLAGS = $(COMMON_CPPFLAGS) $(LIBPNG_CFLAGS)
tools_openslide_write_png_LDADD = $(COMMON_LDADD) $(LIBPNG_LIBS)

# man pages
EXTRA_DIST += $(man_MANS:=.in)
