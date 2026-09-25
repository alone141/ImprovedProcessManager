#include <gtest/gtest.h>

#include "Daemon.hpp"
#include "CommandLine.hpp"
#include "CommandMessage.hpp"
#include "DetailedReport.hpp"
#include "HealthRecord.hpp"
#include "ManagerClient.hpp"
#include "ServiceConfig.hpp"
#include "ServiceState.hpp"
#include "ZmqSocket.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace
{

using Sockets = std::vector<process_manager::ZmqSocket*>;

process_manager::ServiceConfig ShellService(const std::string& name, const std::string& command, bool autostart)
{
    process_manager::ServiceConfig service{};
    service.name = name;
#ifdef _WIN32
    service.binary = "cmd.exe";
    service.arguments = {"/d", "/c", command};
#else
    service.binary = "/bin/sh";
    service.arguments = {"-c", command};
#endif
    service.autostart = autostart;
    service.startGrace = std::chrono::milliseconds{0};
    service.stopTimeout = std::chrono::milliseconds{1000};
    service.restartDelay = std::chrono::milliseconds{100};
    service.output = process_manager::OutputMode::Null;
    return service;
}

process_manager::Config TestConfig()
{
    process_manager::Config config{};
    config.manager.healthEndpoint = "tcp://127.0.0.1:*";
    config.manager.reportEndpoint = "tcp://127.0.0.1:*";
    config.manager.commandEndpoint = "tcp://127.0.0.1:*";
    config.manager.publishInterval = std::chrono::milliseconds{100};
    config.manager.cgroups = process_manager::CgroupMode::Off;
    config.manager.gpu = process_manager::GpuMode::Off;
#ifdef _WIN32
    config.services.push_back(ShellService("sleeper", "ping -n 60 127.0.0.1 > NUL", true));
#else
    config.services.push_back(ShellService("sleeper", "exec sleep 60", true));
#endif
    config.services.push_back(ShellService("oneshot", "exit 0", false));
    return config;
}

const process_manager::ServiceRecord* FindRecord(const process_manager::DetailedReport& report, const std::string& name)
{
    for (const process_manager::ServiceRecord& service : report.services)
    {
        if (service.name == name)
        {
            return &service;
        }
    }
    return nullptr;
}

// Reads reports until one satisfies the check or the time runs out.
bool AwaitReport(process_manager::ManagerClient& client, const std::function<bool(const process_manager::DetailedReport&)>& check,
                 process_manager::DetailedReport& last)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::string error{};
        if (client.WaitForReport(std::chrono::seconds{2}, last, error) == process_manager::ClientCode::Ok && check(last))
        {
            return true;
        }
    }
    return false;
}

bool InState(const process_manager::DetailedReport& report, const std::string& name, process_manager::ServiceState state)
{
    const process_manager::ServiceRecord* service = FindRecord(report, name);
    return service != nullptr && service->state == state;
}

} // namespace

TEST(DaemonTest, ServesCommandsAndReportsUntilStopped)
{
    process_manager::Daemon daemon{TestConfig()};
    std::string error{};
    ASSERT_EQ(daemon.Start(error), process_manager::DaemonCode::Ok) << error;
    int exitCode = -1;
    std::thread loop{[&daemon, &exitCode]()
                     {
                         exitCode = daemon.Run();
                     }};

    process_manager::ManagerClient client{process_manager::ConnectEndpoint(daemon.CommandEndpoint()),
                                          process_manager::ConnectEndpoint(daemon.ReportEndpoint())};
    process_manager::DetailedReport report{};
    EXPECT_TRUE(AwaitReport(
        client,
        [](const process_manager::DetailedReport& seen)
        {
            return InState(seen, "sleeper", process_manager::ServiceState::Running) &&
                   InState(seen, "oneshot", process_manager::ServiceState::Stopped);
        },
        report));
    const process_manager::ServiceRecord* sleeper = FindRecord(report, "sleeper");
    ASSERT_NE(sleeper, nullptr);
    EXPECT_GT(sleeper->pid, 0);
    EXPECT_EQ(report.services.size(), 2u);

    process_manager::CommandReply reply{};
    ASSERT_EQ(client.SendCommand(process_manager::CommandCode::Stop, "sleeper", std::chrono::seconds{5}, reply, error),
              process_manager::ClientCode::Ok)
        << error;
    EXPECT_EQ(reply.result, process_manager::CommandResult::Ok);
    EXPECT_TRUE(AwaitReport(
        client,
        [](const process_manager::DetailedReport& seen)
        { return InState(seen, "sleeper", process_manager::ServiceState::Stopped); },
        report));

    ASSERT_EQ(client.SendCommand(process_manager::CommandCode::Start, "sleeper", std::chrono::seconds{5}, reply, error),
              process_manager::ClientCode::Ok);
    EXPECT_EQ(reply.result, process_manager::CommandResult::Ok);
    ASSERT_EQ(client.SendCommand(process_manager::CommandCode::Restart, "sleeper", std::chrono::seconds{5}, reply, error),
              process_manager::ClientCode::Ok);
    EXPECT_TRUE(AwaitReport(
        client,
        [](const process_manager::DetailedReport& seen)
        {
            const process_manager::ServiceRecord* service = FindRecord(seen, "sleeper");
            return service != nullptr && service->state == process_manager::ServiceState::Running &&
                   service->restartCount == 1;
        },
        report));

    ASSERT_EQ(client.SendCommand(process_manager::CommandCode::Start, "ghost", std::chrono::seconds{5}, reply, error),
              process_manager::ClientCode::Ok);
    EXPECT_EQ(reply.result, process_manager::CommandResult::UnknownService);
    ASSERT_EQ(client.SendCommand(process_manager::CommandCode::Reload, "", std::chrono::seconds{5}, reply, error),
              process_manager::ClientCode::Ok);
    EXPECT_EQ(reply.result, process_manager::CommandResult::ReloadFailed);

    daemon.RequestStop();
    loop.join();
    EXPECT_EQ(exitCode, 0);
}

TEST(DaemonTest, HealthStreamIsWhatTheGuiReads)
{
    process_manager::Daemon daemon{TestConfig()};
    std::string error{};
    ASSERT_EQ(daemon.Start(error), process_manager::DaemonCode::Ok) << error;
    std::thread loop{[&daemon]()
                     {
                         daemon.Run();
                     }};

    process_manager::ZmqContext context{};
    process_manager::ZmqSocket subscriber{context, process_manager::SocketType::Subscriber};
    subscriber.SetOption(process_manager::SocketOption::Subscribe, "");
    subscriber.Connect(process_manager::ConnectEndpoint(daemon.HealthEndpoint()));
    process_manager::Message message{};
    for (int attempt = 0; attempt < 100 && message.empty(); ++attempt)
    {
        std::vector<bool> ready{};
        process_manager::PollReadable(Sockets{&subscriber}, std::chrono::milliseconds{100}, ready);
        if (ready[0])
        {
            subscriber.Receive(message, true);
        }
    }
    daemon.RequestStop();
    loop.join();

    ASSERT_EQ(message.size(), 1u);
    std::vector<process_manager::HealthRecord> records{};
    ASSERT_EQ(process_manager::DecodeHealthRecords(message[0], records), process_manager::DecodeCode::Ok);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].processName, "sleeper");
    EXPECT_EQ(records[1].processName, "oneshot");
    EXPECT_EQ(records[1].state, process_manager::RuntimeState::Stopped);
}

TEST(DaemonTest, BindFailureStopsTheStart)
{
    process_manager::Config config = TestConfig();
    config.manager.commandEndpoint = "nonsense";
    process_manager::Daemon daemon{config};
    std::string error{};
    EXPECT_EQ(daemon.Start(error), process_manager::DaemonCode::BindFailed);
    EXPECT_NE(error.find("nonsense"), std::string::npos) << error;
}
