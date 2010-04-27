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
