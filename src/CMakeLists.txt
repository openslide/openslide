# configure the build's output directories for the src folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin) # .exe, .dll
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib) # .so
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib) # .lib .a

# let's check whether all required dependecies are available
# and where they are located
SET(TIFF_NAMES itktiff)
set(JPEG_NAMES itkjpeg itkjpeg8 itkjpeg12 itkjpeg16)

find_package(jpeg REQUIRED)
find_package(openjpeg REQUIRED)
find_package(tiff REQUIRED)
find_package(zlib REQUIRED)
find_package(GTK2 2.6 REQUIRED)

if(NOT IS_DIRECTORY ${GTK2_GLIB_INCLUDE_DIR} OR 
   NOT IS_DIRECTORY ${GTK2_CAIRO_INCLUDE_DIR} OR 
   NOT IS_DIRECTORY ${GTK2_GLIBCONFIG_INCLUDE_DIR})
  message(FATAL "Not all required GTK dependencies found.")
endif()

include_directories(${JPEG_INCLUDE_DIR})
include_directories(${OPENJPEG_INCLUDE_DIR})
include_directories(${TIFF_INCLUDE_DIR})
include_directories(${GTK2_GLIB_INCLUDE_DIR})
include_directories(${GTK2_CAIRO_INCLUDE_DIR})
include_directories(${GTK2_GLIBCONFIG_INCLUDE_DIR})

# prepare the target library
file(GLOB source_files "*.c" "*.cpp")

# remove test.c
file(GLOB unused_files "test.c")
list(REMOVE_ITEM source_files ${unused_files})

# we want to compile all .c files as C++ code on windows
if(WIN32)
	SET_SOURCE_FILES_PROPERTIES(${source_files} PROPERTIES LANGUAGE CXX)
endif()

set(library_files ${source_files})

# on windows add header files for the IDE
# TODO: do this only on visual studio builds
if (WIN32)
	file(GLOB header_files "*.h")
	
    # there is no valigrind/callgrind support on windows machines	
	file(GLOB grind_files "valgrind.h" "callgrind.h")
	list(REMOVE_ITEM header_files ${grind_files})
	
	list(APPEND library_files ${header_files})
endif()

# finally create the project
add_library(OpenSlide SHARED ${library_files})

add_definitions(-D_OPENSLIDE_BUILDING_DLL -DOPJ_STATIC)

if(WIN32)
  add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

target_link_libraries(OpenSlide ${JPEG_LIBRARY})
target_link_libraries(OpenSlide ${OPENJPEG_LIBRARY})
target_link_libraries(OpenSlide ${TIFF_LIBRARY})
target_link_libraries(OpenSlide ${ZLIB_LIBRARY})
target_link_libraries(OpenSlide ${GTK2_CAIRO_LIBRARY})
target_link_libraries(OpenSlide ${GTK2_GLIB_LIBRARY})
target_link_libraries(OpenSlide ${GTK2_GTHREAD_LIBRARY})
