#include "SignalWatcher.hpp"

#include <windows.h>

#include <atomic>

namespace process_manager
{

namespace
{

// Windows ends the process once the handler returns from a close or shutdown event,
// so the handler waits this long to let the daemon stop its services first.
constexpr DWORD close_grace_ms = 4500;

std::atomic<bool> stopRequested{false};

BOOL WINAPI HandleConsoleEvent(DWORD event)
{
    switch (event)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        stopRequested.store(true);
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        stopRequested.store(true);
        Sleep(close_grace_ms);
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

void SignalWatcher::Install()
{
    SetConsoleCtrlHandler(HandleConsoleEvent, TRUE);
}

bool SignalWatcher::TakeStopRequest()
{
    return stopRequested.exchange(false);
}

bool SignalWatcher::TakeReloadRequest()
{
    return false;
}

} // namespace process_manager
