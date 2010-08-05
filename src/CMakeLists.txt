# configure the build's output directories for the src folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin) # .exe, .dll
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib) # .so
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib) # .lib .a

# let's check whether all required dependecies are available
# and where they are located
SET(TIFF_NAMES tiff libtiff itktiff)
set(JPEG_NAMES jpeg libjpeg itkjpeg itkjpeg8 itkjpeg12 itkjpeg16)
set(ZLIB_NAMES zlib itkzlib)
set(OPENJPEG_NAMES openjpeg itkopenjpeg)

find_package(JPEG REQUIRED)
find_package(OpenJPEG REQUIRED)
find_package(TIFF REQUIRED)
find_package(ZLIB REQUIRED)
find_package(GTK2 2.6 REQUIRED)

if(NOT IS_DIRECTORY ${GTK2_GLIB_INCLUDE_DIR} OR 
   NOT IS_DIRECTORY ${GTK2_CAIRO_INCLUDE_DIR} OR 
   NOT IS_DIRECTORY ${GTK2_GLIBCONFIG_INCLUDE_DIR})
  message(FATAL_ERROR "Not all required GTK dependencies found.")
endif()

include_directories(${JPEG_INCLUDE_DIR})
include_directories(${OPENJPEG_INCLUDE_DIR})
include_directories(${TIFF_INCLUDE_DIR})
include_directories(${GTK2_GLIB_INCLUDE_DIR})
include_directories(${GTK2_CAIRO_INCLUDE_DIR})
include_directories(${GTK2_GLIBCONFIG_INCLUDE_DIR})

# prepare the target library
file(GLOB source_files "*.c" "*.cpp")

# we want to compile all .c files as C++ code on windows
if(MSVC_IDE)
    set_source_files_properties(${source_files} PROPERTIES LANGUAGE CXX)
endif()

set(library_files ${source_files})

# on windows add header files for the IDE
# TODO: do this only on visual studio builds
if(MSVC_IDE)
    file(GLOB header_files "*.h")
    
    # there is no valigrind/callgrind support on windows machines    
    file(GLOB grind_files "valgrind.h" "callgrind.h")
    list(REMOVE_ITEM header_files ${grind_files})
    
    list(APPEND library_files ${header_files})
endif()

if(MSVC_IDE)
  configure_file(./openslide-cmake-dll.rc.in ${CMAKE_CURRENT_BINARY_DIR}/openslide-cmake-dll.rc)
  list(APPEND library_files ${CMAKE_CURRENT_BINARY_DIR}/openslide-cmake-dll.rc)
endif()

if(MSVC_IDE)
  set(CMAKE_DEBUG_POSTFIX d)
endif()

# finally create the project
add_library(openslide SHARED ${library_files})

# configure OpenSlide installation
install(TARGETS openslide
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
)

install(FILES openslide.h openslide-features.h
  DESTINATION include
)

if(MSVC_IDE)
  install(FILES ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Debug/openslide${CMAKE_DEBUG_POSTFIX}.pdb
    DESTINATION bin
    CONFIGURATIONS Debug
  )
endif()

add_definitions(-D_OPENSLIDE_BUILDING_DLL)

if(CMAKE_COMPILER_IS_GNUCXX) # also true when the code is compiled as c-code
  add_definitions(-DG_DISABLE_DEPRECATED -DG_DISABLE_SINGLE_INCLUDES)
endif()

if(MSVC_IDE)
  add_definitions(-DOPJ_STATIC -D_CRT_SECURE_NO_WARNINGS)
endif()

#check whether we got debug versions of our required libraries and use them for linking if available (windows)
set( REQUIRED_LIBS
	${JPEG_LIBRARY}
	${OPENJPEG_LIBRARY}
	${TIFF_LIBRARY}
	${ZLIB_LIBRARY}
	${GTK2_CAIRO_LIBRARY}
	${GTK2_GLIB_LIBRARY}
	${GTK2_GTHREAD_LIBRARY} )
# loop over all required libs
foreach(l ${REQUIRED_LIBS})
	STRING(REGEX REPLACE "[.][lL][iI][bB]" "d.lib" DEBUG_${l} "${l}")
	IF(NOT EXISTS "${DEBUG_${l}}")
		SET(DEBUG_${l} ${l})
	ENDIF()
	TARGET_LINK_LIBRARIES( openslide 
		debug     ${DEBUG_${l}}
		optimized ${l} )
endforeach(l)