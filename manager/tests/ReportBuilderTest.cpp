#include <gtest/gtest.h>

#include "ReportBuilder.hpp"
#include "DetailedReport.hpp"
#include "GpuMonitor.hpp"
#include "HealthRecord.hpp"
#include "Service.hpp"
#include "ServiceState.hpp"
#include "SystemMonitor.hpp"

#include <vector>

namespace
{

using Statuses = std::vector<process_manager::ServiceStatus>;

process_manager::ServiceStatus Status(process_manager::ServiceState state)
{
    process_manager::ServiceStatus status{};
    status.name = "svc";
    status.state = state;
    status.pid = 321;
    status.startTime = 1000;
    status.lastSeen = 2000;
    status.restartCount = 2;
    status.missedBeats = 1;
    status.usageValid = true;
    status.usage.cpuTimeUsec = 5555;
    status.usage.memoryBytes = 4096;
    status.usage.cgroup = true;
    status.autostart = true;
    return status;
}

} // namespace

TEST(ReportBuilderTest, HealthRecordsCarryLiveFigures)
{
    const process_manager::ReportBuilder builder{1, 0};
    const std::vector<process_manager::HealthRecord> records =
        builder.BuildHealth(Statuses{Status(process_manager::ServiceState::Running)}, 9000);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].processName, "svc");
    EXPECT_EQ(records[0].pid, 321);
    EXPECT_EQ(records[0].state, process_manager::RuntimeState::Running);
    EXPECT_EQ(records[0].startTime, 1000);
    EXPECT_EQ(records[0].cpuUsageInUsec, 5555u);
    EXPECT_EQ(records[0].memoryUsageInBytes, 4096u);
    EXPECT_EQ(records[0].lastSeen, 2000);
    EXPECT_EQ(records[0].restartCount, 2);
    EXPECT_EQ(records[0].snapshotTime, 9000);
}

TEST(ReportBuilderTest, DeadServicesHaveNoPidOrStartTime)
{
    const process_manager::ReportBuilder builder{1, 0};
    process_manager::ServiceStatus failed = Status(process_manager::ServiceState::Failed);
    failed.usageValid = false;
    const std::vector<process_manager::HealthRecord> records = builder.BuildHealth(Statuses{failed}, 9000);
    EXPECT_EQ(records[0].pid, 0);
    EXPECT_EQ(records[0].startTime, 0);
    EXPECT_EQ(records[0].memoryUsageInBytes, 0u);
    EXPECT_EQ(records[0].state, process_manager::RuntimeState::Unhealthy);
}

TEST(ReportBuilderTest, DetailedReportCarriesFlagsAndHost)
{
    const process_manager::ReportBuilder builder{77, 123};
    process_manager::SystemSnapshot system{};
    system.hostName = "rig";
    system.record.cpuCount = 4;
    process_manager::GpuSnapshot gpu{};
    process_manager::GpuDevice device{};
    device.name = "card";
    gpu.devices.push_back(device);
    const process_manager::DetailedReport report =
        builder.BuildReport(Statuses{Status(process_manager::ServiceState::Running)}, system, gpu, 456, 1000,
                            process_manager::report_flag_gpu);

    EXPECT_EQ(report.managerPid, 77);
    EXPECT_EQ(report.managerStartTime, 123);
    EXPECT_EQ(report.snapshotTime, 456);
    EXPECT_EQ(report.hostName, "rig");
    EXPECT_EQ(report.managerVersion, "1.0.0");
    EXPECT_EQ(report.flags, process_manager::report_flag_gpu);
    ASSERT_EQ(report.services.size(), 1u);
    const std::uint8_t flags = report.services[0].flags;
    EXPECT_NE(flags & process_manager::service_flag_autostart, 0);
    EXPECT_NE(flags & process_manager::service_flag_usage, 0);
    EXPECT_NE(flags & process_manager::service_flag_cgroup, 0);
    EXPECT_EQ(flags & process_manager::service_flag_gpu, 0);
    EXPECT_EQ(report.services[0].cpuTimeUsec, 5555u);
    ASSERT_EQ(report.gpus.size(), 1u);
    EXPECT_EQ(report.gpus[0].name, "card");
}
