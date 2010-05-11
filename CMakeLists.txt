cmake_minimum_required(VERSION 2.6.2)
if(POLICY CMP0011)
   cmake_policy(SET CMP0011 NEW)
endif(POLICY CMP0011)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  message(FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there. (you may need to remove CMakeCache.txt ")
endif()

project(OpenSlide)

# configure the module path of modified modules
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# add modules for config.h setup
include(CheckIncludeFiles)

# definine the library version
set(OPENSLIDE_WORLD_VERSION "3")
set(OPENSLIDE_MAJOR_VERSION "1")
set(OPENSLIDE_MINOR_VERSION "1")
set(OPENSLIDE_VERSION_NUMBER ${OPENSLIDE_WORLD_VERSION}.${OPENSLIDE_MAJOR_VERSION}.${OPENSLIDE_MINOR_VERSION})

# create a config.h header-file
configure_file(${PROJECT_SOURCE_DIR}/config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h)
add_definitions(-DHAVE_CONFIG_H)

include_directories(${CMAKE_CURRENT_BINARY_DIR} src)

add_subdirectory(src)
add_subdirectory(test)
