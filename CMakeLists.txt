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
  message(FATAL "Not all required GTK dependencies found.")
endif()

# add modules for config.h setup
include(Check64BitFormat)
include(CheckIncludeFiles)

check_include_files("inttypes.h" HAVE_INTTYPES_H)

if(NOT HAVE_INTTYPES_H)
  #64 Bit format
  check_64bit_format(ll FORMAT_64BIT)
  check_64bit_format(L FORMAT_64BIT)
  check_64bit_format(q FORMAT_64BIT)
  check_64bit_format(I64 FORMAT_64BIT)

  if(NOT FORMAT_64BIT)
    message(FATAL "64 bit format missing.")
  endif(NOT FORMAT_64BIT)

  set(PRId64 "${FORMAT_64BIT}d")
endif(NOT HAVE_INTTYPES_H)

# create a config.h header-file
configure_file(${PROJECT_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
add_definitions(-DHAVE_CONFIG_H)

include_directories(src)

add_subdirectory(src)
