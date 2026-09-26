include(GoogleTest)
add_executable(process_manager_router_tests
    EnvelopeTest.cpp
    MessageRouterTest.cpp
    RouterOptionsTest.cpp)
target_link_libraries(process_manager_router_tests PRIVATE process_manager_router ${PROCESS_MANAGER_GTEST_MAIN})
gtest_discover_tests(process_manager_router_tests DISCOVERY_TIMEOUT 60)
