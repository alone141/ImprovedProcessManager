#include "Instant.hpp"

#include <chrono>
#include <cstdint>

namespace process_manager
{

Instant Now()
{
    const std::chrono::system_clock::time_point wall = std::chrono::system_clock::now();
    const std::chrono::nanoseconds sinceEpoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall.time_since_epoch());
    return Instant{std::chrono::steady_clock::now(), sinceEpoch.count()};
}

Instant Advance(const Instant& from, std::chrono::milliseconds delta)
{
    const std::chrono::nanoseconds shift{delta};
    return Instant{from.steady + delta, from.wallNs + shift.count()};
}

std::int64_t WallTimeOf(const Instant& reference, std::chrono::steady_clock::time_point at)
{
    const std::chrono::nanoseconds offset =
        std::chrono::duration_cast<std::chrono::nanoseconds>(at - reference.steady);
    return reference.wallNs + offset.count();
}

} // namespace process_manager
