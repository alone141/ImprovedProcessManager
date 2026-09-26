#include <gtest/gtest.h>

#include "ManagerClient.hpp"
#include "CommandMessage.hpp"
#include "CommandServer.hpp"
#include "DetailedReport.hpp"
#include "MessageRouter.hpp"
#include "ReportPublisher.hpp"
#include "RouterLink.hpp"
#include "ZmqSocket.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Sockets = std::vector<process_manager::ZmqSocket*>;

} // namespace

TEST(ManagerClientTest, SendsACommandAndReadsTheReply)
{
    process_manager::ZmqContext context{};
    process_manager::CommandServer server{context};
    std::string error{};
    ASSERT_EQ(server.Bind("tcp://127.0.0.1:*", error), process_manager::ZmqCode::Ok) << error;
    const std::string endpoint = server.Endpoint();

    std::thread answer{[&server]()
                       {
                           std::vector<bool> ready{};
                           process_manager::PollReadable(Sockets{&server.Socket()}, std::chrono::seconds{5}, ready);
                           process_manager::CommandRequest request{};
                           std::string problem{};
                           if (server.Receive(request, problem) != process_manager::RequestCode::Ok)
                           {
                               return;
                           }
                           process_manager::CommandReply reply{};
                           reply.command = request.command.command;
                           reply.serviceName = request.command.serviceName;
                           reply.message = "done";
                           server.Reply(request, reply);
                       }};

    process_manager::ManagerClient client{endpoint, "tcp://127.0.0.1:1"};
    process_manager::CommandReply reply{};
    const process_manager::ClientCode code = client.SendCommand(process_manager::CommandCode::Restart, "vision",
                                                                std::chrono::seconds{5}, reply, error);
    answer.join();
    ASSERT_EQ(code, process_manager::ClientCode::Ok) << error;
    EXPECT_EQ(reply.command, 81);
    EXPECT_EQ(reply.serviceName, "vision");
    EXPECT_EQ(reply.message, "done");
    EXPECT_EQ(reply.result, process_manager::CommandResult::Ok);
}

TEST(ManagerClientTest, TimesOutWithoutAManager)
{
    process_manager::ZmqContext context{};
    process_manager::ZmqSocket reserve{context, process_manager::SocketType::Router};
    ASSERT_EQ(reserve.Bind("tcp://127.0.0.1:*"), process_manager::ZmqCode::Ok);
    const std::string endpoint = reserve.BoundEndpoint();

    process_manager::ManagerClient client{endpoint, endpoint};
    process_manager::CommandReply reply{};
    std::string error{};
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    EXPECT_EQ(client.SendCommand(process_manager::CommandCode::Stop, "x", std::chrono::milliseconds{200}, reply, error),
              process_manager::ClientCode::Timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{3});
    EXPECT_NE(error.find("no reply"), std::string::npos) << error;
}

TEST(ManagerClientTest, WaitsForTheNextReport)
{
    process_manager::ZmqContext context{};
    process_manager::ReportPublisher publisher{context};
    std::string error{};
    ASSERT_EQ(publisher.Bind("tcp://127.0.0.1:*", "tcp://127.0.0.1:*", error), process_manager::ZmqCode::Ok);
    const std::string reportEndpoint = publisher.ReportEndpoint();

    std::atomic<bool> done{false};
    std::thread publish{[&publisher, &done]()
                        {
                            process_manager::DetailedReport report{};
                            report.hostName = "rig";
                            while (!done.load())
                            {
                                publisher.Publish({}, report);
                                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                            }
                        }};

    process_manager::ManagerClient client{"tcp://127.0.0.1:1", reportEndpoint};
    process_manager::DetailedReport received{};
    const process_manager::ClientCode code = client.WaitForReport(std::chrono::seconds{5}, received, error);
    EXPECT_EQ(client.WaitForReport(std::chrono::seconds{5}, received, error), process_manager::ClientCode::Ok);
    done.store(true);
    publish.join();
    ASSERT_EQ(code, process_manager::ClientCode::Ok) << error;
    EXPECT_EQ(received.hostName, "rig");
}

TEST(ManagerClientTest, ReportWaitTimesOut)
{
    process_manager::ManagerClient client{"tcp://127.0.0.1:1", "tcp://127.0.0.1:1"};
    process_manager::DetailedReport received{};
    std::string error{};
    EXPECT_EQ(client.WaitForReport(std::chrono::milliseconds{100}, received, error), process_manager::ClientCode::Timeout);
    EXPECT_NE(error.find("no report"), std::string::npos) << error;
}

namespace
{

// A router on a thread, stepping until the fixture goes away.
struct RunningRouter
{
    process_manager::ZmqContext context{};
    process_manager::MessageRouter router{context};
    std::atomic<bool> stop{false};
    std::thread loop;
    std::string endpoint; // read before the loop owns the router socket
    std::string error;

    RunningRouter()
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

TEST(ManagerClientTest, SendsThroughARouterAndReadsTheReply)
{
    RunningRouter fixture{};
    ASSERT_TRUE(fixture.error.empty()) << fixture.error;

    // A stand-in manager on the router, as RouterLink would be.
    process_manager::ZmqContext managerContext{};
    process_manager::RouterLink link{managerContext};
    std::string error{};
    ASSERT_EQ(link.Connect(fixture.endpoint, "bpm", error), process_manager::ZmqCode::Ok) << error;
    std::atomic<bool> done{false};
    std::thread answer{[&link, &done]()
                       {
                           while (!done.load())
                           {
                               std::vector<bool> ready{};
                               process_manager::PollReadable(Sockets{&link.Socket()}, std::chrono::milliseconds{50}, ready);
                               process_manager::CommandRequest request{};
                               std::string problem{};
                               if (!ready[0] || link.Receive(request, problem) != process_manager::RequestCode::Ok)
                               {
                                   continue;
                               }
                               process_manager::CommandReply reply{};
                               reply.command = request.command.command;
                               reply.serviceName = request.command.serviceName;
                               reply.message = "via router";
                               link.Reply(request, reply);
                           }
                       }};

    // The client keeps a fresh DEALER per command, so its first send can meet a
    // router that does not know the client yet; the reply wait covers that.
    process_manager::ManagerClient client{"tcp://127.0.0.1:1", "tcp://127.0.0.1:1"};
    client.UseRouter(fixture.endpoint, "bpm");
    process_manager::CommandReply reply{};
    process_manager::ClientCode code = process_manager::ClientCode::Timeout;
    for (int attempt = 0; attempt < 10 && code == process_manager::ClientCode::Timeout; ++attempt)
    {
        code = client.SendCommand(process_manager::CommandCode::Stop, "vision", std::chrono::seconds{1}, reply, error);
    }
    done.store(true);
    answer.join();
    ASSERT_EQ(code, process_manager::ClientCode::Ok) << error;
    EXPECT_EQ(reply.command, 79);
    EXPECT_EQ(reply.serviceName, "vision");
    EXPECT_EQ(reply.message, "via router");
}

TEST(ManagerClientTest, TimesOutWhenTheManagerIsNotOnTheRouter)
{
    RunningRouter fixture{};
    ASSERT_TRUE(fixture.error.empty()) << fixture.error;
    process_manager::ManagerClient client{"tcp://127.0.0.1:1", "tcp://127.0.0.1:1"};
    client.UseRouter(fixture.endpoint, "absent");
    process_manager::CommandReply reply{};
    std::string error{};
    EXPECT_EQ(client.SendCommand(process_manager::CommandCode::Stop, "x", std::chrono::milliseconds{300}, reply, error),
              process_manager::ClientCode::Timeout);
    EXPECT_NE(error.find("through the router"), std::string::npos) << error;
    EXPECT_NE(error.find("absent"), std::string::npos) << error;
}
