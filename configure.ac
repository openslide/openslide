#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([wholeslide], [0.1], [agoode@andrew.cmu.edu])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE([foreign 1.9])

# Checks for programs.
AC_PROG_CC

AC_DISABLE_STATIC
AC_PROG_LIBTOOL


# libraries
AC_SEARCH_LIBS([jpeg_CreateDecompress], [jpeg],,
					 AC_MSG_FAILURE([cannot find libjpeg]))
AC_SEARCH_LIBS([jas_init], [jasper],, AC_MSG_FAILURE([cannot find JasPer]))
AC_SEARCH_LIBS([TIFFOpen], [tiff],, AC_MSG_FAILURE([cannot find libtiff]))

#PKG_CHECK_MODULES(TIFF, [glib-2.0, gthread-2.0])
#AC_SUBST(GLIB2_CFLAGS)
#AC_SUBST(GLIB2_LIBS)


AC_CHECK_HEADERS([stdint.h])



AC_CONFIG_FILES([
Makefile
doc/Makefile
src/Makefile
wholeslide.pc
])
AC_OUTPUT

