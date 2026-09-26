include(GoogleTest)
add_executable(process_manager_common_tests
    ConsoleTest.cpp
    InstantTest.cpp
    LoggerTest.cpp
    ProcFsTest.cpp
    SignalWatcherTest.cpp
    SystemdNotifierTest.cpp
    WireReaderTest.cpp
    WireWriterTest.cpp)
target_link_libraries(process_manager_common_tests PRIVATE process_manager_common ${PROCESS_MANAGER_GTEST_MAIN})
gtest_discover_tests(process_manager_common_tests DISCOVERY_TIMEOUT 60)
