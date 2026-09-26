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

/**
 * @brief Split a received message into a request: [identity] ["" ...] ["BPM"] command.
 * @param message Frames as a ROUTER receives them, or as a DEALER receives them from a router.
 * @param out Receives the request. On RequestCode::Malformed only the identity is set, so
 *        the problem can still be answered.
 * @param problem Receives what is wrong with a malformed message.
 * @return RequestCode::Ok, RequestCode::Empty for an empty message, or RequestCode::Malformed.
 */
RequestCode ParseCommandRequest(const Message& message, CommandRequest& out, std::string& problem);

/**
 * @brief Build the answer to a request: [identity] [""] "BPM" reply.
 * @param request Request being answered; its delimiter is echoed.
 * @param reply Reply to encode.
 * @return The frames to send.
 */
Message BuildCommandReply(const CommandRequest& request, const CommandReply& reply);

// Where commands come from: the manager's own ROUTER socket, or its link to a
// router that other peers reach it through. The daemon serves both the same way.
class CommandSource
{
public:
    virtual ~CommandSource() = default;

    /**
     * @brief Take one request without waiting.
     * @param out Receives the request; see ParseCommandRequest.
     * @param problem Receives what is wrong with a malformed request.
     * @return RequestCode::Ok, RequestCode::Empty when nothing is queued, or RequestCode::Malformed.
     */
    virtual RequestCode Receive(CommandRequest& out, std::string& problem) = 0;

    /**
     * @brief Answer a request.
     * @param request Request being answered.
     * @param reply Reply to send.
     * @return ZmqCode::Ok when queued.
     */
    virtual ZmqCode Reply(const CommandRequest& request, const CommandReply& reply) = 0;

    /**
     * @brief The socket to poll for requests.
     * @return The socket.
     */
    virtual ZmqSocket& Socket() = 0;
};

class CommandServer : public CommandSource
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
    RequestCode Receive(CommandRequest& out, std::string& problem) override;

    /**
     * @brief Answer a request: [identity] [""] "BPM" reply. Old GUIs never read replies;
     *        libzmq drops what they leave unread once their queue is full.
     * @param request Request being answered.
     * @param reply Reply to send.
     * @return ZmqCode::Ok when queued.
     */
    ZmqCode Reply(const CommandRequest& request, const CommandReply& reply) override;

    /**
     * @brief The ROUTER socket, for polling.
     * @return The socket.
     */
    ZmqSocket& Socket() override;

    /**
     * @brief Where the socket is bound.
     * @return The endpoint with its actual port.
     */
    std::string Endpoint() const;

private:
    ZmqSocket router;
};

} // namespace process_manager
