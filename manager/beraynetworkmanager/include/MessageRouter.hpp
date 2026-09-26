#pragma once

#include "ZmqSocket.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace process_manager
{

// Not the manager's own command port (5557), so a router and a manager share a
// host without a change to either default.
constexpr std::string_view default_router_endpoint = "tcp://*:5558";

// The router: one ROUTER socket that forwards each message to the peer named in
// its first frame (see Envelope.hpp). Peers connect DEALER sockets under unique
// identities. The payload is never parsed; what cannot be delivered is dropped
// and logged, and nothing is queued for a peer that is away.
class MessageRouter
{
public:
    /**
     * @brief Create the ROUTER socket and the socket that watches its connections.
     * @param context Context to create them in. It must outlive the router.
     */
    explicit MessageRouter(ZmqContext& context);

    /**
     * @brief Bind the socket. A send to an unknown identity fails at once instead of queueing,
     *        and a peer that reconnects under its identity replaces its old connection.
     * @param endpoint For example tcp://0.0.0.0:5558.
     * @param error Receives the reason on failure.
     * @return ZmqCode::Ok when bound.
     */
    ZmqCode Bind(const std::string& endpoint, std::string& error);

    /**
     * @brief Forward the messages that arrive within @p timeout and log connection changes.
     * @param timeout Longest wait for the first message.
     * @return How many messages were forwarded.
     */
    int Step(std::chrono::milliseconds timeout);

    /**
     * @brief Where the socket is bound.
     * @return The endpoint with its actual port.
     */
    std::string Endpoint() const;

private:
    bool Forward(const Message& frames);
    void ReadEvents();

    ZmqSocket router;
    ZmqSocket monitor;
    std::set<std::string> known;                // identities that have sent something
    std::map<std::uint32_t, std::string> peers; // connection descriptor -> peer address
};

} // namespace process_manager
