#include <gtest/gtest.h>

#include "SystemMonitor.hpp"

#include <chrono>
#include <thread>

TEST(SystemMonitorTest, ReadsTheHost)
{
    process_manager::SystemMonitor monitor{};
    const process_manager::SystemSnapshot first = monitor.Sample();
    EXPECT_LT(first.record.cpuPercent, 0.0);
    EXPECT_GE(first.record.cpuCount, 1u);
    EXPECT_GT(first.record.memoryTotalBytes, 0u);
    EXPECT_LE(first.record.memoryAvailableBytes, first.record.memoryTotalBytes);
    EXPECT_FALSE(first.hostName.empty());
}

TEST(SystemMonitorTest, SecondReadingHasCpuUse)
{
    process_manager::SystemMonitor monitor{};
    monitor.Sample();
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    const process_manager::SystemSnapshot second = monitor.Sample();
    EXPECT_GE(second.record.cpuPercent, 0.0);
    EXPECT_LE(second.record.cpuPercent, 100.0);
}
