# libzmq, in order: the vendored copy (added by the root file), libzmq's own
# CMake package, pkg-config (distribution packages), a plain header and library
# search. Pass ZeroMQ_INCLUDE_DIR and ZeroMQ_LIBRARY to point at a custom build.
if(TARGET libzmq-static)
    set(PROCESS_MANAGER_ZMQ libzmq-static)
elseif(TARGET libzmq)
    set(PROCESS_MANAGER_ZMQ libzmq)
else()
    find_package(ZeroMQ CONFIG QUIET)
    if(TARGET libzmq)
        set(PROCESS_MANAGER_ZMQ libzmq)
    elseif(TARGET libzmq-static)
        set(PROCESS_MANAGER_ZMQ libzmq-static)
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(LIBZMQ QUIET IMPORTED_TARGET libzmq>=4.1)
        endif()
        if(TARGET PkgConfig::LIBZMQ)
            set(PROCESS_MANAGER_ZMQ PkgConfig::LIBZMQ)
        else()
            find_path(ZeroMQ_INCLUDE_DIR zmq.h)
            find_library(ZeroMQ_LIBRARY NAMES zmq libzmq)
            if(ZeroMQ_INCLUDE_DIR AND ZeroMQ_LIBRARY)
                add_library(process_manager_libzmq UNKNOWN IMPORTED)
                set_target_properties(process_manager_libzmq PROPERTIES
                    IMPORTED_LOCATION "${ZeroMQ_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${ZeroMQ_INCLUDE_DIR}")
                set(PROCESS_MANAGER_ZMQ process_manager_libzmq)
            endif()
        endif()
    endif()
endif()
if(NOT PROCESS_MANAGER_ZMQ)
    message(FATAL_ERROR
        "libzmq 4.1 or newer was not found. Install it offline "
        "(Debian/Ubuntu: libzmq3-dev, RHEL/Fedora: zeromq-devel), "
        "unpack the libzmq source into third_party/libzmq, "
        "or pass -DZeroMQ_INCLUDE_DIR=... -DZeroMQ_LIBRARY=...")
endif()
message(STATUS "libzmq: ${PROCESS_MANAGER_ZMQ}")

if(WIN32)
    set(PROCESS_MANAGER_PLATFORM_SOURCES
        windows/Console.cpp
        windows/NativeLauncher.cpp
        windows/PeerAddress.cpp
        windows/ProcessTable.cpp
        windows/SignalWatcher.cpp
        windows/SystemMonitor.cpp
        windows/SystemdNotifier.cpp)
else()
    set(PROCESS_MANAGER_PLATFORM_SOURCES
        posix/Console.cpp
        posix/NativeLauncher.cpp
        posix/PeerAddress.cpp
        posix/ProcessTable.cpp
        posix/SignalWatcher.cpp
        posix/SystemMonitor.cpp
        posix/SystemdNotifier.cpp)
endif()

# Everything that does not talk ZeroMQ: configuration, the service state
# machine, process launching and sampling, the wire format, the status table.
add_library(process_manager_core STATIC
    CgroupTree.cpp
    CommandLine.cpp
    CommandMessage.cpp
    ConfigParser.cpp
    DetailedReport.cpp
    GpuMonitor.cpp
    HealthRecord.cpp
    Instant.cpp
    Logger.cpp
    ProcFs.cpp
    ProcessLauncher.cpp
    ProcessTable.cpp
    ReportBuilder.cpp
    RouterOptions.cpp
    Service.cpp
    ServiceConfig.cpp
    ServiceManager.cpp
    ServiceState.cpp
    StatusTable.cpp
    WireReader.cpp
    WireWriter.cpp
    ${PROCESS_MANAGER_PLATFORM_SOURCES})
target_include_directories(process_manager_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
find_package(Threads REQUIRED) # pthread_sigmask is in libpthread before glibc 2.34
target_link_libraries(process_manager_core PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
if(WIN32)
    # ws2_32: getpeername and inet_ntop for the router's connection log.
    target_link_libraries(process_manager_core PUBLIC user32 shell32 ws2_32)
endif()

# The ZeroMQ side: sockets, the publishers, the command server, the router and
# the manager's link to it, the client and the daemon loop.
add_library(process_manager_net STATIC
    CommandServer.cpp
    Daemon.cpp
    Envelope.cpp
    ManagerClient.cpp
    MessageRouter.cpp
    ReportPublisher.cpp
    RouterLink.cpp
    ZmqSocket.cpp)
target_link_libraries(process_manager_net PUBLIC process_manager_core PRIVATE ${PROCESS_MANAGER_ZMQ})
if(PROCESS_MANAGER_ZMQ STREQUAL "libzmq-static")
    target_compile_definitions(process_manager_net PRIVATE ZMQ_STATIC)
endif()

add_executable(berayprocessmanager main.cpp)
target_link_libraries(berayprocessmanager PRIVATE process_manager_net)

# The router: the NetworkManager design as a second program on the same libraries.
add_executable(beraynetworkmanager RouterMain.cpp)
target_link_libraries(beraynetworkmanager PRIVATE process_manager_net)

install(TARGETS berayprocessmanager beraynetworkmanager RUNTIME DESTINATION bin)
