#include <gtest/gtest.h>

#include "ZmqSocket.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Sockets = std::vector<process_manager::ZmqSocket*>;

bool WaitReadable(process_manager::ZmqSocket& socket, std::chrono::milliseconds timeout)
{
    std::vector<bool> ready{};
    return process_manager::PollReadable(Sockets{&socket}, timeout, ready) == process_manager::ZmqCode::Ok && ready[0];
}

} // namespace

TEST(ZmqSocketTest, FramesCompareByContent)
{
    const process_manager::Frame frame = process_manager::MakeFrame("BPM");
    EXPECT_EQ(frame.size(), 3u);
    EXPECT_TRUE(process_manager::FrameIs(frame, "BPM"));
    EXPECT_FALSE(process_manager::FrameIs(frame, "BP"));
    EXPECT_FALSE(process_manager::FrameIs(frame, "BPX"));
    EXPECT_FALSE(process_manager::ZmqVersion().empty());
}

TEST(ZmqSocketTest, AMandatoryRouterReportsUnknownPeers)
{
    process_manager::ZmqContext context{};
    process_manager::ZmqSocket router{context, process_manager::SocketType::Router};
    ASSERT_EQ(router.SetOption(process_manager::SocketOption::RouterMandatory, 1), process_manager::ZmqCode::Ok);
    ASSERT_EQ(router.Bind("tcp://127.0.0.1:*"), process_manager::ZmqCode::Ok) << router.LastError();
    const process_manager::Message message{process_manager::MakeFrame("nobody"), process_manager::MakeFrame("x")};
    EXPECT_EQ(router.Send(message, true), process_manager::ZmqCode::Unreachable);
    EXPECT_FALSE(router.LastError().empty());
}

TEST(ZmqSocketTest, MonitorsAcceptedConnections)
{
    process_manager::ZmqContext context{};
    process_manager::ZmqSocket router{context, process_manager::SocketType::Router};
    ASSERT_EQ(router.MonitorConnections("inproc://zmq-socket-test-monitor"), process_manager::ZmqCode::Ok)
        << router.LastError();
    process_manager::ZmqSocket events{context, process_manager::SocketType::Pair};
    ASSERT_EQ(events.Connect("inproc://zmq-socket-test-monitor"), process_manager::ZmqCode::Ok);
    ASSERT_EQ(router.Bind("tcp://127.0.0.1:*"), process_manager::ZmqCode::Ok) << router.LastError();

    process_manager::ZmqSocket dealer{context, process_manager::SocketType::Dealer};
    ASSERT_EQ(dealer.Connect(router.BoundEndpoint()), process_manager::ZmqCode::Ok);
    ASSERT_TRUE(WaitReadable(events, std::chrono::seconds{5}));
    process_manager::Message message{};
    ASSERT_EQ(events.Receive(message, true), process_manager::ZmqCode::Ok);
    process_manager::SocketEvent event{};
    ASSERT_TRUE(process_manager::DecodeSocketEvent(message, event));
    EXPECT_EQ(event.kind, process_manager::SocketEventKind::Accepted);
    EXPECT_EQ(event.endpoint, router.BoundEndpoint());
    EXPECT_NE(event.descriptor, 0u);

    process_manager::SocketEvent none{};
    EXPECT_FALSE(process_manager::DecodeSocketEvent(process_manager::Message{process_manager::Frame{1, 2}}, none));
}

TEST(ZmqSocketTest, RouterAndDealerExchangeMultipartMessages)
{
    process_manager::ZmqContext context{};
    process_manager::ZmqSocket router{context, process_manager::SocketType::Router};
    ASSERT_EQ(router.Bind("tcp://127.0.0.1:*"), process_manager::ZmqCode::Ok) << router.LastError();
    const std::string endpoint = router.BoundEndpoint();
    EXPECT_EQ(endpoint.rfind("tcp://127.0.0.1:", 0), 0u);

    process_manager::ZmqSocket dealer{context, process_manager::SocketType::Dealer};
    dealer.SetOption(process_manager::SocketOption::Identity, "PMC");
    ASSERT_EQ(dealer.Connect(endpoint), process_manager::ZmqCode::Ok);
    const process_manager::Message request{process_manager::MakeFrame("BPM"), process_manager::Frame{1, 2, 3}};
    ASSERT_EQ(dealer.Send(request, false), process_manager::ZmqCode::Ok);

    ASSERT_TRUE(WaitReadable(router, std::chrono::seconds{5}));
    process_manager::Message received{};
    ASSERT_EQ(router.Receive(received, true), process_manager::ZmqCode::Ok);
    ASSERT_EQ(received.size(), 3u);
    EXPECT_TRUE(process_manager::FrameIs(received[0], "PMC"));
    EXPECT_TRUE(process_manager::FrameIs(received[1], "BPM"));
    EXPECT_EQ(received[2], (process_manager::Frame{1, 2, 3}));

    const process_manager::Message answer{received[0], process_manager::Frame{}, process_manager::MakeFrame("ok")};
    ASSERT_EQ(router.Send(answer, true), process_manager::ZmqCode::Ok);
    ASSERT_TRUE(WaitReadable(dealer, std::chrono::seconds{5}));
    process_manager::Message reply{};
    ASSERT_EQ(dealer.Receive(reply, true), process_manager::ZmqCode::Ok);
    ASSERT_EQ(reply.size(), 2u);
    EXPECT_TRUE(reply[0].empty());
    EXPECT_TRUE(process_manager::FrameIs(reply[1], "ok"));
}

TEST(ZmqSocketTest, PublisherReachesSubscribers)
{
    process_manager::ZmqContext context{};
    process_manager::ZmqSocket publisher{context, process_manager::SocketType::Publisher};
    ASSERT_EQ(publisher.Bind("tcp://127.0.0.1:*"), process_manager::ZmqCode::Ok);
    process_manager::ZmqSocket subscriber{context, process_manager::SocketType::Subscriber};
    subscriber.SetOption(process_manager::SocketOption::Subscribe, "topic");
    ASSERT_EQ(subscriber.Connect(publisher.BoundEndpoint()), process_manager::ZmqCode::Ok);

    // A new subscription takes a moment to reach the publisher; keep sending until it does.
    const process_manager::Message other{process_manager::MakeFrame("other"), process_manager::MakeFrame("skip")};
    const process_manager::Message wanted{process_manager::MakeFrame("topic"), process_manager::MakeFrame("data")};
    process_manager::Message received{};
    for (int attempt = 0; attempt < 200 && received.empty(); ++attempt)
    {
        publisher.Send(other, true);
        publisher.Send(wanted, true);
        if (WaitReadable(subscriber, std::chrono::milliseconds{20}))
        {
            subscriber.Receive(received, true);
        }
    }
    ASSERT_EQ(received.size(), 2u);
    EXPECT_TRUE(process_manager::FrameIs(received[0], "topic"));
    EXPECT_TRUE(process_manager::FrameIs(received[1], "data"));
}

TEST(ZmqSocketTest, ReceiveWithoutAMessageWouldBlock)
{
    process_manager::ZmqContext context{};
    process_manager::ZmqSocket router{context, process_manager::SocketType::Router};
    ASSERT_EQ(router.Bind("tcp://127.0.0.1:*"), process_manager::ZmqCode::Ok);
    process_manager::Message message{};
    EXPECT_EQ(router.Receive(message, true), process_manager::ZmqCode::WouldBlock);
    EXPECT_FALSE(WaitReadable(router, std::chrono::milliseconds{20}));
}

// A second bind to a port in use is not checked here: WSL1 lets it succeed.
TEST(ZmqSocketTest, ReportsBindFailures)
{
    process_manager::ZmqContext context{};
    process_manager::ZmqSocket socket{context, process_manager::SocketType::Publisher};
    EXPECT_EQ(socket.Bind("nonsense"), process_manager::ZmqCode::Failed);
    EXPECT_FALSE(socket.LastError().empty());
    EXPECT_EQ(socket.Bind("tcp://256.256.256.256:1"), process_manager::ZmqCode::Failed);
}

TEST(ZmqSocketTest, EmptyMessagesAreRefused)
{
    process_manager::ZmqContext context{};
    process_manager::ZmqSocket dealer{context, process_manager::SocketType::Dealer};
    EXPECT_EQ(dealer.Send(process_manager::Message{}, true), process_manager::ZmqCode::Failed);
}

TEST(ZmqSocketTest, DescribesIdentities)
{
    EXPECT_EQ(process_manager::DescribeIdentity(process_manager::MakeFrame("PMC")), "PMC");
    EXPECT_EQ(process_manager::DescribeIdentity(process_manager::Frame{0x00, 0x6b, 0x8b}), "0x006b8b");
    EXPECT_EQ(process_manager::DescribeIdentity(process_manager::Frame{}), "0x");
}
