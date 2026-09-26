# Time, logging, the wire codec, /proc, and the program environment.
add_subdirectory(src)

if(PROCESS_MANAGER_BUILD_TESTS)
    add_subdirectory(tests)
endif()
