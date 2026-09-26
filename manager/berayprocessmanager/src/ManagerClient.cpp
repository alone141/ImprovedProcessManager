#include "ManagerClient.hpp"
#include "CommandMessage.hpp"
#include "DetailedReport.hpp"
#include "ZmqSocket.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace process_manager
{

namespace
{

constexpr int subscriber_queue = 16;

// libzmq resolves names as IPv4 only unless the socket enables IPv6.
void AllowIpv6(ZmqSocket& socket, const std::string& endpoint)
{
    if (endpoint.find('[') != std::string::npos)
    {
        socket.SetOption(SocketOption::Ipv6, 1);
    }
}

std::chrono::milliseconds Remaining(std::chrono::steady_clock::time_point deadline)
{
    const std::chrono::steady_clock::duration left = deadline - std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(left);
}

} // namespace

ManagerClient::ManagerClient(std::string commandEndpoint, std::string reportEndpoint)
    : context{}, commandEndpoint{std::move(commandEndpoint)}, reportEndpoint{std::move(reportEndpoint)},
      routerEndpoint{}, managerIdentity{}, subscriber{}
{
}

void ManagerClient::UseRouter(std::string endpoint, std::string identity)
{
    routerEndpoint = std::move(endpoint);
    managerIdentity = std::move(identity);
}

ClientCode ManagerClient::SendCommand(CommandCode command, const std::string& service,
                                      std::chrono::milliseconds timeout, CommandReply& out, std::string& error)
{
    const bool viaRouter = !routerEndpoint.empty();
    const std::string& target = viaRouter ? routerEndpoint : commandEndpoint;
    ZmqSocket dealer{context, SocketType::Dealer};
    AllowIpv6(dealer, target);
    if (dealer.Connect(target) != ZmqCode::Ok)
    {
        error = "cannot connect to " + target + ": " + dealer.LastError();
        return ClientCode::Failed;
    }

    CommandMessage message{};
    message.command = static_cast<std::uint8_t>(command);
    message.serviceName = service;
    Message request{};
    if (viaRouter)
    {
        request.push_back(MakeFrame(managerIdentity));
    }
    request.push_back(MakeFrame(command_tag));
    request.push_back(EncodeCommand(message));
    if (dealer.Send(request, true) != ZmqCode::Ok)
    {
        error = "cannot send to " + target + ": " + dealer.LastError();
        return ClientCode::Failed;
    }

    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        const std::chrono::milliseconds left = Remaining(deadline);
        if (left.count() <= 0)
        {
            error = viaRouter ? "no reply from " + managerIdentity + " through the router at " + routerEndpoint +
                                    " within " + std::to_string(timeout.count()) +
                                    " ms; is the manager connected to the router under that identity?"
                              : "no reply from the manager at " + commandEndpoint + " within " +
                                    std::to_string(timeout.count()) + " ms; is it running?";
            return ClientCode::Timeout;
        }

        const std::array<ZmqSocket*, 1> sockets{&dealer};
        std::vector<bool> ready{};
        if (PollReadable(sockets, left, ready) != ZmqCode::Ok)
        {
            error = "waiting for the reply failed: " + dealer.LastError();
            return ClientCode::Failed;
        }
        if (!ready[0])
        {
            continue;
        }

        Message reply{};
        if (dealer.Receive(reply, true) != ZmqCode::Ok)
        {
            continue;
        }
        std::size_t index = 0;
        while (index < reply.size() && reply[index].empty())
        {
            ++index;
        }
        // Through a router the reply names its sender first; anyone else's message is not the answer.
        if (viaRouter)
        {
            if (index >= reply.size() || !FrameIs(reply[index], managerIdentity))
            {
                continue;
            }
            ++index;
            while (index < reply.size() && reply[index].empty())
            {
                ++index;
            }
        }
        if (index + 2 != reply.size() || !FrameIs(reply[index], command_tag) ||
            DecodeReply(reply[index + 1], out) != DecodeCode::Ok)
        {
            error = "the manager sent a reply this client does not understand";
            return ClientCode::Malformed;
        }
        return ClientCode::Ok;
    }
}

ClientCode ManagerClient::WaitForReport(std::chrono::milliseconds timeout, DetailedReport& out, std::string& error)
{
    if (subscriber == nullptr)
    {
        std::unique_ptr<ZmqSocket> socket = std::make_unique<ZmqSocket>(context, SocketType::Subscriber);
        socket->SetOption(SocketOption::ReceiveHighWater, subscriber_queue);
        socket->SetOption(SocketOption::Subscribe, report_topic);
        AllowIpv6(*socket, reportEndpoint);
        if (socket->Connect(reportEndpoint) != ZmqCode::Ok)
        {
            error = "cannot connect to " + reportEndpoint + ": " + socket->LastError();
            return ClientCode::Failed;
        }
        subscriber = std::move(socket);
    }

    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    while (true)
    {
        const std::chrono::milliseconds left = Remaining(deadline);
        if (left.count() <= 0)
        {
            error = "no report from the manager at " + reportEndpoint + " within " + std::to_string(timeout.count()) +
                    " ms; is it running?";
            return ClientCode::Timeout;
        }

        const std::array<ZmqSocket*, 1> sockets{subscriber.get()};
        std::vector<bool> ready{};
        if (PollReadable(sockets, left, ready) != ZmqCode::Ok)
        {
            error = "waiting for a report failed: " + subscriber->LastError();
            return ClientCode::Failed;
        }
        if (!ready[0])
        {
            continue;
        }

        Message message{};
        if (subscriber->Receive(message, true) != ZmqCode::Ok || message.size() != 2 ||
            !FrameIs(message[0], report_topic))
        {
            continue;
        }
        const DecodeCode decoded = DecodeReport(message[1], out);
        if (decoded != DecodeCode::Ok)
        {
            error = decoded == DecodeCode::UnsupportedVersion
                        ? "the manager sends a newer report version than this client reads"
                        : "the manager sent a report this client does not understand";
            return ClientCode::Malformed;
        }
        return ClientCode::Ok;
    }
}

} // namespace process_manager
