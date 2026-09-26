#include "DetailedReport.hpp"
#include "ServiceConfig.hpp"
#include "ServiceState.hpp"
#include "WireReader.hpp"
#include "WireWriter.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace process_manager
{

namespace
{

// Header
constexpr std::size_t magic_offset = 0;
constexpr std::size_t version_offset = 4;
constexpr std::size_t header_size_offset = 6;
constexpr std::size_t record_size_offset = 8;
constexpr std::size_t gpu_size_offset = 10;
constexpr std::size_t service_count_offset = 12;
constexpr std::size_t gpu_count_offset = 16;
constexpr std::size_t manager_pid_offset = 20;
constexpr std::size_t snapshot_offset = 24;
constexpr std::size_t manager_start_offset = 32;
constexpr std::size_t interval_offset = 40;
constexpr std::size_t flags_offset = 44;
constexpr std::size_t host_cpu_offset = 48;
constexpr std::size_t memory_total_offset = 56;
constexpr std::size_t memory_available_offset = 64;
constexpr std::size_t load1_offset = 72;
constexpr std::size_t load5_offset = 80;
constexpr std::size_t load15_offset = 88;
constexpr std::size_t uptime_offset = 96;
constexpr std::size_t cpu_count_offset = 104;
constexpr std::size_t host_name_offset = 112;
constexpr std::size_t manager_version_offset = 176;

// Service record
constexpr std::size_t name_offset = 0;
constexpr std::size_t binary_offset = 32;
constexpr std::size_t description_offset = 160;
constexpr std::size_t pid_offset = 224;
constexpr std::size_t state_offset = 228;
constexpr std::size_t restart_mode_offset = 229;
constexpr std::size_t service_flags_offset = 230;
constexpr std::size_t restart_count_offset = 232;
constexpr std::size_t missed_offset = 236;
constexpr std::size_t exit_code_offset = 240;
constexpr std::size_t process_count_offset = 244;
constexpr std::size_t thread_count_offset = 248;
constexpr std::size_t open_files_offset = 252;
constexpr std::size_t start_time_offset = 256;
constexpr std::size_t last_seen_offset = 264;
constexpr std::size_t exit_time_offset = 272;
constexpr std::size_t next_restart_offset = 280;
constexpr std::size_t cpu_time_offset = 288;
constexpr std::size_t cpu_percent_offset = 296;
constexpr std::size_t memory_offset = 304;
constexpr std::size_t memory_peak_offset = 312;
constexpr std::size_t memory_limit_offset = 320;
constexpr std::size_t io_read_offset = 328;
constexpr std::size_t io_write_offset = 336;
constexpr std::size_t gpu_percent_offset = 344;
constexpr std::size_t gpu_memory_offset = 352;
constexpr std::size_t oom_offset = 360;
constexpr std::size_t cpu_limit_offset = 364;

// GPU record
constexpr std::size_t gpu_name_offset = 0;
constexpr std::size_t gpu_uuid_offset = 64;
constexpr std::size_t gpu_index_offset = 112;
constexpr std::size_t gpu_temperature_offset = 116;
constexpr std::size_t gpu_util_offset = 120;
constexpr std::size_t gpu_memory_util_offset = 128;
constexpr std::size_t gpu_memory_total_offset = 136;
constexpr std::size_t gpu_memory_used_offset = 144;
constexpr std::size_t gpu_power_offset = 152;

void EncodeService(const ServiceRecord& record, WireWriter& writer, std::size_t base)
{
    writer.PutText(base + name_offset, report_name_size, record.name);
    writer.PutText(base + binary_offset, report_binary_size, record.binary);
    writer.PutText(base + description_offset, report_description_size, record.description);
    writer.PutI32(base + pid_offset, record.pid);
    writer.PutU8(base + state_offset, static_cast<std::uint8_t>(record.state));
    writer.PutU8(base + restart_mode_offset, static_cast<std::uint8_t>(record.restartMode));
    writer.PutU8(base + service_flags_offset, record.flags);
    writer.PutI32(base + restart_count_offset, record.restartCount);
    writer.PutI32(base + missed_offset, record.missedBeats);
    writer.PutI32(base + exit_code_offset, record.lastExitCode);
    writer.PutI32(base + process_count_offset, record.processCount);
    writer.PutI32(base + thread_count_offset, record.threadCount);
    writer.PutI32(base + open_files_offset, record.openFiles);
    writer.PutI64(base + start_time_offset, record.startTime);
    writer.PutI64(base + last_seen_offset, record.lastSeen);
    writer.PutI64(base + exit_time_offset, record.lastExitTime);
    writer.PutI64(base + next_restart_offset, record.nextRestartTime);
    writer.PutU64(base + cpu_time_offset, record.cpuTimeUsec);
    writer.PutF64(base + cpu_percent_offset, record.cpuPercent);
    writer.PutU64(base + memory_offset, record.memoryBytes);
    writer.PutU64(base + memory_peak_offset, record.memoryPeakBytes);
    writer.PutU64(base + memory_limit_offset, record.memoryLimitBytes);
    writer.PutU64(base + io_read_offset, record.ioReadBytes);
    writer.PutU64(base + io_write_offset, record.ioWriteBytes);
    writer.PutF64(base + gpu_percent_offset, record.gpuPercent);
    writer.PutU64(base + gpu_memory_offset, record.gpuMemoryBytes);
    writer.PutU32(base + oom_offset, record.oomKills);
    writer.PutU32(base + cpu_limit_offset, record.cpuLimitPercent);
}

ServiceRecord DecodeService(const WireReader& reader)
{
    ServiceRecord record{};
    record.name = reader.Text(name_offset, report_name_size);
    record.binary = reader.Text(binary_offset, report_binary_size);
    record.description = reader.Text(description_offset, report_description_size);
    record.pid = reader.I32(pid_offset);
    record.state = ServiceStateFromByte(reader.U8(state_offset)).value_or(ServiceState::Stopped);
    record.restartMode = RestartModeFromByte(reader.U8(restart_mode_offset)).value_or(RestartMode::Never);
    record.flags = reader.U8(service_flags_offset);
    record.restartCount = reader.I32(restart_count_offset);
    record.missedBeats = reader.I32(missed_offset);
    record.lastExitCode = reader.I32(exit_code_offset);
    record.processCount = reader.I32(process_count_offset);
    record.threadCount = reader.I32(thread_count_offset);
    record.openFiles = reader.I32(open_files_offset);
    record.startTime = reader.I64(start_time_offset);
    record.lastSeen = reader.I64(last_seen_offset);
    record.lastExitTime = reader.I64(exit_time_offset);
    record.nextRestartTime = reader.I64(next_restart_offset);
    record.cpuTimeUsec = reader.U64(cpu_time_offset);
    record.cpuPercent = reader.F64(cpu_percent_offset);
    record.memoryBytes = reader.U64(memory_offset);
    record.memoryPeakBytes = reader.U64(memory_peak_offset);
    record.memoryLimitBytes = reader.U64(memory_limit_offset);
    record.ioReadBytes = reader.U64(io_read_offset);
    record.ioWriteBytes = reader.U64(io_write_offset);
    record.gpuPercent = reader.F64(gpu_percent_offset);
    record.gpuMemoryBytes = reader.U64(gpu_memory_offset);
    record.oomKills = reader.U32(oom_offset);
    record.cpuLimitPercent = reader.U32(cpu_limit_offset);
    return record;
}

void EncodeGpu(const GpuRecord& record, WireWriter& writer, std::size_t base)
{
    writer.PutText(base + gpu_name_offset, gpu_name_size, record.name);
    writer.PutText(base + gpu_uuid_offset, gpu_uuid_size, record.uuid);
    writer.PutU32(base + gpu_index_offset, record.index);
    writer.PutU32(base + gpu_temperature_offset, record.temperatureC);
    writer.PutF64(base + gpu_util_offset, record.utilizationPercent);
    writer.PutF64(base + gpu_memory_util_offset, record.memoryUtilizationPercent);
    writer.PutU64(base + gpu_memory_total_offset, record.memoryTotalBytes);
    writer.PutU64(base + gpu_memory_used_offset, record.memoryUsedBytes);
    writer.PutU32(base + gpu_power_offset, record.powerMilliwatts);
}

GpuRecord DecodeGpu(const WireReader& reader)
{
    GpuRecord record{};
    record.name = reader.Text(gpu_name_offset, gpu_name_size);
    record.uuid = reader.Text(gpu_uuid_offset, gpu_uuid_size);
    record.index = reader.U32(gpu_index_offset);
    record.temperatureC = reader.U32(gpu_temperature_offset);
    record.utilizationPercent = reader.F64(gpu_util_offset);
    record.memoryUtilizationPercent = reader.F64(gpu_memory_util_offset);
    record.memoryTotalBytes = reader.U64(gpu_memory_total_offset);
    record.memoryUsedBytes = reader.U64(gpu_memory_used_offset);
    record.powerMilliwatts = reader.U32(gpu_power_offset);
    return record;
}

} // namespace

std::vector<std::uint8_t> EncodeReport(const DetailedReport& report)
{
    const std::size_t total =
        report_header_size + report.services.size() * service_record_size + report.gpus.size() * gpu_record_size;
    WireWriter writer{total};
    writer.PutU32(magic_offset, report_magic);
    writer.PutU16(version_offset, report_version);
    writer.PutU16(header_size_offset, static_cast<std::uint16_t>(report_header_size));
    writer.PutU16(record_size_offset, static_cast<std::uint16_t>(service_record_size));
    writer.PutU16(gpu_size_offset, static_cast<std::uint16_t>(gpu_record_size));
    writer.PutU32(service_count_offset, static_cast<std::uint32_t>(report.services.size()));
    writer.PutU32(gpu_count_offset, static_cast<std::uint32_t>(report.gpus.size()));
    writer.PutI32(manager_pid_offset, report.managerPid);
    writer.PutI64(snapshot_offset, report.snapshotTime);
    writer.PutI64(manager_start_offset, report.managerStartTime);
    writer.PutU32(interval_offset, report.publishIntervalMs);
    writer.PutU32(flags_offset, report.flags);
    writer.PutF64(host_cpu_offset, report.system.cpuPercent);
    writer.PutU64(memory_total_offset, report.system.memoryTotalBytes);
    writer.PutU64(memory_available_offset, report.system.memoryAvailableBytes);
    writer.PutF64(load1_offset, report.system.loadAverage1);
    writer.PutF64(load5_offset, report.system.loadAverage5);
    writer.PutF64(load15_offset, report.system.loadAverage15);
    writer.PutU64(uptime_offset, report.system.uptimeSeconds);
    writer.PutU32(cpu_count_offset, report.system.cpuCount);
    writer.PutText(host_name_offset, report_host_size, report.hostName);
    writer.PutText(manager_version_offset, report_manager_version_size, report.managerVersion);

    std::size_t base = report_header_size;
    for (const ServiceRecord& service : report.services)
    {
        EncodeService(service, writer, base);
        base += service_record_size;
    }
    for (const GpuRecord& gpu : report.gpus)
    {
        EncodeGpu(gpu, writer, base);
        base += gpu_record_size;
    }
    return writer.TakeBytes();
}

DecodeCode DecodeReport(std::span<const std::uint8_t> payload, DetailedReport& out)
{
    if (payload.size() < report_header_size)
    {
        return DecodeCode::BadLength;
    }

    const WireReader header{payload, 0, payload.size()};
    if (header.U32(magic_offset) != report_magic)
    {
        return DecodeCode::BadMagic;
    }
    if (header.U16(version_offset) != report_version)
    {
        return DecodeCode::UnsupportedVersion;
    }

    const std::size_t headerSize = header.U16(header_size_offset);
    const std::size_t recordSize = header.U16(record_size_offset);
    const std::size_t gpuSize = header.U16(gpu_size_offset);
    const std::size_t serviceCount = header.U32(service_count_offset);
    const std::size_t gpuCount = header.U32(gpu_count_offset);
    if (headerSize < report_header_size || recordSize < service_record_size || gpuSize < gpu_record_size)
    {
        return DecodeCode::BadLayout;
    }
    if (serviceCount > max_report_records || gpuCount > max_report_records)
    {
        return DecodeCode::BadLength;
    }
    if (payload.size() != headerSize + serviceCount * recordSize + gpuCount * gpuSize)
    {
        return DecodeCode::BadLength;
    }

    DetailedReport report{};
    report.managerPid = header.I32(manager_pid_offset);
    report.snapshotTime = header.I64(snapshot_offset);
    report.managerStartTime = header.I64(manager_start_offset);
    report.publishIntervalMs = header.U32(interval_offset);
    report.flags = header.U32(flags_offset);
    report.system.cpuPercent = header.F64(host_cpu_offset);
    report.system.memoryTotalBytes = header.U64(memory_total_offset);
    report.system.memoryAvailableBytes = header.U64(memory_available_offset);
    report.system.loadAverage1 = header.F64(load1_offset);
    report.system.loadAverage5 = header.F64(load5_offset);
    report.system.loadAverage15 = header.F64(load15_offset);
    report.system.uptimeSeconds = header.U64(uptime_offset);
    report.system.cpuCount = header.U32(cpu_count_offset);
    report.hostName = header.Text(host_name_offset, report_host_size);
    report.managerVersion = header.Text(manager_version_offset, report_manager_version_size);

    std::size_t base = headerSize;
    report.services.reserve(serviceCount);
    for (std::size_t i = 0; i < serviceCount; ++i)
    {
        const WireReader reader{payload, base, recordSize};
        report.services.push_back(DecodeService(reader));
        base += recordSize;
    }
    report.gpus.reserve(gpuCount);
    for (std::size_t i = 0; i < gpuCount; ++i)
    {
        const WireReader reader{payload, base, gpuSize};
        report.gpus.push_back(DecodeGpu(reader));
        base += gpuSize;
    }

    out = std::move(report);
    return DecodeCode::Ok;
}

} // namespace process_manager
