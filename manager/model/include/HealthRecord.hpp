#pragma once

#include "WireReader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace process_manager
{

// The simplified health report. Its layout is the GUI's DetailedHealthReport
// (128 bytes, 8-byte packing), which is the historical name of this message.
constexpr std::size_t health_record_size = 128;
constexpr std::size_t health_name_size = 64;
constexpr std::size_t max_health_records = 10000;

enum class RuntimeState : std::uint8_t
{
    Unknown = 0,
    Starting = 1,
    Running = 2,
    Stopped = 3,
    Unhealthy = 4,
};

struct HealthRecord
{
    std::string processName;
    std::int32_t pid{0};
    std::uint64_t memoryUsageInBytes{0};
    std::uint64_t cpuUsageInUsec{0};
    RuntimeState state{RuntimeState::Unknown};
    std::int64_t startTime{0};    // ns since the Unix epoch, 0 when not running
    std::int64_t lastSeen{0};     // ns since the Unix epoch
    std::int32_t missedBeats{0};
    std::int32_t restartCount{0};
    std::int64_t snapshotTime{0}; // ns since the Unix epoch
};

/**
 * @brief Encode the health message: the records back to back, 128 bytes each.
 * @param records Records to encode. Names are cut to 63 bytes.
 * @return The message body. It is empty when there are no records.
 */
std::vector<std::uint8_t> EncodeHealthRecords(std::span<const HealthRecord> records);

/**
 * @brief Decode a health message, raw or prefixed with a little-endian uint32 count.
 * @param payload Message body.
 * @param out Receives the records. It is cleared first.
 * @return DecodeCode::Ok on success, DecodeCode::BadLength when the size fits neither form.
 */
DecodeCode DecodeHealthRecords(std::span<const std::uint8_t> payload, std::vector<HealthRecord>& out);

/**
 * @brief Map a wire byte to a runtime state.
 * @param value Byte from the wire.
 * @return The state, or RuntimeState::Unknown for a value outside the enum.
 */
RuntimeState RuntimeStateFromByte(std::uint8_t value);

} // namespace process_manager
