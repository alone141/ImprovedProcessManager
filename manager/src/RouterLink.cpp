#include "RouterLink.hpp"
#include "CommandMessage.hpp"
#include "CommandServer.hpp"
#include "ZmqSocket.hpp"

#include <string>

namespace process_manager
{

RouterLink::RouterLink(ZmqContext& context)
    : dealer{context, SocketType::Dealer}, endpoint{}, identity{}
{
}

ZmqCode RouterLink::Connect(const std::string& routerEndpoint, const std::string& name, std::string& error)
{
    if (dealer.SetOption(SocketOption::Identity, name) != ZmqCode::Ok)
    {
        error = "cannot use '" + name + "' as the identity on the router: " + dealer.LastError();
        return ZmqCode::Failed;
    }
    if (routerEndpoint.find('[') != std::string::npos)
    {
        dealer.SetOption(SocketOption::Ipv6, 1);
    }
    if (dealer.Connect(routerEndpoint) != ZmqCode::Ok)
    {
        error = "cannot connect to the router at " + routerEndpoint + ": " + dealer.LastError();
        return ZmqCode::Failed;
    }
    endpoint = routerEndpoint;
    identity = name;
    return ZmqCode::Ok;
}

RequestCode RouterLink::Receive(CommandRequest& out, std::string& problem)
{
    Message message{};
    if (dealer.Receive(message, true) != ZmqCode::Ok)
    {
        return RequestCode::Empty;
    }
    return ParseCommandRequest(message, out, problem);
}

ZmqCode RouterLink::Reply(const CommandRequest& request, const CommandReply& reply)
{
    return dealer.Send(BuildCommandReply(request, reply), true);
}

ZmqSocket& RouterLink::Socket()
{
    return dealer;
}

std::string RouterLink::Endpoint() const
{
    return endpoint;
}

std::string RouterLink::Identity() const
{
    return identity;
}

} // namespace process_manager
