#include <gtest/gtest.h>

#include "Service.hpp"
#include "CommandMessage.hpp"
#include "FakeLauncher.hpp"
#include "Instant.hpp"
#include "ProcessLauncher.hpp"
#include "ProcessTable.hpp"
#include "ServiceConfig.hpp"
#include "ServiceState.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace
{

using process_manager::CommandResult;
using process_manager::ServiceState;
using process_manager_test::FakeLauncher;

const process_manager::Instant t0{std::chrono::steady_clock::time_point{} + std::chrono::hours{1},
                                  1'700'000'000'000'000'000};

process_manager::Instant At(int ms)
{
    return process_manager::Advance(t0, std::chrono::milliseconds{ms});
}

process_manager::ServiceConfig Config()
{
    process_manager::ServiceConfig config = process_manager_test::MakeService("svc");
    config.startGrace = std::chrono::milliseconds{1000};
    config.restartDelay = std::chrono::milliseconds{1000};
    config.restartDelayMax = std::chrono::milliseconds{8000};
    config.stopTimeout = std::chrono::milliseconds{2000};
    return config;
}

process_manager::Service MakeService(FakeLauncher& launcher, const process_manager::ServiceConfig& config)
{
    return process_manager::Service{config, launcher, {process_manager::EnvironmentVariable{"BPM_COMMAND_ENDPOINT", "tcp://127.0.0.1:5557"}}};
}

void Exit(FakeLauncher& launcher, int code)
{
    launcher.Last()->exit = process_manager::ExitStatus{code, 0};
}

} // namespace

TEST(ServiceTest, StartLaunchesIntoStarting)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    EXPECT_EQ(service.Start(At(0), true, message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    ASSERT_EQ(launcher.requests.size(), 1u);
    EXPECT_EQ(service.Status().pid, 1000);
    EXPECT_EQ(service.Status().startTime, At(0).wallNs);
}

TEST(ServiceTest, BecomesRunningAfterTheStartGrace)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    service.Tick(At(999), true, true);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    service.Tick(At(1000), true, true);
    EXPECT_EQ(service.State(), ServiceState::Running);
}

TEST(ServiceTest, StartWhileRunningChangesNothing)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    EXPECT_EQ(service.Start(At(10), true, message), CommandResult::AlreadyInState);
    EXPECT_EQ(launcher.requests.size(), 1u);
}

TEST(ServiceTest, StopSignalsAndWaitsForTheExit)
{
    FakeLauncher launcher{};
    launcher.exitOnStop = false;
    process_manager::ServiceConfig config = Config();
    config.stopSignal = process_manager::StopSignal::Interrupt;
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);
    EXPECT_EQ(service.Stop(At(100), message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Stopping);
    EXPECT_TRUE(launcher.Last()->stopRequested);
    EXPECT_EQ(launcher.Last()->lastSignal, process_manager::StopSignal::Interrupt);
    service.Tick(At(1000), true, true);
    EXPECT_EQ(service.State(), ServiceState::Stopping);
    Exit(launcher, 0);
    service.Tick(At(1100), true, true);
    EXPECT_EQ(service.State(), ServiceState::Stopped);
    EXPECT_EQ(service.Status().pid, 0);
    EXPECT_EQ(service.Stop(At(1200), message), CommandResult::AlreadyInState);
}

TEST(ServiceTest, KillsAfterTheStopTimeout)
{
    FakeLauncher launcher{};
    launcher.exitOnStop = false;
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    service.Stop(At(100), message);
    service.Tick(At(2099), true, true);
    EXPECT_FALSE(launcher.Last()->killed);
    service.Tick(At(2100), true, true);
    EXPECT_TRUE(launcher.Last()->killed);
    service.Tick(At(2200), true, true);
    EXPECT_EQ(service.State(), ServiceState::Stopped);
    EXPECT_EQ(service.Status().lastExitCode, -9);
}

TEST(ServiceTest, FailureRestartsAfterTheDelay)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    service.Tick(At(1000), true, true);
    Exit(launcher, 3);
    service.Tick(At(5000), true, true);
    EXPECT_EQ(service.State(), ServiceState::Backoff);
    EXPECT_EQ(service.Status().lastExitCode, 3);
    EXPECT_EQ(service.Status().nextRestartTime, At(6000).wallNs);
    service.Tick(At(5999), true, true);
    EXPECT_EQ(service.State(), ServiceState::Backoff);
    service.Tick(At(6000), true, true);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    EXPECT_EQ(launcher.requests.size(), 2u);
    EXPECT_EQ(service.Status().restartCount, 1);
    EXPECT_EQ(service.Status().nextRestartTime, 0);
}

TEST(ServiceTest, BackoffDoublesUpToTheMaximum)
{
    FakeLauncher launcher{};
    process_manager::ServiceConfig config = Config();
    config.restartDelay = std::chrono::milliseconds{100};
    config.restartDelayMax = std::chrono::milliseconds{350};
    config.maxRestarts = 0;
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);

    const std::vector<int> expected{100, 200, 350, 350};
    int now = 0;
    for (const int delay : expected)
    {
        Exit(launcher, 1);
        service.Tick(At(now), true, true);
        ASSERT_EQ(service.State(), ServiceState::Backoff);
        EXPECT_EQ(service.Status().nextRestartTime, At(now + delay).wallNs);
        now += delay;
        service.Tick(At(now), true, true);
        ASSERT_EQ(service.State(), ServiceState::Starting);
    }
}

TEST(ServiceTest, GivesUpAfterMaxRestartsWithinTheWindow)
{
    FakeLauncher launcher{};
    process_manager::ServiceConfig config = Config();
    config.maxRestarts = 2;
    config.restartDelay = std::chrono::milliseconds{10};
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);
    int now = 0;
    for (int restart = 0; restart < 2; ++restart)
    {
        Exit(launcher, 1);
        service.Tick(At(now += 100), true, true);
        service.Tick(At(now += 100), true, true);
        ASSERT_EQ(service.State(), ServiceState::Starting);
    }
    Exit(launcher, 1);
    service.Tick(At(now += 100), true, true);
    EXPECT_EQ(service.State(), ServiceState::Failed);
    EXPECT_EQ(service.Status().restartCount, 2);

    EXPECT_EQ(service.Start(At(now += 100), true, message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Starting);
}

TEST(ServiceTest, RestartBudgetRefillsOutsideTheWindow)
{
    FakeLauncher launcher{};
    process_manager::ServiceConfig config = Config();
    config.maxRestarts = 1;
    config.restartWindow = std::chrono::seconds{10};
    config.restartDelay = std::chrono::milliseconds{10};
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);
    int now = 0;
    for (int restart = 0; restart < 3; ++restart)
    {
        Exit(launcher, 1);
        service.Tick(At(now += 20000), true, true);
        service.Tick(At(now += 100), true, true);
        ASSERT_EQ(service.State(), ServiceState::Starting) << "restart " << restart;
    }
}

TEST(ServiceTest, RestartModesDecideAfterAnExit)
{
    FakeLauncher launcher{};
    std::string message{};

    process_manager::ServiceConfig onFailure = Config();
    process_manager::Service clean = MakeService(launcher, onFailure);
    clean.Start(At(0), true, message);
    Exit(launcher, 0);
    clean.Tick(At(10), true, true);
    EXPECT_EQ(clean.State(), ServiceState::Stopped);

    process_manager::ServiceConfig never = Config();
    never.restart = process_manager::RestartMode::Never;
    process_manager::Service broken = MakeService(launcher, never);
    broken.Start(At(0), true, message);
    Exit(launcher, 2);
    broken.Tick(At(10), true, true);
    EXPECT_EQ(broken.State(), ServiceState::Failed);

    process_manager::ServiceConfig always = Config();
    always.restart = process_manager::RestartMode::Always;
    process_manager::Service loop = MakeService(launcher, always);
    loop.Start(At(0), true, message);
    Exit(launcher, 0);
    loop.Tick(At(10), true, true);
    EXPECT_EQ(loop.State(), ServiceState::Backoff);
}

TEST(ServiceTest, RestartStopsAndStartsAgain)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    service.Tick(At(1000), true, true);
    EXPECT_EQ(service.Restart(At(2000), true, message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Stopping);
    service.Tick(At(2010), true, true);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    EXPECT_EQ(service.Status().pid, 1001);
    EXPECT_EQ(service.Status().restartCount, 1);
}

TEST(ServiceTest, RestartOfAStoppedServiceIsAStart)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    EXPECT_EQ(service.Restart(At(0), true, message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    EXPECT_EQ(service.Status().restartCount, 0);
}

TEST(ServiceTest, StopDuringBackoffCancelsTheRestart)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    Exit(launcher, 1);
    service.Tick(At(10), true, true);
    ASSERT_EQ(service.State(), ServiceState::Backoff);
    EXPECT_EQ(service.Stop(At(20), message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Stopped);
    service.Tick(At(5000), true, true);
    EXPECT_EQ(service.State(), ServiceState::Stopped);
    EXPECT_EQ(launcher.requests.size(), 1u);
}

TEST(ServiceTest, StartDuringBackoffLaunchesAtOnce)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    Exit(launcher, 1);
    service.Tick(At(10), true, true);
    EXPECT_EQ(service.Start(At(20), true, message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    EXPECT_EQ(launcher.requests.size(), 2u);
}

TEST(ServiceTest, NoRestartIsLaunchedWhileShuttingDown)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    Exit(launcher, 1);
    service.Tick(At(10), false, true);
    service.Tick(At(60000), false, true);
    EXPECT_EQ(service.State(), ServiceState::Backoff);
    EXPECT_EQ(launcher.requests.size(), 1u);
}

TEST(ServiceTest, LaunchFailureFollowsTheRestartPolicy)
{
    FakeLauncher launcher{};
    launcher.nextCode = process_manager::LaunchCode::BinaryNotFound;
    process_manager::Service retried = MakeService(launcher, Config());
    std::string message{};
    EXPECT_EQ(retried.Start(At(0), true, message), CommandResult::LaunchFailed);
    EXPECT_EQ(message, "fake launch failure");
    EXPECT_EQ(retried.State(), ServiceState::Backoff);
    EXPECT_EQ(retried.Status().lastExitCode, 127);
    EXPECT_EQ(retried.Status().lastError, "fake launch failure");

    process_manager::ServiceConfig never = Config();
    never.restart = process_manager::RestartMode::Never;
    process_manager::Service abandoned = MakeService(launcher, never);
    EXPECT_EQ(abandoned.Start(At(0), true, message), CommandResult::LaunchFailed);
    EXPECT_EQ(abandoned.State(), ServiceState::Failed);
}

TEST(ServiceTest, WaitsForItsDependencies)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    EXPECT_EQ(service.Start(At(0), false, message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Waiting);
    EXPECT_TRUE(launcher.requests.empty());
    EXPECT_EQ(service.Start(At(10), true, message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Starting);
}

TEST(ServiceTest, HeartbeatsDecideHealth)
{
    FakeLauncher launcher{};
    process_manager::ServiceConfig config = Config();
    config.heartbeatInterval = std::chrono::milliseconds{1000};
    config.heartbeatTolerance = 3;
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);
    service.Tick(At(2000), true, true);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    EXPECT_EQ(service.Heartbeat(At(2500), message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Running);
    EXPECT_EQ(service.Status().lastSeen, At(2500).wallNs);

    service.Tick(At(4499), true, true);
    EXPECT_EQ(service.State(), ServiceState::Running);
    EXPECT_EQ(service.Status().missedBeats, 1);
    service.Tick(At(5500), true, true);
    EXPECT_EQ(service.State(), ServiceState::Unhealthy);
    EXPECT_EQ(service.Status().missedBeats, 3);

    service.Heartbeat(At(5600), message);
    EXPECT_EQ(service.State(), ServiceState::Running);
    EXPECT_EQ(service.Status().missedBeats, 0);
}

TEST(ServiceTest, NoFirstHeartbeatMeansUnhealthy)
{
    FakeLauncher launcher{};
    process_manager::ServiceConfig config = Config();
    config.heartbeatInterval = std::chrono::milliseconds{100};
    config.heartbeatTolerance = 2;
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);
    service.Tick(At(200), true, true);
    EXPECT_EQ(service.State(), ServiceState::Unhealthy);
}

TEST(ServiceTest, UnhealthyServicesCanBeRestarted)
{
    FakeLauncher launcher{};
    process_manager::ServiceConfig config = Config();
    config.heartbeatInterval = std::chrono::milliseconds{100};
    config.heartbeatTolerance = 2;
    config.unhealthyAction = process_manager::UnhealthyAction::Restart;
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);
    service.Tick(At(200), true, true);
    EXPECT_EQ(service.State(), ServiceState::Stopping);
    service.Tick(At(210), true, true);
    EXPECT_EQ(service.State(), ServiceState::Backoff);
    service.Tick(At(1210), true, true);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    EXPECT_EQ(service.Status().restartCount, 1);
}

TEST(ServiceTest, HeartbeatOfAStoppedServiceIsRejected)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    EXPECT_EQ(service.Heartbeat(At(0), message), CommandResult::InvalidState);
}

TEST(ServiceTest, StartWhileStoppingStartsOnceStopped)
{
    FakeLauncher launcher{};
    launcher.exitOnStop = false;
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    service.Stop(At(10), message);
    EXPECT_EQ(service.Start(At(20), true, message), CommandResult::Ok);
    Exit(launcher, 0);
    service.Tick(At(30), true, true);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    EXPECT_EQ(launcher.requests.size(), 2u);
}

TEST(ServiceTest, KillSkipsTheStopSignal)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    service.Kill(At(10));
    EXPECT_TRUE(launcher.Last()->killed);
    EXPECT_FALSE(launcher.Last()->stopRequested);
    service.Tick(At(20), true, true);
    EXPECT_EQ(service.State(), ServiceState::Stopped);
}

TEST(ServiceTest, SamplesCpuPercentBetweenReadings)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    const process_manager::ProcessTable table{};
    launcher.Last()->usage.cpuTimeUsec = 1'000'000;
    launcher.Last()->usage.memoryBytes = 4096;
    service.Sample(At(0), table);
    EXPECT_LT(service.Status().cpuPercent, 0.0);
    EXPECT_TRUE(service.Status().usageValid);
    launcher.Last()->usage.cpuTimeUsec = 1'500'000;
    service.Sample(At(1000), table);
    EXPECT_NEAR(service.Status().cpuPercent, 50.0, 0.001);
    EXPECT_EQ(service.Status().usage.memoryBytes, 4096u);
}

TEST(ServiceTest, LaunchesWithTheManagerEnvironment)
{
    FakeLauncher launcher{};
    process_manager::ServiceConfig config = Config();
    config.environment.push_back(process_manager::EnvironmentVariable{"OWN", "1"});
    config.heartbeatInterval = std::chrono::milliseconds{250};
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);
    const std::vector<process_manager::EnvironmentVariable>& environment = launcher.requests[0].environment;
    ASSERT_EQ(environment.size(), 4u);
    EXPECT_EQ(environment[0].name, "BPM_COMMAND_ENDPOINT");
    EXPECT_EQ(environment[1].name, "BPM_SERVICE_NAME");
    EXPECT_EQ(environment[1].value, "svc");
    EXPECT_EQ(environment[2].name, "BPM_HEARTBEAT_INTERVAL_MS");
    EXPECT_EQ(environment[2].value, "250");
    EXPECT_EQ(environment[3].name, "OWN");
}

TEST(ServiceTest, ReplacementAppliesAtTheNextLaunch)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    process_manager::ServiceConfig next = Config();
    next.binary = "/bin/new";
    service.Replace(next);
    EXPECT_EQ(launcher.requests[0].binary, "/bin/svc");
    service.Restart(At(10), true, message);
    service.Tick(At(20), true, true);
    ASSERT_EQ(launcher.requests.size(), 2u);
    EXPECT_EQ(launcher.requests[1].binary, "/bin/new");
}

TEST(ServiceTest, QueuedRestartIsDroppedWhenTheManagerShutsDown)
{
    FakeLauncher launcher{};
    launcher.exitOnStop = false;
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    service.Restart(At(10), true, message);
    Exit(launcher, 0);
    service.Tick(At(20), false, true);
    EXPECT_EQ(service.State(), ServiceState::Stopped);
    EXPECT_EQ(launcher.requests.size(), 1u);
}

TEST(ServiceTest, RestartsWaitForTheirDependencies)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    Exit(launcher, 1);
    service.Tick(At(10), true, true);
    ASSERT_EQ(service.State(), ServiceState::Backoff);
    service.Tick(At(1010), true, false);
    EXPECT_EQ(service.State(), ServiceState::Waiting);
    EXPECT_EQ(service.Status().restartCount, 1);
    EXPECT_EQ(launcher.requests.size(), 1u);

    EXPECT_EQ(service.Start(At(1100), true, message), CommandResult::Ok);
    EXPECT_EQ(service.State(), ServiceState::Starting);
    EXPECT_EQ(service.Status().restartCount, 1);
}

TEST(ServiceTest, QueuedRestartWaitsForDependencies)
{
    FakeLauncher launcher{};
    process_manager::Service service = MakeService(launcher, Config());
    std::string message{};
    service.Start(At(0), true, message);
    service.Restart(At(10), true, message);
    service.Tick(At(20), true, false);
    EXPECT_EQ(service.State(), ServiceState::Waiting);
    EXPECT_EQ(service.Status().restartCount, 1);
}

TEST(ServiceTest, HeartbeatRestartsCountTowardTheLimit)
{
    FakeLauncher launcher{};
    process_manager::ServiceConfig config = Config();
    config.heartbeatInterval = std::chrono::milliseconds{100};
    config.heartbeatTolerance = 1;
    config.unhealthyAction = process_manager::UnhealthyAction::Restart;
    config.maxRestarts = 2;
    config.restartDelay = std::chrono::milliseconds{10};
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);
    int now = 0;
    for (int round = 0; round < 2; ++round)
    {
        service.Tick(At(now += 100), true, true);
        ASSERT_EQ(service.State(), ServiceState::Stopping);
        service.Tick(At(now += 1), true, true);
        ASSERT_EQ(service.State(), ServiceState::Backoff);
        service.Tick(At(now += 100), true, true);
        ASSERT_EQ(service.State(), ServiceState::Starting);
    }
    service.Tick(At(now += 100), true, true);
    service.Tick(At(now += 1), true, true);
    EXPECT_EQ(service.State(), ServiceState::Failed);
    EXPECT_EQ(service.Status().restartCount, 2);
}

TEST(ServiceTest, SwitchingHeartbeatsOffClearsUnhealthy)
{
    FakeLauncher launcher{};
    process_manager::ServiceConfig config = Config();
    config.heartbeatInterval = std::chrono::milliseconds{100};
    config.heartbeatTolerance = 1;
    process_manager::Service service = MakeService(launcher, config);
    std::string message{};
    service.Start(At(0), true, message);
    service.Tick(At(100), true, true);
    ASSERT_EQ(service.State(), ServiceState::Unhealthy);
    service.Replace(Config());
    service.Tick(At(200), true, true);
    EXPECT_EQ(service.State(), ServiceState::Running);
    EXPECT_EQ(service.Status().missedBeats, 0);
}

TEST(ServiceTest, AbandonFailsOnlyAWaitingService)
{
    FakeLauncher launcher{};
    process_manager::Service waiting = MakeService(launcher, Config());
    std::string message{};
    waiting.Start(At(0), false, message);
    waiting.Abandon("dependency broker failed");
    EXPECT_EQ(waiting.State(), ServiceState::Failed);
    EXPECT_EQ(waiting.Status().lastError, "dependency broker failed");

    process_manager::Service running = MakeService(launcher, Config());
    running.Start(At(0), true, message);
    running.Abandon("no");
    EXPECT_EQ(running.State(), ServiceState::Starting);
}
