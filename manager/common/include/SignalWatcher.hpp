#pragma once

namespace process_manager
{

// Signal handlers may only set flags; the daemon loop reads them.
class SignalWatcher
{
public:
    /**
     * @brief Turn SIGTERM and SIGINT into a stop request and SIGHUP into a reload request,
     *        and ignore SIGPIPE. SIGCHLD goes back to its default and these signals are
     *        unblocked, whatever the parent left. Call it before any thread starts, since
     *        threads inherit the signal mask. On Windows, Ctrl+C, Ctrl+Break, closing the
     *        console and shutdown request a stop.
     */
    static void Install();

    /**
     * @brief Consume a pending stop request.
     * @return true when a stop was requested since the last call.
     */
    static bool TakeStopRequest();

    /**
     * @brief Consume a pending reload request.
     * @return true when a reload was requested since the last call.
     */
    static bool TakeReloadRequest();
};

} // namespace process_manager
