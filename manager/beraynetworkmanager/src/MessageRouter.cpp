#include "MessageRouter.hpp"
#include "Envelope.hpp"
#include "Logger.hpp"
#include "PeerAddress.hpp"
#include "ZmqSocket.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace process_manager
{

namespace
{

constexpr int max_messages_per_step = 1000;

std::string Describe(const Envelope& envelope)
{
    std::size_t bytes = 0;
    for (const Frame& frame : envelope.payload)
    {
        bytes += frame.size();
    }
    return "source=" + DescribeIdentity(envelope.source) + " destination=" + DescribeIdentity(envelope.destination) +
           " frames=" + std::to_string(envelope.payload.size()) + " bytes=" + std::to_string(bytes);
}

// One monitor endpoint per router, so two routers in one context never share it.
std::string MonitorEndpoint(const void* owner)
{
    char text[64]{};
    std::snprintf(text, sizeof(text), "inproc://router-monitor-%p", owner);
    return std::string{text};
}

} // namespace

MessageRouter::MessageRouter(ZmqContext& context)
    : router{context, SocketType::Router}, monitor{context, SocketType::Pair}, known{}, peers{}
{
}

ZmqCode MessageRouter::Bind(const std::string& endpoint, std::string& error)
{
    router.SetOption(SocketOption::RouterMandatory, 1);
    router.SetOption(SocketOption::RouterHandover, 1);
    if (endpoint.find('[') != std::string::npos)
    {
        router.SetOption(SocketOption::Ipv6, 1);
    }

    const std::string events = MonitorEndpoint(this);
    if (router.MonitorConnections(events) != ZmqCode::Ok)
    {
        error = "cannot watch connections: " + router.LastError();
        return ZmqCode::Failed;
    }
    if (monitor.Connect(events) != ZmqCode::Ok)
    {
        error = "cannot watch connections: " + monitor.LastError();
        return ZmqCode::Failed;
    }
    if (router.Bind(endpoint) != ZmqCode::Ok)
    {
        error = "cannot bind " + endpoint + ": " + router.LastError();
        return ZmqCode::Failed;
    }
    LogInfo("listening on " + router.BoundEndpoint());
    return ZmqCode::Ok;
}

int MessageRouter::Step(std::chrono::milliseconds timeout)
{
    const std::array<ZmqSocket*, 2> sockets{&router, &monitor};
    std::vector<bool> ready{};
    if (PollReadable(sockets, timeout, ready) != ZmqCode::Ok)
    {
        return 0;
    }
    if (ready[1])
    {
        ReadEvents();
    }

    int forwarded = 0;
    if (ready[0])
    {
        for (int i = 0; i < max_messages_per_step; ++i)
        {
            Message frames{};
            if (router.Receive(frames, true) != ZmqCode::Ok)
            {
                break;
            }
            forwarded += Forward(frames) ? 1 : 0;
        }
    }
    return forwarded;
}

std::string MessageRouter::Endpoint() const
{
    return router.BoundEndpoint();
}

bool MessageRouter::Forward(const Message& frames)
{
    Envelope envelope{};
    const EnvelopeCode parsed = ParseEnvelope(frames, envelope);
    if (parsed != EnvelopeCode::Ok)
    {
        LogWarning(std::string{"message rejected reason="} +
                   (parsed == EnvelopeCode::EmptySource ? "empty_source" : "malformed") +
                   " source=" + DescribeIdentity(envelope.source));
        return false;
    }

    const std::string source = DescribeIdentity(envelope.source);
    if (known.insert(source).second)
    {
        LogInfo("dealer registered identity=" + source);
    }

    const std::string summary = Describe(envelope);
    const ZmqCode sent = router.Send(BuildEnvelope(envelope.destination, envelope.source, envelope.payload), true);
    if (sent == ZmqCode::Ok)
    {
        LogDebug("message forwarded " + summary);
        return true;
    }
    if (sent == ZmqCode::Unreachable)
    {
        LogWarning("message undeliverable " + summary + " reason=destination_unavailable");
        const std::string destination = DescribeIdentity(envelope.destination);
        if (known.erase(destination) > 0)
        {
            LogInfo("dealer unregistered identity=" + destination);
        }
        return false;
    }
    LogWarning("message undeliverable " + summary +
               (sent == ZmqCode::WouldBlock ? std::string{" reason=send_queue_full"} : " reason=" + router.LastError()));
    return false;
}

void MessageRouter::ReadEvents()
{
    for (int i = 0; i < max_messages_per_step; ++i)
    {
        Message message{};
        if (monitor.Receive(message, true) != ZmqCode::Ok)
        {
            return;
        }
        SocketEvent event{};
        if (!DecodeSocketEvent(message, event))
        {
            continue;
        }

        if (event.kind == SocketEventKind::Accepted)
        {
            std::string address = PeerAddress(event.descriptor);
            if (address.empty())
            {
                address = "unknown";
            }
            peers[event.descriptor] = address;
            LogInfo("dealer connected address=" + address);
        }
        else if (event.kind == SocketEventKind::Disconnected)
        {
            std::string address{"unknown"};
            const auto found = peers.find(event.descriptor);
            if (found != peers.end())
            {
                address = found->second;
                peers.erase(found);
            }
            LogInfo("dealer disconnected address=" + address);
        }
    }
}

} // namespace process_manager
