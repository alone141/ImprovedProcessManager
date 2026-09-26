# The manager: the daemon loop with its sockets and its link to the router, and
# the command-line client, which is the same binary in client mode. Everything
# but main is a library, so the tests can drive it.
add_library(process_manager_daemon STATIC
    CommandLine.cpp
    Daemon.cpp
    ManagerClient.cpp
    RouterLink.cpp
    StatusTable.cpp)
target_include_directories(process_manager_daemon PUBLIC ${PROJECT_SOURCE_DIR}/berayprocessmanager/include)
target_link_libraries(process_manager_daemon PUBLIC process_manager_net process_manager_service)

add_executable(berayprocessmanager main.cpp)
target_link_libraries(berayprocessmanager PRIVATE process_manager_daemon)
set_target_properties(berayprocessmanager PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
install(TARGETS berayprocessmanager RUNTIME DESTINATION bin)
