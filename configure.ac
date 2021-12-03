#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.69)
AC_INIT([OpenSlide], [3.4.1], [openslide-users@lists.andrew.cmu.edu], [openslide], [https://openslide.org])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects 1.11.1 dist-xz])
AM_SILENT_RULES([yes])
AC_CONFIG_MACRO_DIR([m4])

# Check for cross build
AC_CANONICAL_BUILD
AC_CANONICAL_HOST

# Checks for programs.
AM_PROG_CC_C_O
AC_PROG_CC

# Largefile
AC_SYS_LARGEFILE
AC_FUNC_FSEEKO

LT_PREREQ([2.2.5])
LT_INIT([win32-dll disable-static])
LT_LANG([Windows Resource])

# Windows resources and manifest
AM_CONDITIONAL([WINDOWS_RESOURCES], [test $host_os = mingw32])
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


# libraries
AC_SEARCH_LIBS([floor], [m],, AC_MSG_FAILURE([cannot find math library]))

PKG_CHECK_MODULES(ZLIB, [zlib], [], [
  dnl for BSD
  AC_SEARCH_LIBS([inflate], [z],, AC_MSG_FAILURE([cannot find zlib]))
])

PKG_CHECK_MODULES(LIBJPEG, [libjpeg], [], [
  dnl IJG libjpeg, or libjpeg-turbo < 1.5.0
  AC_SEARCH_LIBS([jpeg_CreateDecompress], [jpeg],,
                 AC_MSG_FAILURE([cannot find libjpeg]))
])

PKG_CHECK_MODULES(OPENJPEG, [libopenjp2 >= 2.1.0])
PKG_CHECK_MODULES(LIBTIFF, [libtiff-4])
PKG_CHECK_MODULES(GLIB2, [glib-2.0 >= 2.56, gthread-2.0, gio-2.0, gobject-2.0])
PKG_CHECK_MODULES(CAIRO, [cairo >= 1.2])
PKG_CHECK_MODULES(LIBPNG, [libpng > 1.2])
PKG_CHECK_MODULES(GDKPIXBUF, [gdk-pixbuf-2.0 >= 2.14])
PKG_CHECK_MODULES(LIBXML2, [libxml-2.0])
PKG_CHECK_MODULES(SQLITE3, [sqlite3 >= 3.6.20])
PKG_CHECK_MODULES(LIBZIP, [libzip >= 1.1])

# optional
PKG_CHECK_MODULES(VALGRIND, [valgrind], [
  AC_DEFINE([HAVE_VALGRIND], [1], [Define to 1 if you have the Valgrind headers.])
], [:])

gl_VISIBILITY

# CLOEXEC
AC_MSG_CHECKING([fopen() close-on-exec flag])
AS_CASE([$host_os],
  [mingw32], [
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
      # glibc >= 2.7, FreeBSD >= 10.0, NetBSD >= 6.0
      AC_MSG_RESULT([e])
      AC_DEFINE([FOPEN_CLOEXEC_FLAG], ["e"])
    ], [
      # unknown
      AC_MSG_RESULT([unknown])
      AC_DEFINE([FOPEN_CLOEXEC_FLAG], [""])
      AC_DEFINE([NONATOMIC_CLOEXEC], [1], [Define to 1 if fopen() cannot atomically set FD_CLOEXEC.])
    ], [
      # cross compile
      AC_MSG_RESULT([unknown (cross compile)])
      AC_DEFINE([FOPEN_CLOEXEC_FLAG], [""])
      AC_DEFINE([NONATOMIC_CLOEXEC], [1])
    ])
  ]
)
# Fallback: racily use fcntl()
AC_CHECK_FUNCS([fcntl])

# Windows _wfopen()
AC_CHECK_FUNCS([_wfopen])

# Mac OS X proc_pidfdinfo()
AC_MSG_CHECKING([for proc_pidfdinfo])
AC_LINK_IFELSE([
  AC_LANG_PROGRAM(
    [#include <libproc.h>],
    [proc_pidfdinfo(0, 0, PROC_PIDFDKQUEUEINFO, NULL, 0)]
  )
], [
  AC_MSG_RESULT([yes])
  AC_DEFINE([HAVE_PROC_PIDFDINFO], [1], [Define to 1 if you have the proc_pidfdinfo function.])
], [
  AC_MSG_RESULT([no])
])

# The test driver has special support for testing Windows builds from Cygwin
AC_MSG_CHECKING([whether to cross-test from Cygwin])
if test "$host_os" = "mingw32" -a "$build_os" = "cygwin"; then
  AC_MSG_RESULT([yes])
  CYGWIN_CROSS_TEST=yes
else
  AC_MSG_RESULT([no])
  CYGWIN_CROSS_TEST=""
fi
AC_SUBST([CYGWIN_CROSS_TEST])
AM_CONDITIONAL([CYGWIN_CROSS_TEST], [test -n "$CYGWIN_CROSS_TEST"])

# gcc assumes Win32 stack frames are 16-byte-aligned by default, but the
# Win32 ABI only requires 4-byte alignment.  If gcc emits aligned SSE
# instructions that access the stack, this can cause GPFs if we're called by
# a program compiled by a different compiler such as MSVC.  The user is
# responsible for choosing compiler options appropriate for their desired
# ABI, but this is a sufficiently insidious error that it's worth checking
# in the test suite by default.  We do that by compiling the test programs
# with 4-byte stack alignment on Win32.  We allow the user to disable that
# behavior, however, in case they don't care about ABI compatibility with
# other compilers.
AC_ARG_ENABLE([checking-windows-abi-compat],
              AS_HELP_STRING([--disable-checking-windows-abi-compat],
                             [don't override default calling convention when testing on Windows]))
AS_CASE([$host],
  [i*86-*-mingw32], [
    AC_MSG_CHECKING([whether to test with Windows calling convention])
    AS_IF([test "$enable_checking_windows_abi_compat" != "no"], [
      AC_MSG_RESULT([yes])
      gl_COMPILER_OPTION_IF([-mpreferred-stack-boundary=2], [
        AC_SUBST([TEST_CFLAGS], [-mpreferred-stack-boundary=2])
      ])
    ], [
      AC_MSG_RESULT([no])
    ])
  ]
)

# Compiler warnings
WARN_CFLAGS="-Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wnested-externs"
AC_SUBST([WARN_CFLAGS])
# MinGW only
gl_WARN_ADD([-Wno-pedantic-ms-format])

AC_SUBST(AM_CFLAGS, ['$(WARN_CFLAGS) $(CFLAG_VISIBILITY) -DG_DISABLE_SINGLE_INCLUDES -DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_56 -DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_MIN_REQUIRED -fno-common'])

AC_SUBST(FEATURE_FLAGS)

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
