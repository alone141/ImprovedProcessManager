# ZeroMQ: the socket wrapper, the command server, the report publishers.
add_subdirectory(src)

if(PROCESS_MANAGER_BUILD_TESTS)
    add_subdirectory(tests)
endif()
