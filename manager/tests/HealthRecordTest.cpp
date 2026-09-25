#include <gtest/gtest.h>

#include "HealthRecord.hpp"
#include "WireReader.hpp"
#include "WireWriter.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace
{

using Records = std::vector<process_manager::HealthRecord>;

process_manager::HealthRecord SampleRecord()
{
    process_manager::HealthRecord record{};
    record.processName = "sensor_fusion";
    record.pid = 4242;
    record.memoryUsageInBytes = 123456789;
    record.cpuUsageInUsec = 987654321;
    record.state = process_manager::RuntimeState::Running;
    record.startTime = 1700000000000000000;
    record.lastSeen = 1700000000500000000;
    record.missedBeats = 2;
    record.restartCount = 3;
    record.snapshotTime = 1700000001000000000;
    return record;
}

} // namespace

TEST(HealthRecordTest, EncodesOneRecordPerHundredTwentyEightBytes)
{
    const std::vector<process_manager::HealthRecord> records{SampleRecord(), SampleRecord()};
    EXPECT_EQ(process_manager::EncodeHealthRecords(records).size(), 256u);
    EXPECT_TRUE(process_manager::EncodeHealthRecords(Records{}).empty());
}

TEST(HealthRecordTest, MatchesTheGuiCtypesLayout)
{
    // Offsets of DetailedHealthReport in the GUI's health_structs.py (8-byte packing).
    const std::vector<std::uint8_t> bytes = process_manager::EncodeHealthRecords(Records{SampleRecord()});
    const process_manager::WireReader reader{bytes, 0, bytes.size()};
    EXPECT_EQ(reader.Text(0, 64), "sensor_fusion");
    EXPECT_EQ(reader.I32(64), 4242);
    EXPECT_EQ(reader.U32(68), 0u);
    EXPECT_EQ(reader.U64(72), 123456789u);
    EXPECT_EQ(reader.U64(80), 987654321u);
    EXPECT_EQ(reader.U8(88), 2);
    EXPECT_EQ(reader.I64(96), 1700000000000000000);
    EXPECT_EQ(reader.I64(104), 1700000000500000000);
    EXPECT_EQ(reader.I32(112), 2);
    EXPECT_EQ(reader.I32(116), 3);
    EXPECT_EQ(reader.I64(120), 1700000001000000000);
}

TEST(HealthRecordTest, RoundTripsRawArrays)
{
    const std::vector<std::uint8_t> bytes = process_manager::EncodeHealthRecords(Records{SampleRecord()});
    std::vector<process_manager::HealthRecord> decoded{};
    ASSERT_EQ(process_manager::DecodeHealthRecords(bytes, decoded), process_manager::DecodeCode::Ok);
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0].processName, "sensor_fusion");
    EXPECT_EQ(decoded[0].pid, 4242);
    EXPECT_EQ(decoded[0].state, process_manager::RuntimeState::Running);
    EXPECT_EQ(decoded[0].restartCount, 3);
}

TEST(HealthRecordTest, DecodesCountPrefixedArrays)
{
    const std::vector<std::uint8_t> raw = process_manager::EncodeHealthRecords(Records{SampleRecord(), SampleRecord()});
    process_manager::WireWriter prefix{4};
    prefix.PutU32(0, 2);
    std::vector<std::uint8_t> bytes = prefix.TakeBytes();
    bytes.insert(bytes.end(), raw.begin(), raw.end());

    std::vector<process_manager::HealthRecord> decoded{};
    ASSERT_EQ(process_manager::DecodeHealthRecords(bytes, decoded), process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.size(), 2u);
}

TEST(HealthRecordTest, EmptyPrefixedArrayHasNoRecords)
{
    const std::vector<std::uint8_t> bytes{0, 0, 0, 0};
    std::vector<process_manager::HealthRecord> decoded{SampleRecord()};
    EXPECT_EQ(process_manager::DecodeHealthRecords(bytes, decoded), process_manager::DecodeCode::Ok);
    EXPECT_TRUE(decoded.empty());
}

TEST(HealthRecordTest, OddLengthIsRejected)
{
    const std::vector<std::uint8_t> bytes(130, 0);
    std::vector<process_manager::HealthRecord> decoded{};
    EXPECT_EQ(process_manager::DecodeHealthRecords(bytes, decoded), process_manager::DecodeCode::BadLength);
}

TEST(HealthRecordTest, LongNamesAreCutToSixtyThreeBytes)
{
    process_manager::HealthRecord record = SampleRecord();
    record.processName = std::string(80, 'n');
    std::vector<process_manager::HealthRecord> decoded{};
    ASSERT_EQ(process_manager::DecodeHealthRecords(process_manager::EncodeHealthRecords(Records{record}), decoded),
              process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded[0].processName.size(), 63u);
}

TEST(HealthRecordTest, UnknownStateBytesReadAsUnknown)
{
    EXPECT_EQ(process_manager::RuntimeStateFromByte(4), process_manager::RuntimeState::Unhealthy);
    EXPECT_EQ(process_manager::RuntimeStateFromByte(9), process_manager::RuntimeState::Unknown);
}
