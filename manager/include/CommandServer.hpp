#pragma once

#include "CommandMessage.hpp"
#include "ZmqSocket.hpp"

#include <string>

namespace process_manager
{

struct CommandRequest
{
    Frame identity;
    bool delimiter{false}; // the client sent an empty delimiter frame, as REQ sockets do
    CommandMessage command;
};

enum class RequestCode
{
    Ok,
    Empty,
    Malformed,
};

class CommandServer
{
public:
    /**
     * @brief Create the ROUTER socket.
     * @param context Context to create it in. It must outlive the server.
     */
    explicit CommandServer(ZmqContext& context);

    /**
     * @brief Bind the socket. Any client identity is accepted; with ROUTER_HANDOVER a GUI
     *        that reconnects with its fixed identity "PMC" replaces its stale connection.
     * @param endpoint For example tcp://0.0.0.0:5557.
     * @param error Receives the reason on failure.
     * @return ZmqCode::Ok when bound.
     */
    ZmqCode Bind(const std::string& endpoint, std::string& error);

    /**
     * @brief Take one request without waiting: [identity] ["" ...] ["BPM"] command.
     * @param out Receives the request. On RequestCode::Malformed only the identity is set,
     *        so the problem can still be answered.
     * @param problem Receives what is wrong with a malformed request.
     * @return RequestCode::Ok, RequestCode::Empty when nothing is queued, or RequestCode::Malformed.
     */
    RequestCode Receive(CommandRequest& out, std::string& problem);

    /**
     * @brief Answer a request: [identity] [""] "BPM" reply. Old GUIs never read replies;
     *        libzmq drops what they leave unread once their queue is full.
     * @param request Request being answered.
     * @param reply Reply to send.
     * @return ZmqCode::Ok when queued.
     */
    ZmqCode Reply(const CommandRequest& request, const CommandReply& reply);

    /**
     * @brief The ROUTER socket, for polling.
     * @return The socket.
     */
    ZmqSocket& Socket();

    /**
     * @brief Where the socket is bound.
     * @return The endpoint with its actual port.
     */
    std::string Endpoint() const;

private:
    ZmqSocket router;
};

/**
 * @brief Printable form of a client identity: text when printable, hexadecimal otherwise.
 * @param identity Routing identity frame.
 * @return For example "PMC" or "0x006b8b4567".
 */
std::string DescribeIdentity(const Frame& identity);

} // namespace process_manager
