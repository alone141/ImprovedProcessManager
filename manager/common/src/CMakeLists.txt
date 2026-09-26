# Nothing here knows about services or ZeroMQ: time, logging, the wire codec,
# /proc reading, and the program environment (console, signals, systemd).
if(WIN32)
    set(PROCESS_MANAGER_COMMON_PLATFORM_SOURCES
        windows/Console.cpp
        windows/SignalWatcher.cpp
        windows/SystemdNotifier.cpp)
else()
    set(PROCESS_MANAGER_COMMON_PLATFORM_SOURCES
        posix/Console.cpp
        posix/SignalWatcher.cpp
        posix/SystemdNotifier.cpp)
endif()

add_library(process_manager_common STATIC
    Instant.cpp
    Logger.cpp
    ProcFs.cpp
    WireReader.cpp
    WireWriter.cpp
    ${PROCESS_MANAGER_COMMON_PLATFORM_SOURCES})
target_include_directories(process_manager_common PUBLIC ${PROJECT_SOURCE_DIR}/common/include)
# std::thread and pthread_sigmask are in libpthread before glibc 2.34; every
# library and test above this one inherits the link.
find_package(Threads REQUIRED)
target_link_libraries(process_manager_common PUBLIC Threads::Threads)
if(WIN32)
    # shell32: CommandLineToArgvW, the wide command line (Console).
    target_link_libraries(process_manager_common PUBLIC shell32)
endif()
