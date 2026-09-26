#include "SystemMonitor.hpp"

// windows.h must come before the other Win32 headers, which rely on its types.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace process_manager
{

namespace
{

std::uint64_t Ticks(const FILETIME& time)
{
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::string HostName()
{
    wchar_t name[256]{};
    DWORD size = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, name, &size))
    {
        return std::string{};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
    std::string text{};
    text.resize(static_cast<std::size_t>(length));
    WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), text.data(), length, nullptr, nullptr);
    return text;
}

} // namespace

SystemMonitor::SystemMonitor()
    : previousBusy{0}, previousTotal{0}, hasPrevious{false}
{
}

SystemSnapshot SystemMonitor::Sample()
{
    SystemSnapshot snapshot{};
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user))
    {
        // Kernel time includes idle time.
        const std::uint64_t total = Ticks(kernel) + Ticks(user);
        const std::uint64_t busy = total - Ticks(idle);
        if (hasPrevious && total > previousTotal && busy >= previousBusy)
        {
            snapshot.record.cpuPercent =
                100.0 * static_cast<double>(busy - previousBusy) / static_cast<double>(total - previousTotal);
        }
        previousBusy = busy;
        previousTotal = total;
        hasPrevious = true;
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        snapshot.record.memoryTotalBytes = memory.ullTotalPhys;
        snapshot.record.memoryAvailableBytes = memory.ullAvailPhys;
    }
    snapshot.record.uptimeSeconds = GetTickCount64() / 1000;
    const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    snapshot.record.cpuCount = processors > 0 ? processors : 1;
    snapshot.hostName = HostName();
    return snapshot;
}

} // namespace process_manager
