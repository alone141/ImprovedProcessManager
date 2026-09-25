#include "SystemdNotifier.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace process_manager
{

bool SystemdNotifier::Notify(std::string_view state)
{
    const char* socketPath = std::getenv("NOTIFY_SOCKET");
    if (socketPath == nullptr || socketPath[0] == '\0')
    {
        return false;
    }

    const std::string path{socketPath};
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path))
    {
        return false;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    // A leading '@' names a socket in the abstract namespace.
    if (path[0] == '@')
    {
        address.sun_path[0] = '\0';
    }
    const socklen_t length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size());

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return false;
    }
    const ssize_t sent = ::sendto(fd, state.data(), state.size(), MSG_NOSIGNAL,
                                  reinterpret_cast<const sockaddr*>(&address), length);
    ::close(fd);
    return sent == static_cast<ssize_t>(state.size());
}

} // namespace process_manager
