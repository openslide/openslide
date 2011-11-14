set(apps
  openslide-quickhash1sum
  openslide-show-properties
  openslide-write-png
  )

include_directories(${GTK2_GLIB_INCLUDE_DIR})
include_directories(${GTK2_CAIRO_INCLUDE_DIR})
include_directories(${GTK2_GLIBCONFIG_INCLUDE_DIR})

find_package(PNG REQUIRED)

foreach(app ${apps})
add_executable(${app} ${app}.c)
target_link_libraries(${app} openslide)
if(${app} STREQUAL "openslide-write-png")
target_link_libraries(${app} ${PNG_LIBRARIES})
endif()
endforeach()
