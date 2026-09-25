#include "ZmqSocket.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <zmq.h>

namespace process_manager
{

namespace
{

constexpr std::size_t endpoint_buffer_size = 1024;

int TypeValue(SocketType type)
{
    switch (type)
    {
    case SocketType::Publisher:
        return ZMQ_PUB;
    case SocketType::Subscriber:
        return ZMQ_SUB;
    case SocketType::Router:
        return ZMQ_ROUTER;
    case SocketType::Dealer:
        return ZMQ_DEALER;
    }
    return ZMQ_DEALER;
}

int OptionValue(SocketOption option)
{
    switch (option)
    {
    case SocketOption::Linger:
        return ZMQ_LINGER;
    case SocketOption::SendHighWater:
        return ZMQ_SNDHWM;
    case SocketOption::ReceiveHighWater:
        return ZMQ_RCVHWM;
    case SocketOption::RouterHandover:
        return ZMQ_ROUTER_HANDOVER;
    case SocketOption::Ipv6:
        return ZMQ_IPV6;
    case SocketOption::Subscribe:
        return ZMQ_SUBSCRIBE;
    case SocketOption::Identity:
        return ZMQ_IDENTITY;
    }
    return ZMQ_LINGER;
}

} // namespace

Frame MakeFrame(std::string_view text)
{
    return Frame{text.begin(), text.end()};
}

bool FrameIs(const Frame& frame, std::string_view text)
{
    if (frame.size() != text.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < frame.size(); ++i)
    {
        if (frame[i] != static_cast<std::uint8_t>(text[i]))
        {
            return false;
        }
    }
    return true;
}

std::string ZmqVersion()
{
    int major = 0;
    int minor = 0;
    int patch = 0;
    zmq_version(&major, &minor, &patch);
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

ZmqContext::ZmqContext()
    : handle{zmq_ctx_new()}
{
}

ZmqContext::~ZmqContext()
{
    if (handle != nullptr)
    {
        while (zmq_ctx_term(handle) != 0 && zmq_errno() == EINTR)
        {
        }
    }
}

ZmqContext::ZmqContext(ZmqContext&& other) noexcept
    : handle{other.handle}
{
    other.handle = nullptr;
}

ZmqContext& ZmqContext::operator=(ZmqContext&& other) noexcept
{
    if (this != &other)
    {
        ZmqContext closing{std::move(*this)};
        handle = other.handle;
        other.handle = nullptr;
    }
    return *this;
}

void* ZmqContext::Handle() const
{
    return handle;
}

ZmqSocket::ZmqSocket(ZmqContext& context, SocketType type)
    : handle{context.Handle() != nullptr ? zmq_socket(context.Handle(), TypeValue(type)) : nullptr},
      lastError{handle != nullptr ? 0 : zmq_errno()}
{
    if (handle != nullptr)
    {
        SetOption(SocketOption::Linger, 0);
    }
}

ZmqSocket::~ZmqSocket()
{
    Close();
}

ZmqSocket::ZmqSocket(ZmqSocket&& other) noexcept
    : handle{other.handle}, lastError{other.lastError}
{
    other.handle = nullptr;
}

ZmqSocket& ZmqSocket::operator=(ZmqSocket&& other) noexcept
{
    if (this != &other)
    {
        Close();
        handle = other.handle;
        lastError = other.lastError;
        other.handle = nullptr;
    }
    return *this;
}

bool ZmqSocket::Valid() const
{
    return handle != nullptr;
}

ZmqCode ZmqSocket::Bind(const std::string& endpoint)
{
    if (handle == nullptr || zmq_bind(handle, endpoint.c_str()) != 0)
    {
        lastError = handle == nullptr ? ENOTSOCK : zmq_errno();
        return ZmqCode::Failed;
    }
    return ZmqCode::Ok;
}

ZmqCode ZmqSocket::Connect(const std::string& endpoint)
{
    if (handle == nullptr || zmq_connect(handle, endpoint.c_str()) != 0)
    {
        lastError = handle == nullptr ? ENOTSOCK : zmq_errno();
        return ZmqCode::Failed;
    }
    return ZmqCode::Ok;
}

ZmqCode ZmqSocket::SetOption(SocketOption option, int value)
{
    if (handle == nullptr || zmq_setsockopt(handle, OptionValue(option), &value, sizeof(value)) != 0)
    {
        lastError = handle == nullptr ? ENOTSOCK : zmq_errno();
        return ZmqCode::Failed;
    }
    return ZmqCode::Ok;
}

ZmqCode ZmqSocket::SetOption(SocketOption option, std::string_view value)
{
    if (handle == nullptr || zmq_setsockopt(handle, OptionValue(option), value.data(), value.size()) != 0)
    {
        lastError = handle == nullptr ? ENOTSOCK : zmq_errno();
        return ZmqCode::Failed;
    }
    return ZmqCode::Ok;
}

ZmqCode ZmqSocket::Send(std::span<const Frame> message, bool dontWait)
{
    if (handle == nullptr || message.empty())
    {
        lastError = handle == nullptr ? ENOTSOCK : EINVAL;
        return ZmqCode::Failed;
    }

    static const std::uint8_t empty = 0;
    for (std::size_t i = 0; i < message.size(); ++i)
    {
        const Frame& frame = message[i];
        const int flags = (i + 1 < message.size() ? ZMQ_SNDMORE : 0) | (dontWait ? ZMQ_DONTWAIT : 0);
        const void* data = frame.empty() ? static_cast<const void*>(&empty) : frame.data();
        if (zmq_send(handle, data, frame.size(), flags) < 0)
        {
            lastError = zmq_errno();
            return lastError == EAGAIN && i == 0 ? ZmqCode::WouldBlock : ZmqCode::Failed;
        }
    }
    return ZmqCode::Ok;
}

ZmqCode ZmqSocket::Receive(Message& out, bool dontWait)
{
    out.clear();
    if (handle == nullptr)
    {
        lastError = ENOTSOCK;
        return ZmqCode::Failed;
    }

    bool more = true;
    while (more)
    {
        zmq_msg_t part{};
        zmq_msg_init(&part);
        if (zmq_msg_recv(&part, handle, dontWait ? ZMQ_DONTWAIT : 0) < 0)
        {
            lastError = zmq_errno();
            zmq_msg_close(&part);
            if (out.empty() && (lastError == EAGAIN || lastError == EINTR))
            {
                return ZmqCode::WouldBlock;
            }
            return ZmqCode::Failed;
        }
        const std::uint8_t* data = static_cast<const std::uint8_t*>(zmq_msg_data(&part));
        out.emplace_back(data, data + zmq_msg_size(&part));
        more = zmq_msg_more(&part) != 0;
        zmq_msg_close(&part);
    }
    return ZmqCode::Ok;
}

std::string ZmqSocket::BoundEndpoint() const
{
    if (handle == nullptr)
    {
        return std::string{};
    }

    char endpoint[endpoint_buffer_size]{};
    std::size_t size = sizeof(endpoint);
    if (zmq_getsockopt(handle, ZMQ_LAST_ENDPOINT, endpoint, &size) != 0)
    {
        return std::string{};
    }
    return std::string{endpoint};
}

std::string ZmqSocket::LastError() const
{
    return std::string{zmq_strerror(lastError)};
}

void* ZmqSocket::Handle() const
{
    return handle;
}

void ZmqSocket::Close()
{
    if (handle != nullptr)
    {
        zmq_close(handle);
        handle = nullptr;
    }
}

ZmqCode PollReadable(std::span<ZmqSocket* const> sockets, std::chrono::milliseconds timeout,
                     std::vector<bool>& ready)
{
    // A null socket in a poll item would make libzmq poll file descriptor 0 instead.
    std::vector<zmq_pollitem_t> items{};
    std::vector<std::size_t> positions{};
    for (std::size_t i = 0; i < sockets.size(); ++i)
    {
        if (sockets[i] == nullptr || sockets[i]->Handle() == nullptr)
        {
            continue;
        }
        zmq_pollitem_t item{};
        item.socket = sockets[i]->Handle();
        item.events = ZMQ_POLLIN;
        items.push_back(item);
        positions.push_back(i);
    }
    ready.assign(sockets.size(), false);

    const long milliseconds = static_cast<long>(timeout.count() < 0 ? 0 : timeout.count());
    if (zmq_poll(items.data(), static_cast<int>(items.size()), milliseconds) < 0)
    {
        return zmq_errno() == EINTR ? ZmqCode::Ok : ZmqCode::Failed;
    }
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        ready[positions[i]] = (items[i].revents & ZMQ_POLLIN) != 0;
    }
    return ZmqCode::Ok;
}

} // namespace process_manager
