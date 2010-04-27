project(OpenSlide)

cmake_minimum_required(VERSION 2.6.2)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  message(FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there. (you may need to remove CMakeCache.txt ")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

SET(TIFF_NAMES itktiff)
set(JPEG_NAMES itkjpeg itkjpeg8 itkjpeg12 itkjpeg16)

find_package(jpeg REQUIRED)
find_package(openjpeg REQUIRED)
find_package(tiff REQUIRED)
find_package(GTK2 2.6 REQUIRED)

if(JPEG_FOUND)
  include_directories(${JPEG_INCLUDE_DIR})
endif(JPEG_FOUND)

if(OPENJPEG_FOUND)
  include_directories(${OPENJPEG_INCLUDE_DIR})
endif(OPENJPEG_FOUND)

if(TIFF_FOUND)
  include_directories(${TIFF_INCLUDE_DIR})
endif(TIFF_FOUND)

if(IS_DIRECTORY ${GTK2_GLIB_INCLUDE_DIR} AND 
   IS_DIRECTORY ${GTK2_CAIRO_INCLUDE_DIR} AND 
   IS_DIRECTORY ${GTK2_GLIBCONFIG_INCLUDE_DIR})
  include_directories(${GTK2_GLIB_INCLUDE_DIR})
  include_directories(${GTK2_CAIRO_INCLUDE_DIR})
  include_directories(${GTK2_GLIBCONFIG_INCLUDE_DIR})
else()
  # TODO: make this an error message
  message("Not all required GTK dependencies found.")
endif()

include(CheckIncludeFiles)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CheckCCompilerFlag)

check_include_files("dlfcn.h" HAVE_DLFCN_H)
check_include_files("inttypes.h" HAVE_INTTYPES_H)
check_include_files("memory.h" HAVE_MEMORY_H)
check_include_files("stdlib.h" HAVE_STDLIB_H)
check_include_files("strings.h" HAVE_STRINGS_H)
check_include_files("string.h" HAVE_STRING_H)
check_include_files("sys/stat.h" HAVE_SYS_STAT_H)
check_include_files("sys/types.h" HAVE_SYS_TYPES_H)
check_include_files("unistd.h" HAVE_UNISTD_H)
check_include_files("stdlib.h;stdarg.h;string.h;float.h" STDC_HEADERS)

check_type_size("_Bool" HAVE__BOOL)

check_symbol_exists(fseeko "stdio.h" HAVE_FSEEKO)

check_c_compiler_flag("-c -o" COMPILER_SUPPORTS_MINUS_C_MINUS_O)

if(NOT COMPILER_SUPPORTS_MINUS_C_MINUS_O)
  set(NO_MINUS_C_MINUS_O 1)
endif(NOT COMPILER_SUPPORTS_MINUS_C_MINUS_O)

if(OPENJPEG_FOUND)
  set(HAVE_OPENJPEG 1)
endif(OPENJPEG_FOUND)

# stuff that is yet missing
#undef HAVE_G_CHECKSUM_NEW
#undef LT_OBJDIR
#undef PACKAGE
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION
#undef VERSION
#undef _FILE_OFFSET_BITS
#undef _LARGEFILE_SOURCE
#undef _LARGE_FILES

# create a config.h header-file
# configure_file(${PROJECT_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)
# include_directories(${CMAKE_CURRENT_BINARY_DIR})
# add_definitions(-DHAVE_CONFIG_H)

include_directories(src)

add_subdirectory(src)
