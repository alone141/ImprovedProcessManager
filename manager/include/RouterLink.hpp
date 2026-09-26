#pragma once

#include "CommandServer.hpp"
#include "ZmqSocket.hpp"

#include <string>

namespace process_manager
{

// The manager's side of a router (beraynetworkmanager): a DEALER connected under
// the manager's identity. A peer that sends [identity]["BPM"][command] to the
// router reaches it as [peer]["BPM"][command], and the reply goes back the same
// way: [peer]["BPM"][reply]. The direct ROUTER socket keeps serving alongside.
class RouterLink : public CommandSource
{
public:
    /**
     * @brief Create the DEALER socket.
     * @param context Context to create it in. It must outlive the link.
     */
    explicit RouterLink(ZmqContext& context);

    /**
     * @brief Connect to the router under an identity. The connection completes in the
     *        background and is retried while the router is away.
     * @param endpoint The router's endpoint, for example tcp://127.0.0.1:5558.
     * @param identity The manager's name on the router, for example berayprocessmanager.
     * @param error Receives the reason on failure.
     * @return ZmqCode::Ok when connecting.
     */
    ZmqCode Connect(const std::string& endpoint, const std::string& identity, std::string& error);

    /**
     * @brief Take one request without waiting: [peer] ["BPM"] command.
     * @param out Receives the request; its identity is the peer that sent it.
     * @param problem Receives what is wrong with a malformed request.
     * @return RequestCode::Ok, RequestCode::Empty when nothing is queued, or RequestCode::Malformed.
     */
    RequestCode Receive(CommandRequest& out, std::string& problem) override;

    /**
     * @brief Answer a request through the router: [peer] "BPM" reply.
     * @param request Request being answered.
     * @param reply Reply to send.
     * @return ZmqCode::Ok when queued.
     */
    ZmqCode Reply(const CommandRequest& request, const CommandReply& reply) override;

    /**
     * @brief The DEALER socket, for polling.
     * @return The socket.
     */
    ZmqSocket& Socket() override;

    /**
     * @brief The router's endpoint.
     * @return The endpoint given to Connect.
     */
    std::string Endpoint() const;

    /**
     * @brief The manager's name on the router.
     * @return The identity given to Connect.
     */
    std::string Identity() const;

private:
    ZmqSocket dealer;
    std::string endpoint;
    std::string identity;
};

} // namespace process_manager
