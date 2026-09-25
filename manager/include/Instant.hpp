#pragma once

#include <chrono>
#include <cstdint>

namespace process_manager
{

// One moment on both clocks: steady for timers, wall for reports.
struct Instant
{
    std::chrono::steady_clock::time_point steady;
    std::int64_t wallNs;
};

/**
 * @brief Read both clocks.
 * @return The steady time and the wall time in nanoseconds since the Unix epoch.
 */
Instant Now();

/**
 * @brief Move an instant along both clocks.
 * @param from Starting instant.
 * @param delta Time to add. A negative delta moves back.
 * @return The shifted instant.
 */
Instant Advance(const Instant& from, std::chrono::milliseconds delta);

/**
 * @brief Convert a steady time point to wall time.
 * @param reference Instant whose two readings belong together.
 * @param at Steady time point to convert.
 * @return Wall time of @p at in nanoseconds since the Unix epoch.
 */
std::int64_t WallTimeOf(const Instant& reference, std::chrono::steady_clock::time_point at);

} // namespace process_manager
