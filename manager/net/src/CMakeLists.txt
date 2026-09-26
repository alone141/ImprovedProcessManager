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

# The only library that links libzmq: the socket wrapper, the command server
# and the report publishers. Everything above it speaks ZeroMQ through ZmqSocket.
add_library(process_manager_net STATIC
    CommandServer.cpp
    ReportPublisher.cpp
    ZmqSocket.cpp)
target_include_directories(process_manager_net PUBLIC ${PROJECT_SOURCE_DIR}/net/include)
target_link_libraries(process_manager_net PUBLIC process_manager_model PRIVATE ${PROCESS_MANAGER_ZMQ})
if(PROCESS_MANAGER_ZMQ STREQUAL "libzmq-static")
    target_compile_definitions(process_manager_net PRIVATE ZMQ_STATIC)
endif()
