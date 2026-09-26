#include <gtest/gtest.h>

#include "MessageRouter.hpp"
#include "ZmqSocket.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Sockets = std::vector<process_manager::ZmqSocket*>;

process_manager::Frame Text(const std::string& text)
{
    return process_manager::MakeFrame(text);
}

std::unique_ptr<process_manager::ZmqSocket> Dealer(process_manager::ZmqContext& context, const std::string& identity,
                                                   const std::string& endpoint)
{
    std::unique_ptr<process_manager::ZmqSocket> dealer =
        std::make_unique<process_manager::ZmqSocket>(context, process_manager::SocketType::Dealer);
    dealer->SetOption(process_manager::SocketOption::Identity, identity);
    dealer->Connect(endpoint);
    return dealer;
}

bool Receive(process_manager::ZmqSocket& socket, std::chrono::milliseconds timeout, process_manager::Message& out)
{
    std::vector<bool> ready{};
    process_manager::PollReadable(Sockets{&socket}, timeout, ready);
    return ready[0] && socket.Receive(out, true) == process_manager::ZmqCode::Ok;
}

// Sends until the receiver has it: a peer that has not finished connecting to
// the router yet is unknown to it, and the router drops what it cannot deliver.
bool Deliver(process_manager::ZmqSocket& from, const std::string& destination, const process_manager::Message& payload,
             process_manager::ZmqSocket& to, process_manager::Message& received)
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        process_manager::Message message{Text(destination)};
        message.insert(message.end(), payload.begin(), payload.end());
        from.Send(message, true);
        if (Receive(to, std::chrono::milliseconds{100}, received))
        {
            return true;
        }
    }
    return false;
}

// A router on its own thread, stepping until the fixture goes away.
struct RunningRouter
{
    process_manager::ZmqContext context{};
    process_manager::MessageRouter router{context};
    std::string endpoint;
    std::atomic<bool> stop{false};
    std::thread loop;

    RunningRouter()
    {
        std::string error{};
        if (router.Bind("tcp://127.0.0.1:*", error) == process_manager::ZmqCode::Ok)
        {
            endpoint = router.Endpoint();
            loop = std::thread{[this]()
                               {
                                   while (!stop.load())
                                   {
                                       router.Step(std::chrono::milliseconds{20});
                                   }
                               }};
        }
    }

    ~RunningRouter()
    {
        stop.store(true);
        if (loop.joinable())
        {
            loop.join();
        }
    }
};

} // namespace

TEST(MessageRouterTest, ForwardsToTheNamedPeerWithTheSenderFirst)
{
    RunningRouter fixture{};
    ASSERT_FALSE(fixture.endpoint.empty());
    process_manager::ZmqContext peers{};
    std::unique_ptr<process_manager::ZmqSocket> a = Dealer(peers, "A", fixture.endpoint);
    std::unique_ptr<process_manager::ZmqSocket> b = Dealer(peers, "B", fixture.endpoint);

    process_manager::Message received{};
    ASSERT_TRUE(Deliver(*a, "B", process_manager::Message{Text("hello")}, *b, received));
    ASSERT_EQ(received.size(), 2u);
    EXPECT_TRUE(process_manager::FrameIs(received[0], "A"));
    EXPECT_TRUE(process_manager::FrameIs(received[1], "hello"));

    // B answers the way it was addressed: to the identity it received first.
    ASSERT_TRUE(Deliver(*b, "A", process_manager::Message{Text("ok")}, *a, received));
    ASSERT_EQ(received.size(), 2u);
    EXPECT_TRUE(process_manager::FrameIs(received[0], "B"));
    EXPECT_TRUE(process_manager::FrameIs(received[1], "ok"));
}

TEST(MessageRouterTest, KeepsMultipartPayloadsIntact)
{
    RunningRouter fixture{};
    ASSERT_FALSE(fixture.endpoint.empty());
    process_manager::ZmqContext peers{};
    std::unique_ptr<process_manager::ZmqSocket> a = Dealer(peers, "A", fixture.endpoint);
    std::unique_ptr<process_manager::ZmqSocket> b = Dealer(peers, "B", fixture.endpoint);

    process_manager::Message received{};
    ASSERT_TRUE(Deliver(*a, "B", process_manager::Message{Text("BPM"), process_manager::Frame{1, 2, 3}}, *b, received));
    ASSERT_EQ(received.size(), 3u);
    EXPECT_TRUE(process_manager::FrameIs(received[0], "A"));
    EXPECT_TRUE(process_manager::FrameIs(received[1], "BPM"));
    EXPECT_EQ(received[2], (process_manager::Frame{1, 2, 3}));
}

TEST(MessageRouterTest, DropsWhatNoPeerCanReceiveAndStaysUp)
{
    RunningRouter fixture{};
    ASSERT_FALSE(fixture.endpoint.empty());
    process_manager::ZmqContext peers{};
    std::unique_ptr<process_manager::ZmqSocket> a = Dealer(peers, "A", fixture.endpoint);
    std::unique_ptr<process_manager::ZmqSocket> b = Dealer(peers, "B", fixture.endpoint);

    a->Send(process_manager::Message{Text("missing"), Text("nope")}, true);
    a->Send(process_manager::Message{Text("")}, true);            // no destination at all
    a->Send(process_manager::Message{process_manager::Frame{}, process_manager::Frame{}}, true);

    process_manager::Message received{};
    ASSERT_TRUE(Deliver(*a, "B", process_manager::Message{Text("still")}, *b, received));
    ASSERT_EQ(received.size(), 2u);
    EXPECT_TRUE(process_manager::FrameIs(received[1], "still"));
    EXPECT_FALSE(Receive(*b, std::chrono::milliseconds{100}, received)); // nothing else reached B
}

TEST(MessageRouterTest, APeerThatReconnectsUnderItsIdentityStillReceives)
{
    RunningRouter fixture{};
    ASSERT_FALSE(fixture.endpoint.empty());
    process_manager::ZmqContext peers{};
    std::unique_ptr<process_manager::ZmqSocket> a = Dealer(peers, "A", fixture.endpoint);
    std::unique_ptr<process_manager::ZmqSocket> b = Dealer(peers, "B", fixture.endpoint);
    process_manager::Message received{};
    ASSERT_TRUE(Deliver(*a, "B", process_manager::Message{Text("first")}, *b, received));

    b.reset();
    std::unique_ptr<process_manager::ZmqSocket> again = Dealer(peers, "B", fixture.endpoint);
    ASSERT_TRUE(Deliver(*a, "B", process_manager::Message{Text("after-reconnect")}, *again, received));
    ASSERT_EQ(received.size(), 2u);
    EXPECT_TRUE(process_manager::FrameIs(received[0], "A"));
    EXPECT_TRUE(process_manager::FrameIs(received[1], "after-reconnect"));
}

TEST(MessageRouterTest, StepCountsWhatItForwarded)
{
    process_manager::ZmqContext context{};
    process_manager::MessageRouter router{context};
    std::string error{};
    ASSERT_EQ(router.Bind("tcp://127.0.0.1:*", error), process_manager::ZmqCode::Ok) << error;

    process_manager::ZmqContext peers{};
    std::unique_ptr<process_manager::ZmqSocket> a = Dealer(peers, "A", router.Endpoint());
    std::unique_ptr<process_manager::ZmqSocket> b = Dealer(peers, "B", router.Endpoint());
    std::this_thread::sleep_for(std::chrono::milliseconds{100}); // both connected, so B is known
    a->Send(process_manager::Message{Text("B"), Text("one")}, true);
    a->Send(process_manager::Message{Text("B"), Text("two")}, true);

    int forwarded = 0;
    for (int attempt = 0; attempt < 50 && forwarded < 2; ++attempt)
    {
        forwarded += router.Step(std::chrono::milliseconds{100});
    }
    EXPECT_EQ(forwarded, 2);
    process_manager::Message received{};
    ASSERT_TRUE(Receive(*b, std::chrono::seconds{2}, received));
    EXPECT_TRUE(process_manager::FrameIs(received[1], "one"));
    ASSERT_TRUE(Receive(*b, std::chrono::seconds{2}, received));
    EXPECT_TRUE(process_manager::FrameIs(received[1], "two"));
    EXPECT_EQ(router.Step(std::chrono::milliseconds{10}), 0);
}

TEST(MessageRouterTest, ReportsBindFailures)
{
    process_manager::ZmqContext context{};
    process_manager::MessageRouter router{context};
    std::string error{};
    EXPECT_EQ(router.Bind("nonsense", error), process_manager::ZmqCode::Failed);
    EXPECT_NE(error.find("nonsense"), std::string::npos) << error;
}
