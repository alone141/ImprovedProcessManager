#include <gtest/gtest.h>

#include "StatusTable.hpp"
#include "DetailedReport.hpp"
#include "ServiceState.hpp"

#include <cstdint>
#include <string>

namespace
{

constexpr std::int64_t second = 1'000'000'000;

process_manager::DetailedReport SampleReport()
{
    process_manager::DetailedReport report{};
    report.managerVersion = "1.0.0";
    report.hostName = "rig-01";
    report.managerPid = 99;
    report.snapshotTime = 10'000 * second;
    report.managerStartTime = 10'000 * second - 3725 * second;
    report.system.cpuPercent = 12.5;
    report.system.cpuCount = 8;
    report.system.memoryTotalBytes = 16ull << 30;
    report.system.memoryAvailableBytes = 12ull << 30;

    process_manager::ServiceRecord running{};
    running.name = "vision";
    running.state = process_manager::ServiceState::Running;
    running.pid = 4242;
    running.startTime = report.snapshotTime - 65 * second;
    running.flags = process_manager::service_flag_usage | process_manager::service_flag_gpu;
    running.cpuPercent = 25.0;
    running.memoryBytes = 200ull << 20;
    running.gpuPercent = 40.0;
    running.gpuMemoryBytes = 1ull << 30;
    running.binary = "/opt/app/vision";
    report.services.push_back(running);

    process_manager::ServiceRecord waiting{};
    waiting.name = "planner";
    waiting.state = process_manager::ServiceState::Backoff;
    waiting.restartCount = 3;
    waiting.lastExitCode = 1;
    waiting.lastExitTime = report.snapshotTime - 2 * second;
    waiting.nextRestartTime = report.snapshotTime + 4 * second;
    report.services.push_back(waiting);
    return report;
}

} // namespace

TEST(StatusTableTest, FormatsBytes)
{
    EXPECT_EQ(process_manager::FormatBytes(0), "0 B");
    EXPECT_EQ(process_manager::FormatBytes(1023), "1023 B");
    EXPECT_EQ(process_manager::FormatBytes(1536), "1.5 KiB");
    EXPECT_EQ(process_manager::FormatBytes(200ull << 20), "200.0 MiB");
    EXPECT_EQ(process_manager::FormatBytes(3ull << 40), "3.0 TiB");
}

TEST(StatusTableTest, FormatsDurations)
{
    EXPECT_EQ(process_manager::FormatDuration(-5), "0s");
    EXPECT_EQ(process_manager::FormatDuration(45 * second), "45s");
    EXPECT_EQ(process_manager::FormatDuration(725 * second), "12m 05s");
    EXPECT_EQ(process_manager::FormatDuration(11040 * second), "3h 04m");
    EXPECT_EQ(process_manager::FormatDuration(183600 * second), "2d 03h");
}

TEST(StatusTableTest, FormatsPercentages)
{
    EXPECT_EQ(process_manager::FormatPercent(12.34), "12.3");
    EXPECT_EQ(process_manager::FormatPercent(-1.0), "-");
}

TEST(StatusTableTest, RendersTheHostAndEveryService)
{
    const process_manager::StatusTable table{process_manager::TableOptions{}};
    const std::string text = table.Render(SampleReport());
    EXPECT_NE(text.find("berayprocessmanager 1.0.0 on rig-01, pid 99, up 1h 02m"), std::string::npos) << text;
    EXPECT_NE(text.find("CPU 12.5% of 8 cores, memory 4.0 GiB used of 16.0 GiB"), std::string::npos) << text;
    EXPECT_NE(text.find("SERVICE"), std::string::npos);
    EXPECT_NE(text.find("LAST EXIT"), std::string::npos);
    EXPECT_NE(text.find("vision"), std::string::npos);
    EXPECT_NE(text.find("4242"), std::string::npos);
    EXPECT_NE(text.find("1m 05s"), std::string::npos);
    EXPECT_NE(text.find("200.0 MiB"), std::string::npos);
    EXPECT_NE(text.find("backoff 4s"), std::string::npos) << text;
    EXPECT_NE(text.find("exit 1, 2s ago"), std::string::npos) << text;
    EXPECT_EQ(text.find("BINARY"), std::string::npos);
}

TEST(StatusTableTest, WideAddsColumns)
{
    const process_manager::StatusTable table{process_manager::TableOptions{true}};
    const std::string text = table.Render(SampleReport());
    EXPECT_NE(text.find("BINARY"), std::string::npos);
    EXPECT_NE(text.find("/opt/app/vision"), std::string::npos);
    EXPECT_NE(text.find("THREADS"), std::string::npos);
}

TEST(StatusTableTest, ColumnsLineUp)
{
    const process_manager::StatusTable table{process_manager::TableOptions{}};
    const std::string text = table.Render(SampleReport());
    const std::size_t header = text.find("SERVICE");
    const std::size_t headerLineStart = text.rfind('\n', header) + 1;
    const std::size_t stateColumn = text.find("STATE", header) - headerLineStart;
    const std::size_t row = text.find("planner");
    const std::size_t rowLineStart = text.rfind('\n', row) + 1;
    EXPECT_EQ(text.find("backoff", row) - rowLineStart, stateColumn);
}

TEST(StatusTableTest, SaysSoWhenNoServiceIsConfigured)
{
    process_manager::DetailedReport report = SampleReport();
    report.services.clear();
    const process_manager::StatusTable table{process_manager::TableOptions{}};
    EXPECT_NE(table.Render(report).find("No services are configured."), std::string::npos);
}

TEST(StatusTableTest, WindowsExitCodesAreNotShownAsSignals)
{
    process_manager::DetailedReport report = SampleReport();
    report.flags = process_manager::report_flag_windows;
    report.services[1].lastExitCode = -1;
    const process_manager::StatusTable table{process_manager::TableOptions{}};
    const std::string text = table.Render(report);
    EXPECT_NE(text.find("exit 0xFFFFFFFF"), std::string::npos) << text;
    EXPECT_EQ(text.find("signal 1"), std::string::npos);
}
