#include "ReportBuilder.hpp"
#include "DetailedReport.hpp"
#include "GpuMonitor.hpp"
#include "HealthRecord.hpp"
#include "Service.hpp"
#include "ServiceState.hpp"
#include "SystemMonitor.hpp"
#include "Version.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace process_manager
{

namespace
{

std::uint8_t ServiceFlags(const ServiceStatus& status)
{
    unsigned int flags = 0;
    flags |= status.autostart ? service_flag_autostart : 0u;
    flags |= status.heartbeat ? service_flag_heartbeat : 0u;
    flags |= status.usageValid && status.usage.cgroup ? service_flag_cgroup : 0u;
    flags |= status.usageValid ? service_flag_usage : 0u;
    flags |= status.gpuValid ? service_flag_gpu : 0u;
    flags |= status.removing ? service_flag_removing : 0u;
    return static_cast<std::uint8_t>(flags);
}

ServiceRecord ToServiceRecord(const ServiceStatus& status)
{
    ServiceRecord record{};
    record.name = status.name;
    record.binary = status.binary;
    record.description = status.description;
    record.pid = status.pid;
    record.state = status.state;
    record.restartMode = status.restartMode;
    record.flags = ServiceFlags(status);
    record.restartCount = status.restartCount;
    record.missedBeats = status.missedBeats;
    record.lastExitCode = status.lastExitCode;
    record.startTime = status.startTime;
    record.lastSeen = status.lastSeen;
    record.lastExitTime = status.lastExitTime;
    record.nextRestartTime = status.nextRestartTime;
    record.cpuPercent = status.cpuPercent;
    record.memoryLimitBytes = status.memoryLimitBytes;
    record.cpuLimitPercent = status.cpuLimitPercent;
    if (status.usageValid)
    {
        record.processCount = status.usage.processCount;
        record.threadCount = status.usage.threadCount;
        record.openFiles = status.usage.openFiles;
        record.cpuTimeUsec = status.usage.cpuTimeUsec;
        record.memoryBytes = status.usage.memoryBytes;
        record.memoryPeakBytes = status.usage.memoryPeakBytes;
        record.ioReadBytes = status.usage.ioReadBytes;
        record.ioWriteBytes = status.usage.ioWriteBytes;
        record.oomKills = status.usage.oomKills;
    }
    record.gpuPercent = status.gpuValid ? status.gpuPercent : -1.0;
    record.gpuMemoryBytes = status.gpuValid ? status.gpuMemoryBytes : 0;
    return record;
}

GpuRecord ToGpuRecord(const GpuDevice& device)
{
    GpuRecord record{};
    record.name = device.name;
    record.uuid = device.uuid;
    record.index = device.index;
    record.temperatureC = device.temperatureC;
    record.utilizationPercent = device.utilizationPercent;
    record.memoryUtilizationPercent = device.memoryUtilizationPercent;
    record.memoryTotalBytes = device.memoryTotalBytes;
    record.memoryUsedBytes = device.memoryUsedBytes;
    record.powerMilliwatts = device.powerMilliwatts;
    return record;
}

} // namespace

ReportBuilder::ReportBuilder(std::int32_t managerPid, std::int64_t managerStartTime)
    : managerPid{managerPid}, managerStartTime{managerStartTime}
{
}

std::vector<HealthRecord> ReportBuilder::BuildHealth(std::span<const ServiceStatus> statuses,
                                                     std::int64_t snapshotTime) const
{
    std::vector<HealthRecord> records{};
    records.reserve(statuses.size());
    for (const ServiceStatus& status : statuses)
    {
        const bool alive = IsAlive(status.state);
        HealthRecord record{};
        record.processName = status.name;
        record.pid = alive ? status.pid : 0;
        record.memoryUsageInBytes = status.usageValid ? status.usage.memoryBytes : 0;
        record.cpuUsageInUsec = status.usageValid ? status.usage.cpuTimeUsec : 0;
        record.state = ToRuntimeState(status.state);
        record.startTime = alive ? status.startTime : 0;
        record.lastSeen = status.lastSeen;
        record.missedBeats = status.missedBeats;
        record.restartCount = status.restartCount;
        record.snapshotTime = snapshotTime;
        records.push_back(record);
    }
    return records;
}

DetailedReport ReportBuilder::BuildReport(std::span<const ServiceStatus> statuses, const SystemSnapshot& system,
                                          const GpuSnapshot& gpu, std::int64_t snapshotTime,
                                          std::uint32_t publishIntervalMs, std::uint32_t flags) const
{
    DetailedReport report{};
    report.managerPid = managerPid;
    report.snapshotTime = snapshotTime;
    report.managerStartTime = managerStartTime;
    report.publishIntervalMs = publishIntervalMs;
    report.flags = flags;
    report.system = system.record;
    report.hostName = system.hostName;
    report.managerVersion = std::string{manager_version};
    report.services.reserve(statuses.size());
    for (const ServiceStatus& status : statuses)
    {
        report.services.push_back(ToServiceRecord(status));
    }
    for (const GpuDevice& device : gpu.devices)
    {
        report.gpus.push_back(ToGpuRecord(device));
    }
    return report;
}

} // namespace process_manager
