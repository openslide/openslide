lib_LTLIBRARIES = libwholeslide.la

libwholeslide_la_LIBADD = $(GLIB2_LIBS) ../libopenjpeg/libopenjpeg.la

libwholeslide_la_SOURCES = wholeslide.c \
	wholeslide-vendor-trestle.c wholeslide-vendor-hamamatsu.c \
	wholeslide-vendor-aperio.c \ wholeslide-vendor-generic-jp2k.c \
	wholeslide-ops-tiff.c wholeslide-ops-jpeg.c wholeslide-ops-jp2k.c

libwholeslide_la_CPPFLAGS = -pedantic-errors -DBUILDING_DLL -I../libopenjpeg $(GLIB2_CFLAGS)

libwholeslide_la_LDFLAGS = -version-info 0:0:0


pkginclude_HEADERS = wholeslide.h wholeslide-features.h

noinst_HEADERS = wholeslide-private.h



noinst_PROGRAMS = test

test_CPPFLAGS = $(GLIB2_CFLAGS)

test_LDADD = libwholeslide.la $(GLIB2_LIBS)
