#include <gtest/gtest.h>

#include "DetailedReport.hpp"
#include "WireReader.hpp"
#include "WireWriter.hpp"

#include <cstdint>
#include <vector>

namespace
{

process_manager::DetailedReport SampleReport()
{
    process_manager::DetailedReport report{};
    report.managerPid = 77;
    report.snapshotTime = 1700000000000000000;
    report.managerStartTime = 1690000000000000000;
    report.publishIntervalMs = 1000;
    report.flags = process_manager::report_flag_cgroups;
    report.system.cpuPercent = 12.5;
    report.system.cpuCount = 8;
    report.system.memoryTotalBytes = 16ull << 30;
    report.system.memoryAvailableBytes = 9ull << 30;
    report.system.loadAverage1 = 0.5;
    report.system.uptimeSeconds = 3600;
    report.hostName = "rig-01";
    report.managerVersion = "1.0.0";

    process_manager::ServiceRecord service{};
    service.name = "vision";
    service.binary = "/opt/app/bin/vision";
    service.description = "camera pipeline";
    service.pid = 1234;
    service.state = process_manager::ServiceState::Backoff;
    service.restartMode = process_manager::RestartMode::Always;
    service.flags = process_manager::service_flag_usage | process_manager::service_flag_gpu;
    service.restartCount = 4;
    service.lastExitCode = -11;
    service.openFiles = 17;
    service.cpuTimeUsec = 55;
    service.cpuPercent = 33.3;
    service.memoryBytes = 1ull << 28;
    service.gpuPercent = 41.0;
    service.gpuMemoryBytes = 1ull << 30;
    service.oomKills = 1;
    service.cpuLimitPercent = 150;
    report.services.push_back(service);

    process_manager::GpuRecord gpu{};
    gpu.name = "NVIDIA RTX A2000";
    gpu.uuid = "GPU-1234";
    gpu.index = 0;
    gpu.temperatureC = 51;
    gpu.utilizationPercent = 34.0;
    gpu.memoryTotalBytes = 8ull << 30;
    gpu.memoryUsedBytes = 2ull << 30;
    gpu.powerMilliwatts = 23000;
    report.gpus.push_back(gpu);
    return report;
}

} // namespace

TEST(DetailedReportTest, SizeFollowsTheCounts)
{
    const std::vector<std::uint8_t> bytes = process_manager::EncodeReport(SampleReport());
    EXPECT_EQ(bytes.size(),
              process_manager::report_header_size + process_manager::service_record_size + process_manager::gpu_record_size);
}

TEST(DetailedReportTest, HeaderStartsWithMagicVersionAndSizes)
{
    const std::vector<std::uint8_t> bytes = process_manager::EncodeReport(SampleReport());
    const process_manager::WireReader reader{bytes, 0, bytes.size()};
    EXPECT_EQ(reader.U32(0), process_manager::report_magic);
    EXPECT_EQ(reader.U16(4), 1);
    EXPECT_EQ(reader.U16(6), 192);
    EXPECT_EQ(reader.U16(8), 368);
    EXPECT_EQ(reader.U16(10), 160);
    EXPECT_EQ(reader.U32(12), 1u);
    EXPECT_EQ(reader.U32(16), 1u);
    EXPECT_EQ(bytes[0], 'B');
    EXPECT_EQ(bytes[3], 'R');
}

TEST(DetailedReportTest, RoundTripsEveryField)
{
    process_manager::DetailedReport decoded{};
    ASSERT_EQ(process_manager::DecodeReport(process_manager::EncodeReport(SampleReport()), decoded),
              process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.managerPid, 77);
    EXPECT_EQ(decoded.hostName, "rig-01");
    EXPECT_EQ(decoded.system.cpuCount, 8u);
    EXPECT_DOUBLE_EQ(decoded.system.cpuPercent, 12.5);
    ASSERT_EQ(decoded.services.size(), 1u);
    const process_manager::ServiceRecord& service = decoded.services[0];
    EXPECT_EQ(service.name, "vision");
    EXPECT_EQ(service.binary, "/opt/app/bin/vision");
    EXPECT_EQ(service.state, process_manager::ServiceState::Backoff);
    EXPECT_EQ(service.restartMode, process_manager::RestartMode::Always);
    EXPECT_EQ(service.lastExitCode, -11);
    EXPECT_EQ(service.openFiles, 17);
    EXPECT_DOUBLE_EQ(service.cpuPercent, 33.3);
    EXPECT_EQ(service.gpuMemoryBytes, 1ull << 30);
    EXPECT_EQ(service.oomKills, 1u);
    EXPECT_EQ(service.cpuLimitPercent, 150u);
    ASSERT_EQ(decoded.gpus.size(), 1u);
    EXPECT_EQ(decoded.gpus[0].name, "NVIDIA RTX A2000");
    EXPECT_EQ(decoded.gpus[0].powerMilliwatts, 23000u);
}

TEST(DetailedReportTest, BadMagicIsRejected)
{
    std::vector<std::uint8_t> bytes = process_manager::EncodeReport(SampleReport());
    bytes[0] = 'X';
    process_manager::DetailedReport decoded{};
    EXPECT_EQ(process_manager::DecodeReport(bytes, decoded), process_manager::DecodeCode::BadMagic);
}

TEST(DetailedReportTest, NewerVersionIsRejected)
{
    std::vector<std::uint8_t> bytes = process_manager::EncodeReport(SampleReport());
    bytes[4] = 2;
    process_manager::DetailedReport decoded{};
    EXPECT_EQ(process_manager::DecodeReport(bytes, decoded), process_manager::DecodeCode::UnsupportedVersion);
}

TEST(DetailedReportTest, TruncatedPayloadIsRejected)
{
    std::vector<std::uint8_t> bytes = process_manager::EncodeReport(SampleReport());
    bytes.pop_back();
    process_manager::DetailedReport decoded{};
    EXPECT_EQ(process_manager::DecodeReport(bytes, decoded), process_manager::DecodeCode::BadLength);
    const std::vector<std::uint8_t> tiny(10, 0);
    EXPECT_EQ(process_manager::DecodeReport(tiny, decoded), process_manager::DecodeCode::BadLength);
}

TEST(DetailedReportTest, LongerRecordsFromANewerManagerStillDecode)
{
    // A later manager appends 8 bytes to each service record and says so in the header.
    const std::vector<std::uint8_t> original = process_manager::EncodeReport(SampleReport());
    const std::size_t header = process_manager::report_header_size;
    const std::size_t record = process_manager::service_record_size;
    std::vector<std::uint8_t> bytes(original.begin(), original.begin() + static_cast<long>(header + record));
    bytes.insert(bytes.end(), 8, 0xEE);
    bytes.insert(bytes.end(), original.begin() + static_cast<long>(header + record), original.end());
    process_manager::WireWriter size{2};
    size.PutU16(0, static_cast<std::uint16_t>(record + 8));
    bytes[8] = size.Bytes()[0];
    bytes[9] = size.Bytes()[1];

    process_manager::DetailedReport decoded{};
    ASSERT_EQ(process_manager::DecodeReport(bytes, decoded), process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.services[0].name, "vision");
    EXPECT_EQ(decoded.gpus[0].uuid, "GPU-1234");
}

TEST(DetailedReportTest, ShorterRecordsAreRejected)
{
    std::vector<std::uint8_t> bytes = process_manager::EncodeReport(SampleReport());
    bytes[8] = 100;
    bytes[9] = 0;
    process_manager::DetailedReport decoded{};
    EXPECT_EQ(process_manager::DecodeReport(bytes, decoded), process_manager::DecodeCode::BadLayout);
}
