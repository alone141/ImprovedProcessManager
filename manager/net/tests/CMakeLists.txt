include(GoogleTest)
add_executable(process_manager_net_tests
    CommandServerTest.cpp
    ReportPublisherTest.cpp
    ZmqSocketTest.cpp)
target_link_libraries(process_manager_net_tests PRIVATE process_manager_net ${PROCESS_MANAGER_GTEST_MAIN})
gtest_discover_tests(process_manager_net_tests DISCOVERY_TIMEOUT 60)
