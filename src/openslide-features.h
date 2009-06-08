/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#ifndef OPENSLIDE_OPENSLIDE_FEATURES_H_
#define OPENSLIDE_OPENSLIDE_FEATURES_H_


// for exporting from shared libraries or DLLs
#if defined _WIN32
#  ifdef BUILDING_DLL
#    define openslide_public __declspec(dllexport)
#  else
#    define openslide_public __declspec(dllimport)
#  endif
#elif __GNUC__ > 3
# define openslide_public __attribute((visibility("default")))
#else
# define openslide_public
#endif



#endif
