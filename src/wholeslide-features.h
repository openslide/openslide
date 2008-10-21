/*
 *  Wholeslide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  Wholeslide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  Wholeslide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Wholeslide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking Wholeslide statically or dynamically with other modules is
 *  making a combined work based on Wholeslide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#ifndef WHOLESLIDE_FEATURES_H
#define WHOLESLIDE_FEATURES_H


// for exporting from shared libraries or DLLs
#if defined WIN32
#  ifdef BUILDING_DLL
#    define wholeslide_public __declspec(dllexport)
#  else
#    define wholeslide_public __declspec(dllimport)
#  endif
#elif __GNUC__ > 3
# define wholeslide_public __attribute((visibility("default")))
#else
# define wholeslide_public
#endif



#endif
