#include <gtest/gtest.h>

#include "Instant.hpp"

#include <chrono>
#include <cstdint>

TEST(InstantTest, NowIsAfterTheYearTwoThousand)
{
    const process_manager::Instant now = process_manager::Now();
    EXPECT_GT(now.wallNs, 946684800000000000);
}

TEST(InstantTest, AdvanceMovesBothClocks)
{
    const process_manager::Instant start{std::chrono::steady_clock::time_point{} + std::chrono::hours{1}, 1000};
    const process_manager::Instant later = process_manager::Advance(start, std::chrono::milliseconds{1500});
    EXPECT_EQ(later.wallNs, 1000 + 1'500'000'000);
    EXPECT_EQ(later.steady - start.steady, std::chrono::milliseconds{1500});
    const process_manager::Instant earlier = process_manager::Advance(start, std::chrono::milliseconds{-1});
    EXPECT_EQ(earlier.wallNs, 1000 - 1'000'000);
}

TEST(InstantTest, WallTimeOfFollowsTheSteadyOffset)
{
    const process_manager::Instant reference{std::chrono::steady_clock::time_point{} + std::chrono::hours{2}, 5'000'000'000};
    EXPECT_EQ(process_manager::WallTimeOf(reference, reference.steady + std::chrono::seconds{2}), 7'000'000'000);
    EXPECT_EQ(process_manager::WallTimeOf(reference, reference.steady - std::chrono::seconds{1}), 4'000'000'000);
}
