include(GoogleTest)
add_executable(process_manager_model_tests
    CommandMessageTest.cpp
    DetailedReportTest.cpp
    HealthRecordTest.cpp
    ServiceConfigTest.cpp
    ServiceStateTest.cpp)
target_link_libraries(process_manager_model_tests PRIVATE process_manager_model ${PROCESS_MANAGER_GTEST_MAIN})
gtest_discover_tests(process_manager_model_tests DISCOVERY_TIMEOUT 60)
