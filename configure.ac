#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([openslide], [1.1.1], [agoode@andrew.cmu.edu])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE([foreign 1.9])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AC_PROG_CC_C99

AC_HEADER_STDBOOL

# Largefile
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO

AC_DISABLE_STATIC
AC_LIBTOOL_WIN32_DLL
AC_PROG_LIBTOOL

AC_C_BIGENDIAN

# Check for recent pkg-config which supports Requires.private
# (snippet taken from cairo configure.in)
PKG_PROG_PKG_CONFIG()
if test "x$PKG_CONFIG" = x; then
        AC_MSG_ERROR([pkg-config >= $PKGCONFIG_REQUIRED required but not found (http://pkgconfig.freedesktop.org/)])
fi

case `$PKG_CONFIG --version` in
[0.?|0.?.?|0.1[0-7]|0.1[0-7].?]) PKGCONFIG_REQUIRES="Requires"; ;;
*) PKGCONFIG_REQUIRES="Requires.private"; ;;
esac

AC_SUBST(PKGCONFIG_REQUIRES)


# libraries
AC_SEARCH_LIBS([floor], [m],, AC_MSG_FAILURE([cannot find math library]))
AC_SEARCH_LIBS([jpeg_CreateDecompress], [jpeg],,
					 AC_MSG_FAILURE([cannot find libjpeg]))

AC_MSG_CHECKING([for OpenJPEG])
dnl AC_CHECK_LIB won't work with the Win32 version of openjpeg
dnl because of the stdcall calling convention which requires
dnl configure to read openjpeg.h.

old_LIBS="$LIBS"
LIBS="-lopenjpeg $LIBS"
AC_LINK_IFELSE(
  [AC_LANG_SOURCE(
[[
#include <openjpeg/openjpeg.h>
int
main ()
{
  const char *ver = opj_version();
  return 0;
}
]])],
  openjpeg_ok=yes,
  openjpeg_ok=no)
LIBS="$old_LIBS"

if test "$openjpeg_ok" = yes; then
  AC_DEFINE(HAVE_OPENJPEG, 1, [Is OpenJPEG available and enabled])
  OPENJPEG_LIBS="-lopenjpeg"
  AC_MSG_RESULT($openjpeg_ok)
else
  AC_MSG_FAILURE([cannot find OpenJPEG])
fi

AC_SEARCH_LIBS([TIFFOpen], [tiff],, AC_MSG_FAILURE([cannot find libtiff]))

PKG_CHECK_MODULES(GLIB2, [glib-2.0 >= 2.12, gthread-2.0])
AC_SUBST(GLIB2_CFLAGS)
AC_SUBST(GLIB2_LIBS)


AC_SUBST(AM_CFLAGS, ['-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wnested-externs -fvisibility=hidden -DG_DISABLE_DEPRECATED -DG_DISABLE_SINGLE_INCLUDES'])


AC_CONFIG_FILES([
Makefile
src/Makefile
openslide.pc
])
AC_OUTPUT
