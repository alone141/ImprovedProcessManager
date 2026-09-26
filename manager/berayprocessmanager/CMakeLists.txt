# The manager program: the daemon, its router link and the command-line client.
add_subdirectory(src)

if(PROCESS_MANAGER_BUILD_TESTS)
    add_subdirectory(tests)
endif()
