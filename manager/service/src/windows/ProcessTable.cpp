#include "ProcessTable.hpp"

// windows.h must come before the other Win32 headers, which rely on its types.
#include <windows.h>

#include <tlhelp32.h>

namespace process_manager
{

ProcessTable ProcessTable::Capture()
{
    ProcessTable table{};
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return table;
    }

    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (Process32FirstW(snapshot, &process))
    {
        do
        {
            ProcessEntry entry{};
            entry.pid = static_cast<int>(process.th32ProcessID);
            entry.parentPid = static_cast<int>(process.th32ParentProcessID);
            entry.threads = static_cast<int>(process.cntThreads);
            table.Add(entry);
        } while (Process32NextW(snapshot, &process));
    }
    CloseHandle(snapshot);
    return table;
}

} // namespace process_manager
