# The configuration model, service states and the messages of docs/protocol.md.
add_subdirectory(src)

if(PROCESS_MANAGER_BUILD_TESTS)
    add_subdirectory(tests)
endif()
