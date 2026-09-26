#include <gtest/gtest.h>

#include "CommandServer.hpp"
#include "CommandMessage.hpp"
#include "ZmqSocket.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

using Sockets = std::vector<process_manager::ZmqSocket*>;

struct Fixture
{
    process_manager::ZmqContext context{};
    process_manager::CommandServer server{context};
    process_manager::ZmqSocket dealer{context, process_manager::SocketType::Dealer};

    Fixture()
    {
        std::string error{};
        server.Bind("tcp://127.0.0.1:*", error);
        dealer.SetOption(process_manager::SocketOption::Identity, "PMC");
        dealer.Connect(server.Endpoint());
    }

    process_manager::RequestCode Next(process_manager::CommandRequest& request, std::string& problem)
    {
        std::vector<bool> ready{};
        process_manager::PollReadable(Sockets{&server.Socket()}, std::chrono::seconds{5}, ready);
        return server.Receive(request, problem);
    }

    process_manager::Message Reply()
    {
        std::vector<bool> ready{};
        process_manager::PollReadable(Sockets{&dealer}, std::chrono::seconds{5}, ready);
        process_manager::Message message{};
        dealer.Receive(message, true);
        return message;
    }
};

process_manager::Frame Command(process_manager::CommandCode code, const std::string& name)
{
    process_manager::CommandMessage message{};
    message.command = static_cast<std::uint8_t>(code);
    message.serviceName = name;
    return process_manager::EncodeCommand(message);
}

} // namespace

TEST(CommandServerTest, AcceptsTheGuiFramesAndAnswersThem)
{
    Fixture fixture{};
    const process_manager::Message restart{process_manager::MakeFrame("BPM"),
                                           Command(process_manager::CommandCode::Restart, "vision")};
    fixture.dealer.Send(restart, false);
    process_manager::CommandRequest request{};
    std::string problem{};
    ASSERT_EQ(fixture.Next(request, problem), process_manager::RequestCode::Ok) << problem;
    EXPECT_TRUE(process_manager::FrameIs(request.identity, "PMC"));
    EXPECT_FALSE(request.delimiter);
    EXPECT_EQ(request.command.command, 81);
    EXPECT_EQ(request.command.serviceName, "vision");

    process_manager::CommandReply reply{};
    reply.command = 81;
    reply.serviceName = "vision";
    reply.message = "restarting";
    ASSERT_EQ(fixture.server.Reply(request, reply), process_manager::ZmqCode::Ok);
    const process_manager::Message answer = fixture.Reply();
    ASSERT_EQ(answer.size(), 2u);
    EXPECT_TRUE(process_manager::FrameIs(answer[0], "BPM"));
    process_manager::CommandReply decoded{};
    ASSERT_EQ(process_manager::DecodeReply(answer[1], decoded), process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.message, "restarting");
}

TEST(CommandServerTest, EchoesARequestStyleDelimiter)
{
    Fixture fixture{};
    fixture.dealer.Send(process_manager::Message{process_manager::Frame{}, process_manager::MakeFrame("BPM"),
                         Command(process_manager::CommandCode::Stop, "a")},
                        false);
    process_manager::CommandRequest request{};
    std::string problem{};
    ASSERT_EQ(fixture.Next(request, problem), process_manager::RequestCode::Ok) << problem;
    EXPECT_TRUE(request.delimiter);
    fixture.server.Reply(request, process_manager::CommandReply{});
    const process_manager::Message answer = fixture.Reply();
    ASSERT_EQ(answer.size(), 3u);
    EXPECT_TRUE(answer[0].empty());
}

TEST(CommandServerTest, AcceptsACommandWithoutTheTag)
{
    Fixture fixture{};
    fixture.dealer.Send(process_manager::Message{Command(process_manager::CommandCode::Start, "a")}, false);
    process_manager::CommandRequest request{};
    std::string problem{};
    ASSERT_EQ(fixture.Next(request, problem), process_manager::RequestCode::Ok) << problem;
    EXPECT_EQ(request.command.command, 78);
}

TEST(CommandServerTest, ReportsMalformedRequestsWithTheirIdentity)
{
    Fixture fixture{};
    const process_manager::Message garbled{process_manager::MakeFrame("BPM"), process_manager::Frame{1, 2, 3}};
    fixture.dealer.Send(garbled, false);
    process_manager::CommandRequest request{};
    std::string problem{};
    ASSERT_EQ(fixture.Next(request, problem), process_manager::RequestCode::Malformed);
    EXPECT_NE(problem.find("3 bytes"), std::string::npos) << problem;
    EXPECT_TRUE(process_manager::FrameIs(request.identity, "PMC"));

    fixture.dealer.Send(process_manager::Message{process_manager::MakeFrame("BPM")}, false);
    ASSERT_EQ(fixture.Next(request, problem), process_manager::RequestCode::Malformed);
}

TEST(CommandServerTest, NothingQueuedMeansEmpty)
{
    Fixture fixture{};
    process_manager::CommandRequest request{};
    std::string problem{};
    EXPECT_EQ(fixture.server.Receive(request, problem), process_manager::RequestCode::Empty);
}
