#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([OpenSlide], [3.3.3], [openslide-users@lists.andrew.cmu.edu], [openslide], [http://openslide.org])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects 1.11.1 dist-xz])
AM_SILENT_RULES([yes])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AM_PROG_CC_C_O
AC_PROG_CC_C99

# Largefile
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO

# Optional in C99
AC_TYPE_UINTPTR_T

LT_PREREQ([2.2.5])
LT_INIT([win32-dll disable-static])
LT_LANG([Windows Resource])
AM_CONDITIONAL([WINDOWS_RESOURCES], [test x$RC != x])

# for windows manifest
WINDOWS_VERSIONINFO=$(echo "${VERSION}.0.0.0" | cut -f1-4 -d. | tr . ,)
AC_SUBST([WINDOWS_VERSIONINFO])

# Suffix appended to version string
AC_ARG_WITH([version-suffix],
            AS_HELP_STRING([--with-version-suffix=STRING],
                           [suffix to append to the package version string]))
AC_MSG_CHECKING([version string])
if test "x$with_version_suffix" != "x"; then
  SUFFIXED_VERSION="$VERSION-$with_version_suffix"
else
  SUFFIXED_VERSION="$VERSION"
fi
AC_MSG_RESULT([$SUFFIXED_VERSION])
AC_DEFINE_UNQUOTED([SUFFIXED_VERSION], ["$SUFFIXED_VERSION"],
                   [Define to the package version string including any suffix.])
AC_SUBST([SUFFIXED_VERSION])


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

# Can we run programs we built?
AM_CONDITIONAL([CROSS_COMPILING], [test $host != $build])


# libraries
AC_SEARCH_LIBS([floor], [m],, AC_MSG_FAILURE([cannot find math library]))
AC_SEARCH_LIBS([jpeg_CreateDecompress], [jpeg],,
					 AC_MSG_FAILURE([cannot find libjpeg]))

PKG_CHECK_MODULES(ZLIB, [zlib], [], [
  dnl for Ubuntu Hardy and Lucid
  AC_SEARCH_LIBS([inflate], [z],, AC_MSG_FAILURE([cannot find zlib]))
])

PKG_CHECK_MODULES(OPENJPEG, [libopenjpeg1], [], [
  dnl OpenJPEG < 1.4 has no pkg-config file

  AC_MSG_CHECKING([for OpenJPEG (fallback)])
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
    OPENJPEG_LIBS="-lopenjpeg"
    AC_MSG_RESULT($openjpeg_ok)
  else
    AC_MSG_FAILURE([cannot find OpenJPEG])
  fi
])

PKG_CHECK_MODULES(LIBTIFF, [libtiff-4], [], [
  dnl libtiff < 4 has no pkg-config file
  old_LIBS="$LIBS"
  AC_SEARCH_LIBS([TIFFOpen], [tiff],, AC_MSG_FAILURE([cannot find libtiff]))
  LIBS="$old_LIBS"
  LIBTIFF_LIBS="-ltiff"
])

PKG_CHECK_MODULES(GLIB2, [glib-2.0 >= 2.16, gthread-2.0, gio-2.0, gobject-2.0])
PKG_CHECK_MODULES(CAIRO, [cairo >= 1.2])
PKG_CHECK_MODULES(LIBPNG, [libpng > 1.2])
PKG_CHECK_MODULES(GDKPIXBUF, [gdk-pixbuf-2.0 >= 2.14])
PKG_CHECK_MODULES(LIBXML2, [libxml-2.0])

# optional
PKG_CHECK_MODULES(VALGRIND, [valgrind], [
  AC_DEFINE([HAVE_VALGRIND], [1], [Define to 1 if you have the Valgrind headers.])
], [:])

gl_VISIBILITY

# CLOEXEC
AC_MSG_CHECKING([fopen() close-on-exec flag])
AS_CASE([$host],
  [*-*-mingw32], [
    # Assume that if we're building for Windows, we want to pass N to fopen().
    AC_MSG_RESULT([N])
    AC_DEFINE([FOPEN_CLOEXEC_FLAG], ["N"], [Set to the fopen() flag string that sets FD_CLOEXEC, or an empty string if not supported.])
  ],
  [
    # Default
    AC_RUN_IFELSE([
      AC_LANG_PROGRAM([
        #include <stdio.h>
        #include <unistd.h>
        #include <fcntl.h>
      ], [
        FILE *fp = fopen("/dev/null", "re");
        if (fp != NULL) {
          int fd = fileno(fp);
          if (fd != -1) {
            long ret = fcntl(fd, F_GETFD);
            if (ret != -1 && (ret & FD_CLOEXEC)) {
              return 0;
            }
          }
        }
        return 1;
      ])
    ], [
      # glibc >= 2.7
      AC_MSG_RESULT([e])
      AC_DEFINE([FOPEN_CLOEXEC_FLAG], ["e"])
    ], [
      # unknown
      AC_MSG_RESULT([unknown])
      AC_DEFINE([FOPEN_CLOEXEC_FLAG], [""])
    ], [
      # cross compile
      AC_MSG_RESULT([unknown (cross compile)])
      AC_DEFINE([FOPEN_CLOEXEC_FLAG], [""])
    ])
  ]
)
# Fallback: racily use fcntl()
AC_CHECK_FUNCS([fcntl])

# Only enable this on MinGW, since otherwise gcc will complain about an
# unknown option whenever it produces any *other* warnings
AS_CASE([$host],
  [*-*-mingw32],   [CFLAG_MS_FORMAT=-Wno-pedantic-ms-format],
  [CFLAG_MS_FORMAT=""]
)
AC_SUBST([CFLAG_MS_FORMAT])

# The min/max glib version is actually 2.16, but glib doesn't have special
# handling for API changes that old
AC_SUBST(AM_CFLAGS, ['-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wnested-externs $(CFLAG_MS_FORMAT) $(CFLAG_VISIBILITY) -DG_DISABLE_SINGLE_INCLUDES -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_26 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_MIN_REQUIRED -fno-common'])


AC_CONFIG_FILES([
Makefile
openslide.pc
src/openslide-dll.manifest
src/openslide-dll.rc
tools/openslide-quickhash1sum.1
tools/openslide-show-properties.1
tools/openslide-write-png.1
])
AC_OUTPUT
