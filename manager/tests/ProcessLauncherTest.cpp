#include <gtest/gtest.h>

#include "ProcessLauncher.hpp"

#include <cstdint>

TEST(ProcessLauncherTest, OnlyExitZeroWithoutSignalIsClean)
{
    EXPECT_TRUE(process_manager::ExitedCleanly(process_manager::ExitStatus{0, 0}));
    EXPECT_FALSE(process_manager::ExitedCleanly(process_manager::ExitStatus{1, 0}));
    EXPECT_FALSE(process_manager::ExitedCleanly(process_manager::ExitStatus{0, 15}));
}

TEST(ProcessLauncherTest, SignalsAreNegativeOnTheWire)
{
    EXPECT_EQ(process_manager::WireExitCode(process_manager::ExitStatus{3, 0}), 3);
    EXPECT_EQ(process_manager::WireExitCode(process_manager::ExitStatus{0, 9}), -9);
}

TEST(ProcessLauncherTest, DescribesReportedExitsOfAPosixManager)
{
    EXPECT_EQ(process_manager::DescribeExit(0, true), "exit 0");
    EXPECT_EQ(process_manager::DescribeExit(3, true), "exit 3");
    EXPECT_EQ(process_manager::DescribeExit(-9, true), "signal 9 (KILL)");
    EXPECT_EQ(process_manager::DescribeExit(-40, true), "signal 40");
    EXPECT_EQ(process_manager::DescribeExit(static_cast<std::int32_t>(0xC0000005u), true), "exit 0xC0000005");
}

TEST(ProcessLauncherTest, WindowsExitCodesAreNeverSignals)
{
    EXPECT_EQ(process_manager::DescribeExit(-1, false), "exit 0xFFFFFFFF");
    EXPECT_EQ(process_manager::DescribeExit(7, false), "exit 7");
}

TEST(ProcessLauncherTest, DescribesLocalExitStatuses)
{
    EXPECT_EQ(process_manager::DescribeExitStatus(process_manager::ExitStatus{0, 15}), "signal 15 (TERM)");
    EXPECT_EQ(process_manager::DescribeExitStatus(process_manager::ExitStatus{2, 0}), "exit 2");
    EXPECT_EQ(process_manager::DescribeExitStatus(process_manager::ExitStatus{-1, 0}), "exit 0xFFFFFFFF");
}
