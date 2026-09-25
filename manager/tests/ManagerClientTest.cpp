#include <gtest/gtest.h>

#include "ManagerClient.hpp"
#include "CommandMessage.hpp"
#include "CommandServer.hpp"
#include "DetailedReport.hpp"
#include "ReportPublisher.hpp"
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
