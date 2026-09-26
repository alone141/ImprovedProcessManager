#include <gtest/gtest.h>

#include "ServiceManager.hpp"
#include "CommandMessage.hpp"
#include "FakeLauncher.hpp"
#include "GpuMonitor.hpp"
#include "Instant.hpp"
#include "ProcessTable.hpp"
#include "ServiceConfig.hpp"
#include "ServiceState.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

using Configs = std::vector<process_manager::ServiceConfig>;

using process_manager::CommandCode;
using process_manager::CommandResult;
using process_manager::ServiceState;
using process_manager_test::FakeLauncher;

const process_manager::Instant t0{std::chrono::steady_clock::time_point{} + std::chrono::hours{1},
                                  1'700'000'000'000'000'000};

process_manager::Instant At(int ms)
{
    return process_manager::Advance(t0, std::chrono::milliseconds{ms});
}

process_manager::ServiceConfig Service(const std::string& name, bool autostart = true)
{
    process_manager::ServiceConfig config = process_manager_test::MakeService(name);
    config.autostart = autostart;
    config.startGrace = std::chrono::milliseconds{0};
    return config;
}

process_manager::CommandMessage Command(CommandCode code, const std::string& name)
{
    process_manager::CommandMessage message{};
    message.command = static_cast<std::uint8_t>(code);
    message.serviceName = name;
    return message;
}

ServiceState StateOf(const process_manager::ServiceManager& manager, const std::string& name)
{
    const process_manager::Service* service = manager.Find(name);
    return service != nullptr ? service->State() : ServiceState::Stopped;
}

} // namespace

TEST(ServiceManagerTest, AutostartStartsOnlyMarkedServices)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    manager.Load(Configs{Service("a"), Service("b", false)});
    manager.StartAutostart(At(0));
    EXPECT_EQ(StateOf(manager, "a"), ServiceState::Starting);
    EXPECT_EQ(StateOf(manager, "b"), ServiceState::Stopped);
}

TEST(ServiceManagerTest, DependenciesStartFirst)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    process_manager::ServiceConfig client = Service("client");
    client.dependsOn = {"broker"};
    manager.Load(Configs{client, Service("broker")});
    manager.StartAutostart(At(0));
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Waiting);
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Starting);
    manager.Tick(At(10));
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Running);
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Starting);
    ASSERT_EQ(launcher.requests.size(), 2u);
    EXPECT_EQ(launcher.requests[0].serviceName, "broker");
}

TEST(ServiceManagerTest, StartingADependentStartsWhatItNeeds)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    process_manager::ServiceConfig client = Service("client", false);
    client.dependsOn = {"broker"};
    manager.Load(Configs{client, Service("broker", false)});
    const process_manager::CommandReply reply = manager.Execute(Command(CommandCode::Start, "client"), At(0));
    EXPECT_EQ(reply.result, CommandResult::Ok);
    EXPECT_EQ(reply.message, "waiting for its dependencies");
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Starting);
    manager.Tick(At(10));
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Starting);
}

TEST(ServiceManagerTest, WildcardReachesEveryService)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    manager.Load(Configs{Service("a"), Service("b")});
    manager.StartAutostart(At(0));
    const process_manager::CommandReply reply = manager.Execute(Command(CommandCode::Stop, "*"), At(10));
    EXPECT_EQ(reply.result, CommandResult::Ok);
    EXPECT_EQ(reply.message, "stop sent to 2 services");
    manager.Tick(At(20));
    EXPECT_TRUE(manager.AllStopped());
}

TEST(ServiceManagerTest, ReportsUnknownNamesAndCommands)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    manager.Load(Configs{Service("a")});
    EXPECT_EQ(manager.Execute(Command(CommandCode::Start, "ghost"), At(0)).result, CommandResult::UnknownService);

    process_manager::CommandMessage unknown{};
    unknown.command = 80;
    unknown.serviceName = "a";
    const process_manager::CommandReply reply = manager.Execute(unknown, At(0));
    EXPECT_EQ(reply.result, CommandResult::UnknownCommand);
    EXPECT_EQ(reply.command, 80);
    EXPECT_EQ(reply.serviceName, "a");
    EXPECT_EQ(manager.Execute(Command(CommandCode::Heartbeat, "*"), At(0)).result, CommandResult::Malformed);
}

TEST(ServiceManagerTest, HeartbeatsReachTheService)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    process_manager::ServiceConfig beating = Service("a");
    beating.heartbeatInterval = std::chrono::milliseconds{100};
    manager.Load(Configs{beating});
    manager.StartAutostart(At(0));
    EXPECT_EQ(manager.Execute(Command(CommandCode::Heartbeat, "a"), At(50)).result, CommandResult::Ok);
    EXPECT_EQ(StateOf(manager, "a"), ServiceState::Running);
}

TEST(ServiceManagerTest, ReloadAddsRemovesAndChanges)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    manager.Load(Configs{Service("keep"), Service("drop")});
    manager.StartAutostart(At(0));

    process_manager::ServiceConfig changed = Service("keep");
    changed.binary = "/bin/other";
    const process_manager::ReloadSummary summary = manager.Apply(Configs{changed, Service("fresh")}, At(10));
    EXPECT_EQ(summary.added, 1);
    EXPECT_EQ(summary.removed, 1);
    EXPECT_EQ(summary.changed, 1);
    EXPECT_EQ(StateOf(manager, "fresh"), ServiceState::Starting);
    EXPECT_EQ(manager.Execute(Command(CommandCode::Start, "drop"), At(10)).result, CommandResult::UnknownService);

    manager.Tick(At(20));
    EXPECT_EQ(manager.Find("drop"), nullptr);
    EXPECT_EQ(manager.Count(), 2u);
    EXPECT_EQ(manager.TakeRemoved(), std::vector<std::string>{"drop"});
    EXPECT_TRUE(manager.TakeRemoved().empty());
    EXPECT_EQ(manager.Find("keep")->Config().binary, "/bin/other");
}

TEST(ServiceManagerTest, ShutdownStopsDependentsFirst)
{
    FakeLauncher launcher{};
    launcher.exitOnStop = false;
    process_manager::ServiceManager manager{launcher, {}};
    process_manager::ServiceConfig client = Service("client");
    client.dependsOn = {"broker"};
    manager.Load(Configs{client, Service("broker")});
    manager.StartAutostart(At(0));
    manager.Tick(At(10));
    ASSERT_EQ(StateOf(manager, "client"), ServiceState::Starting);

    manager.StopAll(At(100));
    EXPECT_TRUE(manager.ShuttingDown());
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Stopping);
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Running);

    launcher.processes[1]->exit = process_manager::ExitStatus{0, 15};
    manager.Tick(At(110));
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Stopped);
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Stopping);
    launcher.processes[0]->exit = process_manager::ExitStatus{0, 15};
    manager.Tick(At(120));
    EXPECT_TRUE(manager.AllStopped());
}

TEST(ServiceManagerTest, RefusesCommandsWhileShuttingDown)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    manager.Load(Configs{Service("a", false)});
    manager.StopAll(At(0));
    EXPECT_EQ(manager.Execute(Command(CommandCode::Start, "a"), At(10)).result, CommandResult::ShuttingDown);
}

TEST(ServiceManagerTest, KillAllEndsEveryService)
{
    FakeLauncher launcher{};
    launcher.exitOnStop = false;
    process_manager::ServiceManager manager{launcher, {}};
    manager.Load(Configs{Service("a"), Service("b")});
    manager.StartAutostart(At(0));
    manager.KillAll(At(10));
    manager.Tick(At(20));
    EXPECT_TRUE(manager.AllStopped());
    EXPECT_TRUE(launcher.processes[0]->killed);
    EXPECT_TRUE(launcher.processes[1]->killed);
}

TEST(ServiceManagerTest, JoinsGpuUsageByPid)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    manager.Load(Configs{Service("gpu"), Service("cpu")});
    manager.StartAutostart(At(0));

    process_manager::GpuSnapshot gpu{};
    gpu.available = true;
    gpu.processes[1000] = process_manager::GpuProcessUsage{30.0, 1ull << 30};
    manager.Sample(At(10), process_manager::ProcessTable{}, gpu);
    const std::vector<process_manager::ServiceStatus> statuses = manager.Statuses();
    ASSERT_EQ(statuses.size(), 2u);
    EXPECT_TRUE(statuses[0].gpuValid);
    EXPECT_DOUBLE_EQ(statuses[0].gpuPercent, 30.0);
    EXPECT_EQ(statuses[0].gpuMemoryBytes, 1ull << 30);
    EXPECT_TRUE(statuses[1].gpuValid);
    EXPECT_DOUBLE_EQ(statuses[1].gpuPercent, 0.0);

    manager.Sample(At(20), process_manager::ProcessTable{}, process_manager::GpuSnapshot{});
    EXPECT_FALSE(manager.Statuses()[0].gpuValid);
}

TEST(ServiceManagerTest, StartingADependentQueuesAStoppingDependency)
{
    FakeLauncher launcher{};
    launcher.exitOnStop = false;
    process_manager::ServiceManager manager{launcher, {}};
    process_manager::ServiceConfig client = Service("client", false);
    client.dependsOn = {"broker"};
    manager.Load(Configs{client, Service("broker")});
    manager.StartAutostart(At(0));
    manager.Tick(At(10));
    ASSERT_EQ(StateOf(manager, "broker"), ServiceState::Running);
    manager.Execute(Command(CommandCode::Stop, "broker"), At(20));
    ASSERT_EQ(StateOf(manager, "broker"), ServiceState::Stopping);

    manager.Execute(Command(CommandCode::Start, "client"), At(30));
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Waiting);
    launcher.processes[0]->exit = process_manager::ExitStatus{0, 15};
    manager.Tick(At(40));
    // A zero start grace makes the relaunched broker running within the same tick.
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Running);
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Starting);
    EXPECT_EQ(launcher.requests.size(), 3u);
}

TEST(ServiceManagerTest, WildcardRestartLaunchesEachServiceOnce)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    process_manager::ServiceConfig client = Service("client", false);
    client.dependsOn = {"broker"};
    manager.Load(Configs{client, Service("broker", false)});
    const process_manager::CommandReply reply = manager.Execute(Command(CommandCode::Restart, "*"), At(0));
    EXPECT_EQ(reply.message, "restart sent to 2 services");
    manager.Tick(At(10));
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Running);
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Starting);
    EXPECT_EQ(launcher.requests.size(), 2u);
}

TEST(ServiceManagerTest, CrashRestartsKeepTheDependencyOrder)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    process_manager::ServiceConfig client = Service("client");
    client.dependsOn = {"broker"};
    client.restartDelay = std::chrono::milliseconds{100};
    process_manager::ServiceConfig broker = Service("broker");
    broker.restartDelay = std::chrono::milliseconds{1000};
    manager.Load(Configs{client, broker});
    manager.StartAutostart(At(0));
    manager.Tick(At(10));
    ASSERT_EQ(StateOf(manager, "client"), ServiceState::Starting);

    launcher.processes[0]->exit = process_manager::ExitStatus{1, 0};
    launcher.processes[1]->exit = process_manager::ExitStatus{1, 0};
    manager.Tick(At(100));
    manager.Tick(At(200));
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Waiting);
    EXPECT_EQ(launcher.requests.size(), 2u);
    manager.Tick(At(1100));
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Starting);
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Waiting);
    manager.Tick(At(1110));
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Running);
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Starting);
    ASSERT_EQ(launcher.requests.size(), 4u);
    EXPECT_EQ(launcher.requests[2].serviceName, "broker");
    EXPECT_EQ(launcher.requests[3].serviceName, "client");
}

TEST(ServiceManagerTest, ServiceReaddedWhileStoppingStartsAgain)
{
    FakeLauncher launcher{};
    launcher.exitOnStop = false;
    process_manager::ServiceManager manager{launcher, {}};
    manager.Load(Configs{Service("x")});
    manager.StartAutostart(At(0));
    manager.Apply(Configs{}, At(10));
    ASSERT_EQ(StateOf(manager, "x"), ServiceState::Stopping);

    const process_manager::ReloadSummary summary = manager.Apply(Configs{Service("x")}, At(20));
    EXPECT_EQ(summary.added, 1);
    launcher.Last()->exit = process_manager::ExitStatus{0, 15};
    manager.Tick(At(30));
    ASSERT_NE(manager.Find("x"), nullptr);
    EXPECT_EQ(StateOf(manager, "x"), ServiceState::Running);
    EXPECT_EQ(launcher.requests.size(), 2u);
    EXPECT_TRUE(manager.TakeRemoved().empty());
}

TEST(ServiceManagerTest, WaitingServiceGivesUpWhenItsDependencyFails)
{
    FakeLauncher launcher{};
    process_manager::ServiceManager manager{launcher, {}};
    process_manager::ServiceConfig client = Service("client");
    client.dependsOn = {"broker"};
    process_manager::ServiceConfig broker = Service("broker");
    broker.restart = process_manager::RestartMode::Never;
    broker.startGrace = std::chrono::milliseconds{1000};
    manager.Load(Configs{client, broker});
    manager.StartAutostart(At(0));
    ASSERT_EQ(StateOf(manager, "client"), ServiceState::Waiting);

    launcher.Last()->exit = process_manager::ExitStatus{1, 0};
    manager.Tick(At(10));
    EXPECT_EQ(StateOf(manager, "broker"), ServiceState::Failed);
    EXPECT_EQ(StateOf(manager, "client"), ServiceState::Failed);
    EXPECT_EQ(manager.Find("client")->Status().lastError, "dependency broker is failed");
}
