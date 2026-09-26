include(GoogleTest)
add_executable(process_manager_service_tests
    CgroupTreeTest.cpp
    ConfigParserTest.cpp
    GpuMonitorTest.cpp
    NativeLauncherTest.cpp
    ProcessLauncherTest.cpp
    ProcessTableTest.cpp
    ReportBuilderTest.cpp
    ServiceManagerTest.cpp
    ServiceTest.cpp
    SystemMonitorTest.cpp)
# FakeLauncher.hpp, the launcher stand-in these tests share.
target_include_directories(process_manager_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(process_manager_service_tests PRIVATE process_manager_service ${PROCESS_MANAGER_GTEST_MAIN})
gtest_discover_tests(process_manager_service_tests DISCOVERY_TIMEOUT 60)
