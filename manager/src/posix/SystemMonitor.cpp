#include "SystemMonitor.hpp"
#include "ProcFs.hpp"

#include <cstdint>
#include <string>
#include <unistd.h>

namespace process_manager
{

SystemMonitor::SystemMonitor()
    : previousBusy{0}, previousTotal{0}, hasPrevious{false}
{
}

SystemSnapshot SystemMonitor::Sample()
{
    const ProcFs procFs{"/proc"};
    SystemSnapshot snapshot{};
    CpuTimes times{};
    if (procFs.ReadSystemCpu(times))
    {
        if (hasPrevious && times.total > previousTotal && times.busy >= previousBusy)
        {
            snapshot.record.cpuPercent = 100.0 * static_cast<double>(times.busy - previousBusy) /
                                         static_cast<double>(times.total - previousTotal);
        }
        previousBusy = times.busy;
        previousTotal = times.total;
        hasPrevious = true;
    }

    MemoryInfo memory{};
    if (procFs.ReadMemory(memory))
    {
        snapshot.record.memoryTotalBytes = memory.totalBytes;
        snapshot.record.memoryAvailableBytes = memory.availableBytes;
    }
    LoadAverage load{};
    if (procFs.ReadLoadAverage(load))
    {
        snapshot.record.loadAverage1 = load.one;
        snapshot.record.loadAverage5 = load.five;
        snapshot.record.loadAverage15 = load.fifteen;
    }
    std::uint64_t uptime = 0;
    if (procFs.ReadUptime(uptime))
    {
        snapshot.record.uptimeSeconds = uptime;
    }

    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    snapshot.record.cpuCount = cpus > 0 ? static_cast<std::uint32_t>(cpus) : 1;
    char host[256]{};
    if (gethostname(host, sizeof(host) - 1) == 0)
    {
        snapshot.hostName = std::string{host};
    }
    return snapshot;
}

} // namespace process_manager
