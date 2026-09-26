# The router: a ZeroMQ identity ROUTER that forwards [destination][payload] as
# [source][payload]. Everything but main is a library, so the tests here and
# the manager's tests can run a router in-process.
if(WIN32)
    set(PROCESS_MANAGER_ROUTER_PLATFORM_SOURCES windows/PeerAddress.cpp)
else()
    set(PROCESS_MANAGER_ROUTER_PLATFORM_SOURCES posix/PeerAddress.cpp)
endif()

add_library(process_manager_router STATIC
    Envelope.cpp
    MessageRouter.cpp
    RouterOptions.cpp
    ${PROCESS_MANAGER_ROUTER_PLATFORM_SOURCES})
target_include_directories(process_manager_router PUBLIC ${PROJECT_SOURCE_DIR}/beraynetworkmanager/include)
target_link_libraries(process_manager_router PUBLIC process_manager_net)
if(WIN32)
    # ws2_32: getpeername and inet_ntop for the connection log (PeerAddress).
    target_link_libraries(process_manager_router PUBLIC ws2_32)
endif()

add_executable(beraynetworkmanager main.cpp)
target_link_libraries(beraynetworkmanager PRIVATE process_manager_router)
set_target_properties(beraynetworkmanager PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
install(TARGETS beraynetworkmanager RUNTIME DESTINATION bin)
