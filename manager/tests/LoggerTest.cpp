#include <gtest/gtest.h>

#include "Logger.hpp"

#include <optional>
#include <string>

TEST(LoggerTest, JournalLinesCarryThePriority)
{
    const process_manager::Logger logger{process_manager::LogLevel::Debug, true};
    EXPECT_EQ(logger.Format(process_manager::LogLevel::Error, "boom", 0), "<3>boom\n");
    EXPECT_EQ(logger.Format(process_manager::LogLevel::Warning, "hmm", 0), "<4>hmm\n");
    EXPECT_EQ(logger.Format(process_manager::LogLevel::Info, "ok", 0), "<6>ok\n");
    EXPECT_EQ(logger.Format(process_manager::LogLevel::Debug, "x", 0), "<7>x\n");
}

TEST(LoggerTest, TerminalLinesCarryATimestampAndLevel)
{
    const process_manager::Logger logger{process_manager::LogLevel::Info, false};
    const std::string line = logger.Format(process_manager::LogLevel::Warning, "careful", 1'700'000'000'123'000'000);
    EXPECT_NE(line.find(".123 [warning] careful\n"), std::string::npos) << line;
    EXPECT_EQ(line.size(), std::string{"2023-11-14 22:13:20.123 [warning] careful\n"}.size());
}

TEST(LoggerTest, FiltersByLevel)
{
    process_manager::Logger logger{process_manager::LogLevel::Warning, false};
    EXPECT_TRUE(logger.Enabled(process_manager::LogLevel::Error));
    EXPECT_TRUE(logger.Enabled(process_manager::LogLevel::Warning));
    EXPECT_FALSE(logger.Enabled(process_manager::LogLevel::Info));
    logger.SetLevel(process_manager::LogLevel::Debug);
    EXPECT_TRUE(logger.Enabled(process_manager::LogLevel::Debug));
}

TEST(LoggerTest, ParsesLevelNames)
{
    EXPECT_EQ(process_manager::ParseLogLevel("debug"), process_manager::LogLevel::Debug);
    EXPECT_EQ(process_manager::ParseLogLevel("warning"), process_manager::LogLevel::Warning);
    EXPECT_EQ(process_manager::ParseLogLevel("verbose"), std::nullopt);
}
