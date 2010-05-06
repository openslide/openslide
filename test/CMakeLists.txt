enable_testing()

# configure the build's output directories for the test folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin) # .exe, .dll
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib) # .so
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib) # .lib .a

include_directories(${GTK2_GLIB_INCLUDE_DIR})
include_directories(${GTK2_CAIRO_INCLUDE_DIR})
include_directories(${GTK2_GLIBCONFIG_INCLUDE_DIR})

if(WIN32)
	set_source_files_properties("test.c" PROPERTIES LANGUAGE CXX)
endif()

add_executable(openslide_test "test.c")
target_link_libraries(openslide_test openslide)
