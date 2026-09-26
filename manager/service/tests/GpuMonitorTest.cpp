#include <gtest/gtest.h>

#include "GpuMonitor.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace
{

using Samples = std::vector<process_manager::GpuProcessSample>;

process_manager::GpuProcessSample Memory(unsigned int device, int pid, std::uint64_t bytes)
{
    process_manager::GpuProcessSample sample{};
    sample.device = device;
    sample.pid = pid;
    sample.hasMemory = true;
    sample.memoryBytes = bytes;
    return sample;
}

process_manager::GpuProcessSample Utilization(unsigned int device, int pid, double percent)
{
    process_manager::GpuProcessSample sample{};
    sample.device = device;
    sample.pid = pid;
    sample.hasUtilization = true;
    sample.utilizationPercent = percent;
    return sample;
}

} // namespace

TEST(GpuMonitorTest, TakesTheLargestReadingPerDeviceAndSumsDevices)
{
    const std::vector<process_manager::GpuProcessSample> samples{
        Memory(0, 10, 100), Memory(0, 10, 300), Memory(1, 10, 50),
        Utilization(0, 10, 20.0), Utilization(0, 10, 35.0), Utilization(1, 10, 5.0)};
    const std::unordered_map<int, process_manager::GpuProcessUsage> usage =
        process_manager::AggregateGpuProcesses(samples);
    ASSERT_EQ(usage.count(10), 1u);
    EXPECT_EQ(usage.at(10).memoryBytes, 350u);
    EXPECT_DOUBLE_EQ(usage.at(10).utilizationPercent, 40.0);
}

TEST(GpuMonitorTest, MemoryOnlyProcessesKeepUnknownUtilization)
{
    const std::unordered_map<int, process_manager::GpuProcessUsage> usage =
        process_manager::AggregateGpuProcesses(Samples{Memory(0, 7, 64)});
    EXPECT_EQ(usage.at(7).memoryBytes, 64u);
    EXPECT_LT(usage.at(7).utilizationPercent, 0.0);
}

TEST(GpuMonitorTest, IgnoresInvalidPids)
{
    EXPECT_TRUE(process_manager::AggregateGpuProcesses(Samples{Memory(0, 0, 64), Memory(0, -3, 1)}).empty());
}

TEST(GpuMonitorTest, AnUnopenedMonitorReportsNoGpu)
{
    process_manager::GpuMonitor monitor{};
    EXPECT_FALSE(monitor.Available());
    const process_manager::GpuSnapshot snapshot = monitor.Sample();
    EXPECT_FALSE(snapshot.available);
    EXPECT_TRUE(snapshot.devices.empty());
}

TEST(GpuMonitorTest, OpenEitherWorksOrExplainsWhy)
{
    process_manager::GpuMonitor monitor{};
    std::string error{};
    const process_manager::GpuCode code = monitor.Open(error);
    if (code == process_manager::GpuCode::Ok)
    {
        EXPECT_TRUE(monitor.Available());
        EXPECT_TRUE(monitor.Sample().available);
    }
    else
    {
        EXPECT_FALSE(monitor.Available());
        EXPECT_FALSE(error.empty());
    }
}
