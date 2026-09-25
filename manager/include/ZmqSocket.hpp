#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace process_manager
{

using Frame = std::vector<std::uint8_t>;
using Message = std::vector<Frame>;

enum class ZmqCode
{
    Ok,
    WouldBlock,
    Failed,
};

enum class SocketType
{
    Publisher,
    Subscriber,
    Router,
    Dealer,
};

enum class SocketOption
{
    Linger,
    SendHighWater,
    ReceiveHighWater,
    RouterHandover,
    Ipv6,
    Subscribe,
    Identity,
};

/**
 * @brief Make a frame holding text.
 * @param text Bytes to copy.
 * @return The frame.
 */
Frame MakeFrame(std::string_view text);

/**
 * @brief Compare a frame with text.
 * @param frame Frame to check.
 * @param text Expected bytes.
 * @return true when the bytes match exactly.
 */
bool FrameIs(const Frame& frame, std::string_view text);

/**
 * @brief The libzmq version linked in.
 * @return For example "4.3.2".
 */
std::string ZmqVersion();

class ZmqContext
{
public:
    /**
     * @brief Create a libzmq context.
     */
    ZmqContext();

    /**
     * @brief Terminate the context. Every socket must be closed first.
     */
    ~ZmqContext();

    ZmqContext(const ZmqContext&) = delete;
    ZmqContext& operator=(const ZmqContext&) = delete;

    /**
     * @brief Take over another context.
     * @param other Context to move from. It is left empty.
     */
    ZmqContext(ZmqContext&& other) noexcept;

    /**
     * @brief Take over another context, terminating this one first.
     * @param other Context to move from. It is left empty.
     * @return This context.
     */
    ZmqContext& operator=(ZmqContext&& other) noexcept;

    /**
     * @brief The libzmq handle.
     * @return The handle, or nullptr when creation failed.
     */
    void* Handle() const;

private:
    void* handle;
};

class ZmqSocket
{
public:
    /**
     * @brief Create a socket with linger 0, so closing never waits for unsent messages.
     * @param context Context to create it in. It must outlive the socket.
     * @param type Socket pattern.
     */
    ZmqSocket(ZmqContext& context, SocketType type);

    /**
     * @brief Close the socket.
     */
    ~ZmqSocket();

    ZmqSocket(const ZmqSocket&) = delete;
    ZmqSocket& operator=(const ZmqSocket&) = delete;

    /**
     * @brief Take over another socket.
     * @param other Socket to move from. It is left closed.
     */
    ZmqSocket(ZmqSocket&& other) noexcept;

    /**
     * @brief Take over another socket, closing this one first.
     * @param other Socket to move from. It is left closed.
     * @return This socket.
     */
    ZmqSocket& operator=(ZmqSocket&& other) noexcept;

    /**
     * @brief Tell whether the socket was created.
     * @return true when it can be used.
     */
    bool Valid() const;

    /**
     * @brief Bind to an endpoint.
     * @param endpoint For example tcp://0.0.0.0:5557; the port "*" picks a free one.
     * @return ZmqCode::Ok, or ZmqCode::Failed; LastError tells why.
     */
    ZmqCode Bind(const std::string& endpoint);

    /**
     * @brief Connect to an endpoint. The connection completes in the background.
     * @param endpoint For example tcp://127.0.0.1:5557.
     * @return ZmqCode::Ok, or ZmqCode::Failed for a malformed endpoint.
     */
    ZmqCode Connect(const std::string& endpoint);

    /**
     * @brief Set an integer option.
     * @param option Option to set.
     * @param value New value; milliseconds, message counts or 0 and 1, by option.
     * @return ZmqCode::Ok, or ZmqCode::Failed.
     */
    ZmqCode SetOption(SocketOption option, int value);

    /**
     * @brief Set a byte-string option such as a subscription prefix.
     * @param option Option to set.
     * @param value New value.
     * @return ZmqCode::Ok, or ZmqCode::Failed.
     */
    ZmqCode SetOption(SocketOption option, std::string_view value);

    /**
     * @brief Send a multipart message.
     * @param message Frames to send; it must not be empty.
     * @param dontWait Return ZmqCode::WouldBlock instead of waiting when the queue is full.
     * @return ZmqCode::Ok, ZmqCode::WouldBlock, or ZmqCode::Failed.
     */
    ZmqCode Send(std::span<const Frame> message, bool dontWait);

    /**
     * @brief Receive a multipart message.
     * @param out Receives the frames.
     * @param dontWait Return ZmqCode::WouldBlock instead of waiting when nothing is queued.
     * @return ZmqCode::Ok, ZmqCode::WouldBlock, or ZmqCode::Failed.
     */
    ZmqCode Receive(Message& out, bool dontWait);

    /**
     * @brief The endpoint the socket was last bound to, with the actual port.
     * @return For example tcp://0.0.0.0:5557; empty before a bind.
     */
    std::string BoundEndpoint() const;

    /**
     * @brief Describe the last failure.
     * @return libzmq's error text.
     */
    std::string LastError() const;

    /**
     * @brief The libzmq handle, for polling.
     * @return The handle, or nullptr when closed.
     */
    void* Handle() const;

private:
    void Close();

    void* handle;
    int lastError;
};

/**
 * @brief Wait until a socket has a message to read.
 * @param sockets Sockets to watch.
 * @param timeout Longest wait; zero checks without waiting.
 * @param ready Receives one flag per socket.
 * @return ZmqCode::Ok when the wait ended, even with nothing ready or when a signal
 *         interrupted it; ZmqCode::Failed otherwise.
 */
ZmqCode PollReadable(std::span<ZmqSocket* const> sockets, std::chrono::milliseconds timeout,
                     std::vector<bool>& ready);

} // namespace process_manager
