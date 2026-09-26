#include "HealthRecord.hpp"
#include "WireReader.hpp"
#include "WireWriter.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace process_manager
{

namespace
{

constexpr std::size_t name_offset = 0;
constexpr std::size_t pid_offset = 64;
constexpr std::size_t memory_offset = 72;
constexpr std::size_t cpu_offset = 80;
constexpr std::size_t state_offset = 88;
constexpr std::size_t start_offset = 96;
constexpr std::size_t last_seen_offset = 104;
constexpr std::size_t missed_offset = 112;
constexpr std::size_t restarts_offset = 116;
constexpr std::size_t snapshot_offset = 120;
constexpr std::size_t count_prefix_size = 4;

void EncodeOne(const HealthRecord& record, WireWriter& writer, std::size_t base)
{
    writer.PutText(base + name_offset, health_name_size, record.processName);
    writer.PutI32(base + pid_offset, record.pid);
    writer.PutU64(base + memory_offset, record.memoryUsageInBytes);
    writer.PutU64(base + cpu_offset, record.cpuUsageInUsec);
    writer.PutU8(base + state_offset, static_cast<std::uint8_t>(record.state));
    writer.PutI64(base + start_offset, record.startTime);
    writer.PutI64(base + last_seen_offset, record.lastSeen);
    writer.PutI32(base + missed_offset, record.missedBeats);
    writer.PutI32(base + restarts_offset, record.restartCount);
    writer.PutI64(base + snapshot_offset, record.snapshotTime);
}

HealthRecord DecodeOne(const WireReader& reader)
{
    HealthRecord record{};
    record.processName = reader.Text(name_offset, health_name_size);
    record.pid = reader.I32(pid_offset);
    record.memoryUsageInBytes = reader.U64(memory_offset);
    record.cpuUsageInUsec = reader.U64(cpu_offset);
    record.state = RuntimeStateFromByte(reader.U8(state_offset));
    record.startTime = reader.I64(start_offset);
    record.lastSeen = reader.I64(last_seen_offset);
    record.missedBeats = reader.I32(missed_offset);
    record.restartCount = reader.I32(restarts_offset);
    record.snapshotTime = reader.I64(snapshot_offset);
    return record;
}

} // namespace

std::vector<std::uint8_t> EncodeHealthRecords(std::span<const HealthRecord> records)
{
    WireWriter writer{records.size() * health_record_size};
    for (std::size_t i = 0; i < records.size(); ++i)
    {
        EncodeOne(records[i], writer, i * health_record_size);
    }
    return writer.TakeBytes();
}

DecodeCode DecodeHealthRecords(std::span<const std::uint8_t> payload, std::vector<HealthRecord>& out)
{
    out.clear();
    if (payload.empty())
    {
        return DecodeCode::Ok;
    }

    std::size_t base = 0;
    std::size_t count = 0;
    bool prefixed = false;
    if (payload.size() >= count_prefix_size)
    {
        const WireReader prefix{payload, 0, count_prefix_size};
        const std::size_t declared = prefix.U32(0);
        if (declared < max_health_records && payload.size() - count_prefix_size == declared * health_record_size)
        {
            base = count_prefix_size;
            count = declared;
            prefixed = true;
        }
    }
    if (!prefixed)
    {
        if (payload.size() % health_record_size != 0)
        {
            return DecodeCode::BadLength;
        }
        count = payload.size() / health_record_size;
    }

    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const WireReader reader{payload, base + i * health_record_size, health_record_size};
        out.push_back(DecodeOne(reader));
    }
    return DecodeCode::Ok;
}

RuntimeState RuntimeStateFromByte(std::uint8_t value)
{
    if (value > static_cast<std::uint8_t>(RuntimeState::Unhealthy))
    {
        return RuntimeState::Unknown;
    }

    return static_cast<RuntimeState>(value);
}

} // namespace process_manager
