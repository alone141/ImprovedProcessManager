#pragma once

#include "DetailedReport.hpp"

#include <cstdint>
#include <string>

namespace process_manager
{

struct SystemSnapshot
{
    SystemRecord record;
    std::string hostName;
};

class SystemMonitor
{
public:
    /**
     * @brief Create a monitor with no CPU reading yet.
     */
    SystemMonitor();

    /**
     * @brief Read host CPU use, memory, load and uptime.
     * @return The figures. CPU use needs two readings, so the first one reports it as unknown.
     */
    SystemSnapshot Sample();

private:
    std::uint64_t previousBusy;
    std::uint64_t previousTotal;
    bool hasPrevious;
};

} // namespace process_manager
