enable_testing()

include_directories(${GTK2_GLIB_INCLUDE_DIR})
include_directories(${GTK2_CAIRO_INCLUDE_DIR})
include_directories(${GTK2_GLIBCONFIG_INCLUDE_DIR})

if(WIN32)
	set_source_files_properties("test.c" PROPERTIES LANGUAGE CXX)
endif()

add_executable(openslide_test "test.c")
target_link_libraries(openslide_test OpenSlide)
