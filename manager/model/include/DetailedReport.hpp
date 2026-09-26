#pragma once

#include "ServiceConfig.hpp"
#include "ServiceState.hpp"
#include "WireReader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace process_manager
{

// The detailed report: frame "report", then a 192-byte header, the service
// records and the GPU records. The header carries the sizes, so a reader keeps
// working when later versions append fields to the header or the records.
constexpr std::string_view report_topic = "report";
constexpr std::uint32_t report_magic = 0x524D5042; // "BPMR" in little-endian byte order
constexpr std::uint16_t report_version = 1;
constexpr std::size_t report_header_size = 192;
constexpr std::size_t service_record_size = 368;
constexpr std::size_t gpu_record_size = 160;
constexpr std::size_t report_name_size = 32;
constexpr std::size_t report_binary_size = 128;
constexpr std::size_t report_description_size = 64;
constexpr std::size_t report_host_size = 64;
constexpr std::size_t report_manager_version_size = 16;
constexpr std::size_t gpu_name_size = 64;
constexpr std::size_t gpu_uuid_size = 48;
constexpr std::size_t max_report_records = 100000;

constexpr std::uint8_t service_flag_autostart = 0x01;
constexpr std::uint8_t service_flag_heartbeat = 0x02;
constexpr std::uint8_t service_flag_cgroup = 0x04;
constexpr std::uint8_t service_flag_usage = 0x08;
constexpr std::uint8_t service_flag_gpu = 0x10;
constexpr std::uint8_t service_flag_removing = 0x20;

constexpr std::uint32_t report_flag_cgroups = 0x1;
constexpr std::uint32_t report_flag_gpu = 0x2;
constexpr std::uint32_t report_flag_stopping = 0x4;
// Exit codes are Windows status codes, never minus a signal number.
constexpr std::uint32_t report_flag_windows = 0x8;

struct SystemRecord
{
    double cpuPercent{-1.0}; // whole host, 0 to 100; negative when unknown
    std::uint32_t cpuCount{0};
    std::uint64_t memoryTotalBytes{0};
    std::uint64_t memoryAvailableBytes{0};
    double loadAverage1{0.0};
    double loadAverage5{0.0};
    double loadAverage15{0.0};
    std::uint64_t uptimeSeconds{0};
};

struct ServiceRecord
{
    std::string name;
    std::string binary;
    std::string description;
    std::int32_t pid{0};
    ServiceState state{ServiceState::Stopped};
    RestartMode restartMode{RestartMode::Never};
    std::uint8_t flags{0};
    std::int32_t restartCount{0};
    std::int32_t missedBeats{0};
    std::int32_t lastExitCode{0}; // exit code, or minus the signal number
    std::int32_t processCount{0};
    std::int32_t threadCount{0};
    std::int32_t openFiles{-1};    // -1 when unknown
    std::int64_t startTime{0};     // ns since the Unix epoch; 0 when never started
    std::int64_t lastSeen{0};      // ns
    std::int64_t lastExitTime{0};  // ns; 0 when it never exited
    std::int64_t nextRestartTime{0}; // ns; 0 when no restart is scheduled
    std::uint64_t cpuTimeUsec{0};
    double cpuPercent{-1.0};       // 100 = one busy core; negative when unknown
    std::uint64_t memoryBytes{0};  // resident set of every process of the service
    std::uint64_t memoryPeakBytes{0};
    std::uint64_t memoryLimitBytes{0}; // 0 = no limit
    std::uint64_t ioReadBytes{0};
    std::uint64_t ioWriteBytes{0};
    double gpuPercent{-1.0};       // summed over GPUs; negative when unknown
    std::uint64_t gpuMemoryBytes{0};
    std::uint32_t oomKills{0};
    std::uint32_t cpuLimitPercent{0}; // 0 = no limit
};

struct GpuRecord
{
    std::string name;
    std::string uuid;
    std::uint32_t index{0};
    std::uint32_t temperatureC{0};
    double utilizationPercent{-1.0};
    double memoryUtilizationPercent{-1.0};
    std::uint64_t memoryTotalBytes{0};
    std::uint64_t memoryUsedBytes{0};
    std::uint32_t powerMilliwatts{0};
};

struct DetailedReport
{
    std::int32_t managerPid{0};
    std::int64_t snapshotTime{0};     // ns since the Unix epoch
    std::int64_t managerStartTime{0}; // ns since the Unix epoch
    std::uint32_t publishIntervalMs{0};
    std::uint32_t flags{0};
    SystemRecord system{};
    std::string hostName;
    std::string managerVersion;
    std::vector<ServiceRecord> services;
    std::vector<GpuRecord> gpus;
};

/**
 * @brief Encode a report as the payload frame that follows the "report" topic.
 * @param report Report to encode. Text is cut to fit its field.
 * @return The payload.
 */
std::vector<std::uint8_t> EncodeReport(const DetailedReport& report);

/**
 * @brief Decode a report payload.
 * @param payload Payload frame.
 * @param out Receives the report on success.
 * @return DecodeCode::Ok on success, otherwise what is wrong with the payload.
 */
DecodeCode DecodeReport(std::span<const std::uint8_t> payload, DetailedReport& out);

} // namespace process_manager
