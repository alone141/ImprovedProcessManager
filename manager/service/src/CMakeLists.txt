# Supervision and measurement: the configuration file, the service state
# machine, launching and stopping processes, and the samplers behind the
# detailed report. No ZeroMQ here.
if(WIN32)
    set(PROCESS_MANAGER_SERVICE_PLATFORM_SOURCES
        windows/NativeLauncher.cpp
        windows/ProcessTable.cpp
        windows/SystemMonitor.cpp)
else()
    set(PROCESS_MANAGER_SERVICE_PLATFORM_SOURCES
        posix/NativeLauncher.cpp
        posix/ProcessTable.cpp
        posix/SystemMonitor.cpp)
endif()

add_library(process_manager_service STATIC
    CgroupTree.cpp
    ConfigParser.cpp
    GpuMonitor.cpp
    ProcessLauncher.cpp
    ProcessTable.cpp
    ReportBuilder.cpp
    Service.cpp
    ServiceManager.cpp
    ${PROCESS_MANAGER_SERVICE_PLATFORM_SOURCES})
target_include_directories(process_manager_service PUBLIC ${PROJECT_SOURCE_DIR}/service/include)
# CMAKE_DL_LIBS: dlopen, which loads NVML at run time (GpuMonitor).
target_link_libraries(process_manager_service PUBLIC process_manager_model ${CMAKE_DL_LIBS})
if(WIN32)
    # user32: the WM_CLOSE stop of windowed services (NativeLauncher).
    target_link_libraries(process_manager_service PUBLIC user32)
endif()
