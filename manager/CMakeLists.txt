cmake_minimum_required(VERSION 3.30)
project(process_manager VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# The sources use no C++20 modules, so no compiler has to scan them for imports.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(PROCESS_MANAGER_BUILD_TESTS "Build the GoogleTest suite" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The oldest compilers whose standard library has C++20's <span>, which the
# sources use for every sequence they only read.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
    message(FATAL_ERROR
        "GCC ${CMAKE_CXX_COMPILER_VERSION} lacks C++20's <span>. Install GCC 10 or newer offline "
        "(Ubuntu 20.04: g++-10) and configure with -DCMAKE_CXX_COMPILER=g++-10.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old for C++20. Install Clang 10 or newer offline.")
endif()
if(MSVC AND MSVC_VERSION LESS 1929)
    message(FATAL_ERROR "Visual Studio 2019 16.10 or newer is required for C++20.")
endif()

# Offline dependencies (versions in third_party/README.md). Each one is either
# vendored under third_party/ and added here, before the targets that link it,
# or already installed and found by the directory that uses it.
#   libzmq      4.1 or newer, built with 4.3.2   (src/)
#   GoogleTest  1.8.1 or newer, tests only        (tests/)
block()
    # Vendored code keeps its own warnings, also under -DCMAKE_COMPILE_WARNING_AS_ERROR=ON.
    set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
    # libzmq and older GoogleTest releases declare cmake_minimum_required below 3.5,
    # which CMake 4 refuses unless this raises it; CMake 3 ignores the variable.
    if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    endif()
    if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/libzmq/CMakeLists.txt")
        set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
        set(WITH_DOCS OFF CACHE BOOL "" FORCE)
        set(WITH_LIBSODIUM OFF CACHE BOOL "" FORCE)
        set(ENABLE_CURVE OFF CACHE BOOL "" FORCE)
        set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
        set(ENABLE_CPACK OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/libzmq EXCLUDE_FROM_ALL)
    endif()
    if(PROCESS_MANAGER_BUILD_TESTS AND EXISTS "${PROJECT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
    endif()
endblock()

# Project-wide compile settings, for the directories below; vendored code builds
# with its own. -DCMAKE_COMPILE_WARNING_AS_ERROR=ON turns the warnings into errors.
if(MSVC)
    # C4458: a constructor parameter named after the member it initializes (the house style).
    add_compile_options(/W4 /wd4458 /permissive- /utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(src)

if(PROCESS_MANAGER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Files that are not build targets: the example configuration and the systemd unit.
install(FILES config/services.conf DESTINATION share/berayprocessmanager)
install(FILES packaging/berayprocessmanager.service DESTINATION share/berayprocessmanager)
