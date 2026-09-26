# GoogleTest, in order: the vendored copy (added by the root file), an installed
# CMake package, CMake's FindGTest (set GTEST_ROOT for a custom location).
if(NOT TARGET GTest::gtest_main AND NOT TARGET gtest_main)
    find_package(GTest CONFIG QUIET)
    if(NOT TARGET GTest::gtest_main)
        find_package(GTest QUIET)
    endif()
endif()

if(TARGET GTest::gtest_main)
    set(PROCESS_MANAGER_GTEST_MAIN GTest::gtest_main)
elseif(TARGET gtest_main)
    set(PROCESS_MANAGER_GTEST_MAIN gtest_main)
else()
    message(FATAL_ERROR
        "GoogleTest 1.8.1 or newer was not found. Install it offline "
        "(Debian/Ubuntu: libgtest-dev, RHEL/Fedora: gtest-devel), unpack its source "
        "into third_party/googletest, pass -DGTEST_ROOT=..., or configure with "
        "-DPROCESS_MANAGER_BUILD_TESTS=OFF")
endif()

# The integration tests run the daemon and fake peers on std::thread.
find_package(Threads REQUIRED)

include(GoogleTest)
add_executable(process_manager_tests
    CgroupTreeTest.cpp
    CommandLineTest.cpp
    CommandMessageTest.cpp
    CommandServerTest.cpp
    ConfigParserTest.cpp
    ConsoleTest.cpp
    DaemonTest.cpp
    DetailedReportTest.cpp
    EnvelopeTest.cpp
    GpuMonitorTest.cpp
    HealthRecordTest.cpp
    InstantTest.cpp
    LoggerTest.cpp
    ManagerClientTest.cpp
    MessageRouterTest.cpp
    NativeLauncherTest.cpp
    ProcFsTest.cpp
    ProcessLauncherTest.cpp
    ProcessTableTest.cpp
    ReportBuilderTest.cpp
    ReportPublisherTest.cpp
    RouterLinkTest.cpp
    RouterOptionsTest.cpp
    ServiceConfigTest.cpp
    ServiceManagerTest.cpp
    ServiceStateTest.cpp
    ServiceTest.cpp
    SignalWatcherTest.cpp
    StatusTableTest.cpp
    SystemMonitorTest.cpp
    SystemdNotifierTest.cpp
    WireReaderTest.cpp
    WireWriterTest.cpp
    ZmqSocketTest.cpp)
target_include_directories(process_manager_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(process_manager_tests PRIVATE process_manager_net ${PROCESS_MANAGER_GTEST_MAIN} Threads::Threads)
gtest_discover_tests(process_manager_tests DISCOVERY_TIMEOUT 60)
