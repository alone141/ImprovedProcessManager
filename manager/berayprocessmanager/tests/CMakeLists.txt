include(GoogleTest)
add_executable(process_manager_daemon_tests
    CommandLineTest.cpp
    DaemonTest.cpp
    ManagerClientTest.cpp
    RouterLinkTest.cpp
    StatusTableTest.cpp)
# The router library too: these tests run a router in-process and reach the
# manager through it.
target_link_libraries(process_manager_daemon_tests PRIVATE process_manager_daemon process_manager_router ${PROCESS_MANAGER_GTEST_MAIN})
gtest_discover_tests(process_manager_daemon_tests DISCOVERY_TIMEOUT 60)
