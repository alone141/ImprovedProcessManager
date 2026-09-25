# Vendored dependencies

The build downloads nothing. A dependency is either installed on the build
machine (found with `find_package` or pkg-config) or its source sits in this
directory, where the root `CMakeLists.txt` adds it with `add_subdirectory`.
The vendored copy wins when both exist.

| Directory | Package | Versions | Needed for |
|-----------|---------|----------|------------|
| `libzmq/` | ZeroMQ core library, source release | 4.1 or newer; built and tested with 4.3.2 | the manager |
| `googletest/` | GoogleTest, source release | 1.8.1 or newer | the tests only |

To prepare an offline machine, on a machine with network access:

1. Download the release archives, for example `zeromq-4.3.5.tar.gz` from
   github.com/zeromq/libzmq/releases and `googletest-1.14.0.tar.gz` (tag
   `v1.14.0`) from github.com/google/googletest.
2. Unpack them here so that `third_party/libzmq/CMakeLists.txt` and
   `third_party/googletest/CMakeLists.txt` exist.
3. Copy the whole `manager/` tree to the offline machine and build as usual.

The vendored libzmq is built as a static library without CURVE, libsodium,
draft APIs, tests or documentation (see the root `CMakeLists.txt`). The root file
adds both directories inside a `block()`. Their warnings never fail a
`-DCMAKE_COMPILE_WARNING_AS_ERROR=ON` build. CMake 4 refuses a
`cmake_minimum_required` below 3.5, which libzmq and GoogleTest 1.8 declare, so the
block raises their minimum to 3.5 through `CMAKE_POLICY_VERSION_MINIMUM`; CMake 3
ignores that variable. This path
is not exercised by the builds that verified this tree, which used the
distribution's `libzmq3-dev`; the options follow libzmq's own CMake file.
