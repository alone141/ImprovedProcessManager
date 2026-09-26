#include "ProcessTable.hpp"
#include "ProcFs.hpp"

#include <cstdint>
#include <unistd.h>
#include <vector>

namespace process_manager
{

ProcessTable ProcessTable::Capture()
{
    ProcessTable table{};
    const ProcFs procFs{"/proc"};
    const long ticks = sysconf(_SC_CLK_TCK);
    const long page = sysconf(_SC_PAGESIZE);
    const std::uint64_t ticksPerSecond = ticks > 0 ? static_cast<std::uint64_t>(ticks) : 100;
    const std::uint64_t pageSize = page > 0 ? static_cast<std::uint64_t>(page) : 4096;

    for (const int pid : procFs.ListPids())
    {
        ProcessStat stat{};
        if (!procFs.ReadProcessStat(pid, stat))
        {
            continue;
        }

        ProcessEntry entry{};
        entry.pid = stat.pid;
        entry.parentPid = stat.parentPid;
        entry.processGroup = stat.processGroup;
        entry.session = stat.session;
        entry.cpuTimeUsec = (stat.userTicks + stat.systemTicks) * 1'000'000 / ticksPerSecond;
        entry.childCpuTimeUsec = (stat.childUserTicks + stat.childSystemTicks) * 1'000'000 / ticksPerSecond;
        entry.rssBytes = stat.rssPages * pageSize;
        entry.threads = stat.threads;
        entry.zombie = stat.state == 'Z' || stat.state == 'X';
        table.Add(entry);
    }
    return table;
}

} // namespace process_manager
