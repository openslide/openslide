#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([OpenSlide], [3.0.3], [openslide-users@lists.andrew.cmu.edu], [openslide], [http://openslide.cs.cmu.edu])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects 1.11.1 dist-xz])
AM_SILENT_RULES([yes])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AM_PROG_CC_C_O
AC_PROG_CC_C99

AC_HEADER_STDBOOL

# Largefile
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO

LT_PREREQ([2.2.5])
LT_INIT([win32-dll disable-static])

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
#include <openjpeg.h>
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
  LIBS="$OPENJPEG_LIBS $LIBS"
  AC_MSG_RESULT($openjpeg_ok)
else
  AC_MSG_FAILURE([cannot find OpenJPEG])
fi

AC_SEARCH_LIBS([TIFFOpen], [tiff],, AC_MSG_FAILURE([cannot find libtiff]))

# for win32 -- why the trickery? to avoid libtool linking the exe files to msvcr90
old_LIBS="$LIBS"
AC_SEARCH_LIBS([_fseeki64], [msvcr80], AC_SUBST(WIN32_DLL_TRICKERY, [-lmsvcr80]),)
LIBS="$old_LIBS"

PKG_CHECK_MODULES(GLIB2, [glib-2.0 >= 2.12, gthread-2.0])
AC_SUBST(GLIB2_CFLAGS)
AC_SUBST(GLIB2_LIBS)

old_LIBS="$LIBS"
LIBS="$GLIB2_LIBS $LIBS"
AC_CHECK_FUNCS([g_checksum_new], build_hashprogs="yes", build_hashprogs="no")
LIBS="$old_LIBS"
AM_CONDITIONAL([BUILD_HASHPROGS], [test x$build_hashprogs = xyes])

PKG_CHECK_MODULES(CAIRO, [cairo >= 1.2])
AC_SUBST(CAIRO_CFLAGS)
AC_SUBST(CAIRO_LIBS)


AC_SUBST(AM_CFLAGS, ['-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wnested-externs -fvisibility=hidden -DG_DISABLE_DEPRECATED -DG_DISABLE_SINGLE_INCLUDES -fno-common'])


AC_CONFIG_FILES([
Makefile
openslide.pc
openslide-dll.manifest
])
AC_OUTPUT
