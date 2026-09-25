#include "SignalWatcher.hpp"

#include <atomic>
#include <csignal>
#include <initializer_list>

namespace process_manager
{

namespace
{

std::atomic<bool> stopRequested{false};
std::atomic<bool> reloadRequested{false};

void HandleSignal(int number)
{
    if (number == SIGHUP)
    {
        reloadRequested.store(true);
    }
    else
    {
        stopRequested.store(true);
    }
}

} // namespace

void SignalWatcher::Install()
{
    struct sigaction action{};
    action.sa_handler = HandleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    // A parent may leave SIGCHLD ignored, which makes the kernel reap services before the
    // manager reads their exit status, or leave the signals above blocked.
    std::signal(SIGCHLD, SIG_DFL);
    sigset_t wanted{};
    sigemptyset(&wanted);
    for (const int number : {SIGTERM, SIGINT, SIGHUP, SIGCHLD})
    {
        sigaddset(&wanted, number);
    }
    sigprocmask(SIG_UNBLOCK, &wanted, nullptr);
}

bool SignalWatcher::TakeStopRequest()
{
    return stopRequested.exchange(false);
}

bool SignalWatcher::TakeReloadRequest()
{
    return reloadRequested.exchange(false);
}

} // namespace process_manager
