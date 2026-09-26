#include "CommandServer.hpp"
#include "CommandMessage.hpp"
#include "ZmqSocket.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace process_manager
{

RequestCode ParseCommandRequest(const Message& message, CommandRequest& out, std::string& problem)
{
    if (message.empty())
    {
        return RequestCode::Empty;
    }

    out = CommandRequest{};
    out.identity = message[0];
    std::size_t index = 1;
    while (index < message.size() && message[index].empty())
    {
        out.delimiter = true;
        ++index;
    }
    if (index < message.size() && FrameIs(message[index], command_tag))
    {
        ++index;
    }
    if (index + 1 != message.size())
    {
        problem = "expected frame \"BPM\" and one command frame, got " + std::to_string(message.size() - 1) +
                  " frames";
        return RequestCode::Malformed;
    }
    if (DecodeCommand(message[index], out.command) != DecodeCode::Ok)
    {
        problem = "the command frame has " + std::to_string(message[index].size()) + " bytes, expected " +
                  std::to_string(command_message_size);
        return RequestCode::Malformed;
    }
    return RequestCode::Ok;
}

Message BuildCommandReply(const CommandRequest& request, const CommandReply& reply)
{
    Message message{request.identity};
    if (request.delimiter)
    {
        message.push_back(Frame{});
    }
    message.push_back(MakeFrame(command_tag));
    message.push_back(EncodeReply(reply));
    return message;
}

CommandServer::CommandServer(ZmqContext& context)
    : router{context, SocketType::Router}
{
}

ZmqCode CommandServer::Bind(const std::string& endpoint, std::string& error)
{
    router.SetOption(SocketOption::RouterHandover, 1);
    if (endpoint.find('[') != std::string::npos)
    {
        router.SetOption(SocketOption::Ipv6, 1);
    }
    if (router.Bind(endpoint) != ZmqCode::Ok)
    {
        error = "cannot bind " + endpoint + ": " + router.LastError();
        return ZmqCode::Failed;
    }
    return ZmqCode::Ok;
}

RequestCode CommandServer::Receive(CommandRequest& out, std::string& problem)
{
    Message message{};
    if (router.Receive(message, true) != ZmqCode::Ok)
    {
        return RequestCode::Empty;
    }
    return ParseCommandRequest(message, out, problem);
}

ZmqCode CommandServer::Reply(const CommandRequest& request, const CommandReply& reply)
{
    return router.Send(BuildCommandReply(request, reply), true);
}

ZmqSocket& CommandServer::Socket()
{
    return router;
}

std::string CommandServer::Endpoint() const
{
    return router.BoundEndpoint();
}

std::string DescribeIdentity(const Frame& identity)
{
    bool printable = !identity.empty();
    for (const std::uint8_t byte : identity)
    {
        printable = printable && byte >= 0x20 && byte < 0x7f;
    }
    if (printable)
    {
        return std::string{identity.begin(), identity.end()};
    }

    std::string text{"0x"};
    for (const std::uint8_t byte : identity)
    {
        char digits[3]{};
        std::snprintf(digits, sizeof(digits), "%02x", byte);
        text += digits;
    }
    return text;
}

} // namespace process_manager
