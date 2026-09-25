#pragma once

#include <string_view>

namespace process_manager
{

// The sd_notify protocol, without libsystemd: one datagram to $NOTIFY_SOCKET.
class SystemdNotifier
{
public:
    /**
     * @brief Tell systemd about a state change when it runs this process as Type=notify.
     * @param state For example "READY=1", "RELOADING=1" or "STOPPING=1".
     * @return true when NOTIFY_SOCKET is set and the message was sent; always false on Windows.
     */
    static bool Notify(std::string_view state);
};

} // namespace process_manager
