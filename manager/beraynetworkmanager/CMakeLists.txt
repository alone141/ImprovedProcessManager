# The router program and the library its tests run in-process.
add_subdirectory(src)

if(PROCESS_MANAGER_BUILD_TESTS)
    add_subdirectory(tests)
endif()
