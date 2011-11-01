/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
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


// for exporting from shared libraries or DLLs
#if defined _WIN32
#  ifdef _OPENSLIDE_BUILDING_DLL
#    define OPENSLIDE_PUBLIC() __declspec(dllexport)
#  else
#    define OPENSLIDE_PUBLIC() __declspec(dllimport)
#  endif
#elif __GNUC__ > 3
# define OPENSLIDE_PUBLIC() __attribute__ ((visibility("default")))
#else
# define OPENSLIDE_PUBLIC()
#endif


#ifndef __cplusplus
#if defined(HAVE_STDBOOL_H)
/*
The C language implementation does correctly provide the standard header
file "stdbool.h".
 */
#include <stdbool.h>
#else
/*
The C language implementation does not provide the standard header file
"stdbool.h" as required by ISO/IEC 9899:1999.  Try to compensate for this
braindamage below.
*/
#if !defined(bool)
#define	bool	int
#endif
#if !defined(true)
#define true	1
#endif
#if !defined(false)
#define	false	0
#endif
#endif
#endif /* __cplusplus */


#endif
