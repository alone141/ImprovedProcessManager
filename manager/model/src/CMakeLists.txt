# What every other directory shares: the configuration model, the service
# states, and the messages of docs/protocol.md (health records, the detailed
# report, commands and replies).
add_library(process_manager_model STATIC
    CommandMessage.cpp
    DetailedReport.cpp
    HealthRecord.cpp
    ServiceConfig.cpp
    ServiceState.cpp)
target_include_directories(process_manager_model PUBLIC ${PROJECT_SOURCE_DIR}/model/include)
target_link_libraries(process_manager_model PUBLIC process_manager_common)
