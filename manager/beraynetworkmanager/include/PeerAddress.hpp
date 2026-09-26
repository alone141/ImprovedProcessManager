#pragma once

#include <cstdint>
#include <string>

namespace process_manager
{

/**
 * @brief The address of the peer behind a connected socket descriptor.
 * @param descriptor The descriptor a socket event reported.
 * @return For example 127.0.0.1:51234 or [::1]:51234; empty when it cannot be read.
 */
std::string PeerAddress(std::uint32_t descriptor);

} // namespace process_manager
