#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([OpenSlide], [3.2.5], [openslide-users@lists.andrew.cmu.edu], [openslide], [http://openslide.org])
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
LT_LANG([Windows Resource])
AM_CONDITIONAL([WINDOWS_RESOURCES], [test x$RC != x])

# for windows manifest
AS_CASE([$host],
  [i?86-*-*],   [WINDOWS_PROCESSOR_ARCHITECTURE=x86],
  [x86_64-*-*], [WINDOWS_PROCESSOR_ARCHITECTURE=amd64],
  [WINDOWS_PROCESSOR_ARCHITECTURE=""]
)
AC_SUBST([WINDOWS_PROCESSOR_ARCHITECTURE])


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

dnl https://bugzilla.redhat.com/show_bug.cgi?id=497492
AC_CHECK_TYPES([jpeg_boolean], [], [], [[#include <stdio.h>
#include <jpeglib.h>]])

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
  OPENJPEG_LIBS="-lopenjpeg"
  LIBS="$OPENJPEG_LIBS $LIBS"
  AC_MSG_RESULT($openjpeg_ok)
else
  AC_MSG_FAILURE([cannot find OpenJPEG])
fi

AC_SEARCH_LIBS([TIFFOpen], [tiff],, AC_MSG_FAILURE([cannot find libtiff]))

# for win32 -- why the trickery? to avoid libtool linking the exe files to msvcr80
old_LIBS="$LIBS"
AC_SEARCH_LIBS([_fseeki64], [msvcr80], [
  AC_SUBST(WIN32_DLL_TRICKERY, [-lmsvcr80])
  msvcrt=yes
], [msvcrt=no])
LIBS="$old_LIBS"

PKG_CHECK_MODULES(GLIB2, [glib-2.0 >= 2.12, gthread-2.0])

old_LIBS="$LIBS"
LIBS="$GLIB2_LIBS $LIBS"
AC_CHECK_FUNCS([g_checksum_new], need_sha256="no", need_sha256="yes")
LIBS="$old_LIBS"
AM_CONDITIONAL([NEED_SHA256], [test x$need_sha256 = xyes])

PKG_CHECK_MODULES(CAIRO, [cairo >= 1.2])
PKG_CHECK_MODULES(LIBPNG, [libpng > 1.2])

gl_VISIBILITY

# CLOEXEC
AC_MSG_CHECKING([fopen() close-on-exec flag])
if test z$msvcrt = zyes ; then
  # Assume that if we're using msvcrt, we want to pass N to fopen().
  # We don't test this directly because we are likely cross-compiling.
  AC_MSG_RESULT([N])
  AC_DEFINE([FOPEN_CLOEXEC_FLAG], ["N"], [Set to the fopen() flag string that sets FD_CLOEXEC, or an empty string if not supported.])
else
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
fi
# Fallback: racily use fcntl()
AC_CHECK_FUNCS([fcntl])

# Only enable this on MinGW, since otherwise gcc will complain about an
# unknown option whenever it produces any *other* warnings
AS_CASE([$host],
  [*-*-mingw32],   [CFLAG_MS_FORMAT=-Wno-pedantic-ms-format],
  [CFLAG_MS_FORMAT=""]
)
AC_SUBST([CFLAG_MS_FORMAT])

AC_SUBST(AM_CFLAGS, ['-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wnested-externs -Wc++-compat $(CFLAG_MS_FORMAT) $(CFLAG_VISIBILITY) -DG_DISABLE_DEPRECATED -DG_DISABLE_SINGLE_INCLUDES -fno-common'])


AC_CONFIG_FILES([
Makefile
openslide.pc
src/openslide-dll.manifest
])
AC_OUTPUT
