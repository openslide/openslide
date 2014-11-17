/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#ifndef OPENSLIDE_OPENSLIDE_FEATURES_H_
#define OPENSLIDE_OPENSLIDE_FEATURES_H_


#ifndef __cplusplus
#  ifdef _MSC_VER
#    ifndef bool
#      define bool unsigned char
#    endif
#    ifndef true
#      define true 1
#    endif
#    ifndef false
#      define false 0
#    endif
#    ifndef __bool_true_false_are_defined
#      define __bool_true_false_are_defined 1
#    endif
#  else
#    include <stdbool.h>
#  endif
#endif


// for exporting from shared libraries or DLLs
#if defined _WIN32
#  ifdef _OPENSLIDE_BUILDING_DLL
#    define OPENSLIDE_PUBLIC() __declspec(dllexport)
#  else
#    define OPENSLIDE_PUBLIC() __declspec(dllimport)
#  endif
#elif defined OPENSLIDE_SIMPLIFY_HEADERS
// avoid constructs that could confuse a simplistic header parser
# define OPENSLIDE_PUBLIC()
#elif __GNUC__ > 3
# define OPENSLIDE_PUBLIC() __attribute__ ((visibility("default")))
#else
# define OPENSLIDE_PUBLIC()
#endif


// if possible, produce compiler warnings when deprecated functions
// are used
#if defined OPENSLIDE_SIMPLIFY_HEADERS
# define OPENSLIDE_DEPRECATED()
#elif __GNUC__ > 3 || (__GNUC__ == 3 && __GNUC_MINOR__ >= 1)
# define OPENSLIDE_DEPRECATED() __attribute__((deprecated))
#elif defined _MSC_VER
# define OPENSLIDE_DEPRECATED() __declspec(deprecated)
#else
# define OPENSLIDE_DEPRECATED()
#endif

#if defined OPENSLIDE_SIMPLIFY_HEADERS
# define OPENSLIDE_DEPRECATED_FOR(f)
#elif __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5)
# define OPENSLIDE_DEPRECATED_FOR(f) \
  __attribute__((deprecated("Use " #f " instead")))
#elif defined _MSC_VER
# define OPENSLIDE_DEPRECATED_FOR(f) \
  __declspec(deprecated("deprecated: Use " #f " instead"))
#else
# define OPENSLIDE_DEPRECATED_FOR(f) OPENSLIDE_DEPRECATED()
#endif


#endif
