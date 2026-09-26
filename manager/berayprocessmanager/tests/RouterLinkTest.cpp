#include <gtest/gtest.h>

#include "RouterLink.hpp"
#include "CommandMessage.hpp"
#include "CommandServer.hpp"
#include "MessageRouter.hpp"
#include "ZmqSocket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Sockets = std::vector<process_manager::ZmqSocket*>;

process_manager::Frame Command(process_manager::CommandCode code, const std::string& name)
{
    process_manager::CommandMessage message{};
    message.command = static_cast<std::uint8_t>(code);
    message.serviceName = name;
    return process_manager::EncodeCommand(message);
}

// A router on a thread, the manager's link to it as "bpm", and a peer "PMC".
struct Fixture
{
    process_manager::ZmqContext context{};
    process_manager::MessageRouter router{context};
    process_manager::RouterLink link{context};
    process_manager::ZmqSocket peer{context, process_manager::SocketType::Dealer};
    std::atomic<bool> stop{false};
    std::thread loop;
    std::string endpoint; // read before the loop owns the router socket
    std::string error;

    Fixture()
    {
        if (router.Bind("tcp://127.0.0.1:*", error) != process_manager::ZmqCode::Ok)
        {
            return;
        }
        endpoint = router.Endpoint();
        loop = std::thread{[this]()
                           {
                               while (!stop.load())
                               {
                                   router.Step(std::chrono::milliseconds{20});
                               }
                           }};
        link.Connect(endpoint, "bpm", error);
        peer.SetOption(process_manager::SocketOption::Identity, "PMC");
        peer.Connect(endpoint);
    }

    ~Fixture()
    {
        stop.store(true);
        if (loop.joinable())
        {
            loop.join();
        }
    }

    // The peer sends [bpm]["BPM"][frame] until the link has it (the router drops
    // what it cannot deliver while the link is still connecting).
    process_manager::RequestCode Ask(const process_manager::Frame& frame, process_manager::CommandRequest& request,
                                     std::string& problem)
    {
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            peer.Send(process_manager::Message{process_manager::MakeFrame("bpm"), process_manager::MakeFrame("BPM"), frame},
                      true);
            std::vector<bool> ready{};
            process_manager::PollReadable(Sockets{&link.Socket()}, std::chrono::milliseconds{100}, ready);
            if (ready[0])
            {
                return link.Receive(request, problem);
            }
        }
        return process_manager::RequestCode::Empty;
    }

    process_manager::Message Answer()
    {
        std::vector<bool> ready{};
        process_manager::PollReadable(Sockets{&peer}, std::chrono::seconds{5}, ready);
        process_manager::Message message{};
        peer.Receive(message, true);
        return message;
    }
};

} // namespace

TEST(RouterLinkTest, ACommandThroughTheRouterCarriesThePeerAndIsAnsweredToIt)
{
    Fixture fixture{};
    ASSERT_TRUE(fixture.error.empty()) << fixture.error;
    EXPECT_EQ(fixture.link.Endpoint(), fixture.endpoint);
    EXPECT_EQ(fixture.link.Identity(), "bpm");

    process_manager::CommandRequest request{};
    std::string problem{};
    ASSERT_EQ(fixture.Ask(Command(process_manager::CommandCode::Restart, "vision"), request, problem),
              process_manager::RequestCode::Ok)
        << problem;
    EXPECT_TRUE(process_manager::FrameIs(request.identity, "PMC"));
    EXPECT_FALSE(request.delimiter);
    EXPECT_EQ(request.command.command, 81);
    EXPECT_EQ(request.command.serviceName, "vision");

    process_manager::CommandReply reply{};
    reply.command = 81;
    reply.serviceName = "vision";
    reply.message = "restarting";
    ASSERT_EQ(fixture.link.Reply(request, reply), process_manager::ZmqCode::Ok);
    const process_manager::Message answer = fixture.Answer();
    ASSERT_EQ(answer.size(), 3u);
    EXPECT_TRUE(process_manager::FrameIs(answer[0], "bpm"));
    EXPECT_TRUE(process_manager::FrameIs(answer[1], "BPM"));
    process_manager::CommandReply decoded{};
    ASSERT_EQ(process_manager::DecodeReply(answer[2], decoded), process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.message, "restarting");
}

TEST(RouterLinkTest, AMalformedRequestIsAnsweredThroughTheRouter)
{
    Fixture fixture{};
    ASSERT_TRUE(fixture.error.empty()) << fixture.error;
    process_manager::CommandRequest request{};
    std::string problem{};
    ASSERT_EQ(fixture.Ask(process_manager::Frame{1, 2, 3}, request, problem), process_manager::RequestCode::Malformed);
    EXPECT_NE(problem.find("3 bytes"), std::string::npos) << problem;
    EXPECT_TRUE(process_manager::FrameIs(request.identity, "PMC"));

    process_manager::CommandReply reply{};
    reply.result = process_manager::CommandResult::Malformed;
    reply.message = problem;
    ASSERT_EQ(fixture.link.Reply(request, reply), process_manager::ZmqCode::Ok);
    const process_manager::Message answer = fixture.Answer();
    ASSERT_EQ(answer.size(), 3u);
    process_manager::CommandReply decoded{};
    ASSERT_EQ(process_manager::DecodeReply(answer[2], decoded), process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.result, process_manager::CommandResult::Malformed);
}

TEST(RouterLinkTest, NothingQueuedMeansEmpty)
{
    Fixture fixture{};
    ASSERT_TRUE(fixture.error.empty()) << fixture.error;
    process_manager::CommandRequest request{};
    std::string problem{};
    EXPECT_EQ(fixture.link.Receive(request, problem), process_manager::RequestCode::Empty);
}

TEST(RouterLinkTest, RejectsANonsenseEndpoint)
{
    process_manager::ZmqContext context{};
    process_manager::RouterLink link{context};
    std::string error{};
    EXPECT_EQ(link.Connect("nonsense", "bpm", error), process_manager::ZmqCode::Failed);
    EXPECT_NE(error.find("nonsense"), std::string::npos) << error;
}
