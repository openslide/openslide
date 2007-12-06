#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT([libwholeslide], [0.1], [agoode@andrew.cmu.edu])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE([foreign 1.9])

# Checks for programs.
AC_PROG_CC

AC_DISABLE_STATIC
AC_PROG_LIBTOOL


# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([stdint.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_UINT32_T

# Checks for library functions.



AC_CONFIG_FILES([
Makefile
doc/Makefile
src/Makefile
])
AC_OUTPUT
