lib_LTLIBRARIES = libwholeslide.la

libwholeslide_la_LIBADD = $(GLIB2_LIBS)

libwholeslide_la_SOURCES = wholeslide.c

AM_CPPFLAGS = $(GLIB2_CFLAGS)

pkginclude_HEADERS = wholeslide.h

noinst_HEADERS = wholeslide-private.h

noinst_PROGRAMS = test

test_LDADD = libwholeslide.la $(GLIB2_LIBS)
