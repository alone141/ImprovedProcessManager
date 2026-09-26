#include "PeerAddress.hpp"

#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <string>

namespace process_manager
{

std::string PeerAddress(std::uint32_t descriptor)
{
    sockaddr_storage storage{};
    int length = sizeof(storage);
    if (::getpeername(static_cast<SOCKET>(descriptor), reinterpret_cast<sockaddr*>(&storage), &length) != 0)
    {
        return std::string{};
    }

    char text[INET6_ADDRSTRLEN]{};
    if (storage.ss_family == AF_INET)
    {
        const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) == nullptr)
        {
            return std::string{};
        }
        return std::string{text} + ":" + std::to_string(ntohs(address->sin_port));
    }
    if (storage.ss_family == AF_INET6)
    {
        const sockaddr_in6* address = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &address->sin6_addr, text, sizeof(text)) == nullptr)
        {
            return std::string{};
        }
        return "[" + std::string{text} + "]:" + std::to_string(ntohs(address->sin6_port));
    }
    return std::string{};
}

} // namespace process_manager
