#include "Service.hpp"
#include "CommandMessage.hpp"
#include "Instant.hpp"
#include "Logger.hpp"
#include "ProcessLauncher.hpp"
#include "ServiceConfig.hpp"
#include "ServiceState.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace process_manager
{

namespace
{

std::int64_t Nanoseconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

std::string Seconds(std::chrono::milliseconds delay)
{
    const std::int64_t tenths = (delay.count() + 50) / 100;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " s";
}

std::chrono::milliseconds BackoffDelay(const ServiceConfig& config, std::size_t previousRestarts)
{
    std::chrono::milliseconds delay = config.restartDelay;
    for (std::size_t i = 0; i < previousRestarts && delay < config.restartDelayMax; ++i)
    {
        delay *= 2;
    }
    return delay < config.restartDelayMax ? delay : config.restartDelayMax;
}

} // namespace

Service::Service(ServiceConfig config, ProcessLauncher& launcher, std::vector<EnvironmentVariable> managerEnvironment)
    : config{std::move(config)}, launcher{&launcher}, managerEnvironment{std::move(managerEnvironment)}
{
}

const std::string& Service::Name() const
{
    return config.name;
}

const ServiceConfig& Service::Config() const
{
    return config;
}

ServiceState Service::State() const
{
    return state;
}

ServiceStatus Service::Status() const
{
    ServiceStatus status{};
    status.name = config.name;
    status.description = config.description;
    status.binary = config.binary;
    status.restartMode = config.restart;
    status.autostart = config.autostart;
    status.heartbeat = HeartbeatEnabled();
    status.removing = removing;
    status.memoryLimitBytes = config.memoryMax;
    status.cpuLimitPercent = config.cpuMaxPercent;
    status.state = state;
    status.pid = child != nullptr ? child->Pid() : 0;
    status.startTime = startTime;
    status.lastSeen = lastSeen;
    status.lastExitTime = lastExitTime;
    status.nextRestartTime = state == ServiceState::Backoff ? nextRestartTime : 0;
    status.lastExitCode = lastExitCode;
    status.restartCount = restartCount;
    status.missedBeats = missedBeats;
    status.usageValid = usageValid;
    status.usage = usage;
    status.cpuPercent = cpuPercent;
    status.gpuValid = gpuValid;
    status.gpuPercent = gpuPercent;
    status.gpuMemoryBytes = gpuMemoryBytes;
    status.lastError = lastError;
    return status;
}

CommandResult Service::Start(const Instant& now, bool dependenciesReady, std::string& message)
{
    switch (state)
    {
    case ServiceState::Starting:
    case ServiceState::Running:
    case ServiceState::Unhealthy:
        message = "already running";
        return CommandResult::AlreadyInState;
    case ServiceState::Stopping:
        pendingStart = true;
        pendingStartIsRestart = false;
        restartAfterStop = false;
        message = "starts again once it has stopped";
        return CommandResult::Ok;
    case ServiceState::Waiting:
        break;
    case ServiceState::Stopped:
    case ServiceState::Failed:
    case ServiceState::Backoff:
        // A start by hand gives the service a fresh restart budget.
        restartHistory.clear();
        break;
    }

    if (!dependenciesReady)
    {
        state = ServiceState::Waiting;
        message = "waiting for its dependencies";
        return CommandResult::Ok;
    }
    return Launch(now, message);
}

CommandResult Service::Stop(const Instant& now, std::string& message)
{
    switch (state)
    {
    case ServiceState::Starting:
    case ServiceState::Running:
    case ServiceState::Unhealthy:
        BeginStop(now);
        message = "stopping";
        return CommandResult::Ok;
    case ServiceState::Stopping:
        pendingStart = false;
        restartAfterStop = false;
        message = "already stopping";
        return CommandResult::AlreadyInState;
    case ServiceState::Waiting:
    case ServiceState::Backoff:
    case ServiceState::Failed:
        state = ServiceState::Stopped;
        message = "stopped";
        return CommandResult::Ok;
    case ServiceState::Stopped:
        message = "already stopped";
        return CommandResult::AlreadyInState;
    }
    return CommandResult::InvalidState;
}

CommandResult Service::Restart(const Instant& now, bool dependenciesReady, std::string& message)
{
    switch (state)
    {
    case ServiceState::Starting:
    case ServiceState::Running:
    case ServiceState::Unhealthy:
        BeginStop(now);
        pendingStart = true;
        pendingStartIsRestart = true;
        message = "restarting";
        return CommandResult::Ok;
    case ServiceState::Stopping:
        pendingStart = true;
        pendingStartIsRestart = true;
        restartAfterStop = false;
        message = "starts again once it has stopped";
        return CommandResult::Ok;
    case ServiceState::Stopped:
    case ServiceState::Waiting:
    case ServiceState::Backoff:
    case ServiceState::Failed:
        break;
    }
    return Start(now, dependenciesReady, message);
}

void Service::Kill(const Instant& now)
{
    pendingStart = false;
    restartAfterStop = false;
    if (child == nullptr)
    {
        if (state != ServiceState::Stopped)
        {
            state = ServiceState::Stopped;
        }
        return;
    }

    state = ServiceState::Stopping;
    stopDeadline = now.steady;
    child->Kill();
    killSent = true;
    killDeadline = now.steady + kill_grace;
}

CommandResult Service::Heartbeat(const Instant& now, std::string& message)
{
    if (state != ServiceState::Starting && state != ServiceState::Running && state != ServiceState::Unhealthy)
    {
        message = "not running";
        return CommandResult::InvalidState;
    }

    lastBeat = now.steady;
    lastSeen = now.wallNs;
    missedBeats = 0;
    if (state == ServiceState::Unhealthy)
    {
        LogInfo("service " + config.name + " is healthy again");
    }
    state = ServiceState::Running;
    message = "ok";
    return CommandResult::Ok;
}

void Service::Tick(const Instant& now, bool launchAllowed, bool dependenciesReady)
{
    if (child != nullptr)
    {
        const std::optional<ExitStatus> exited = child->Poll();
        if (exited.has_value())
        {
            HandleExit(now, *exited, launchAllowed, dependenciesReady);
        }
    }

    switch (state)
    {
    case ServiceState::Starting:
        if (HeartbeatEnabled())
        {
            CheckHeartbeat(now);
        }
        else
        {
            lastSeen = now.wallNs;
            if (now.steady - launchedAt >= config.startGrace)
            {
                state = ServiceState::Running;
            }
        }
        break;
    case ServiceState::Running:
    case ServiceState::Unhealthy:
        if (HeartbeatEnabled())
        {
            CheckHeartbeat(now);
        }
        else
        {
            // A reload may have switched heartbeats off while the service was unhealthy.
            state = ServiceState::Running;
            missedBeats = 0;
            lastSeen = now.wallNs;
        }
        break;
    case ServiceState::Stopping:
        if (!killSent && now.steady >= stopDeadline)
        {
            LogWarning("service " + config.name + " did not stop within " + Seconds(config.stopTimeout) +
                       "; killing it");
            child->Kill();
            killSent = true;
            killDeadline = now.steady + kill_grace;
        }
        else if (killSent && !stuckReported && now.steady >= killDeadline)
        {
            LogError("service " + config.name + " is still running " + Seconds(kill_grace) +
                     " after it was killed");
            stuckReported = true;
        }
        break;
    case ServiceState::Backoff:
        if (launchAllowed && now.steady >= restartAt)
        {
            ++restartCount;
            Relaunch(now, dependenciesReady);
        }
        break;
    case ServiceState::Stopped:
    case ServiceState::Waiting:
    case ServiceState::Failed:
        break;
    }
}

void Service::Abandon(const std::string& reason)
{
    if (state != ServiceState::Waiting)
    {
        return;
    }

    LogWarning("service " + config.name + " will not start: " + reason);
    lastError = reason;
    state = ServiceState::Failed;
}

void Service::Sample(const Instant& now, const ProcessTable& table)
{
    if (child == nullptr)
    {
        usage = UsageSample{};
        usageValid = false;
        cpuPercent = -1.0;
        hasPreviousSample = false;
        return;
    }

    UsageSample sample{};
    child->Sample(table, sample);
    cpuPercent = -1.0;
    if (hasPreviousSample && sample.cpuTimeUsec >= previousCpu && now.steady > previousSampleAt)
    {
        const double elapsedUsec = static_cast<double>(Nanoseconds(now.steady - previousSampleAt)) / 1000.0;
        cpuPercent = static_cast<double>(sample.cpuTimeUsec - previousCpu) / elapsedUsec * 100.0;
    }
    previousCpu = sample.cpuTimeUsec;
    previousSampleAt = now.steady;
    hasPreviousSample = true;
    usage = sample;
    usageValid = true;
}

void Service::SetGpuUsage(bool valid, double percent, std::uint64_t memoryBytes)
{
    gpuValid = valid;
    gpuPercent = valid ? percent : -1.0;
    gpuMemoryBytes = valid ? memoryBytes : 0;
}

std::vector<int> Service::MemberPids(const ProcessTable& table) const
{
    if (child == nullptr)
    {
        return std::vector<int>{};
    }

    return child->MemberPids(table);
}

void Service::Replace(ServiceConfig config)
{
    this->config = std::move(config);
}

void Service::SetRemoving(bool removing)
{
    this->removing = removing;
}

bool Service::Removing() const
{
    return removing;
}

CommandResult Service::Launch(const Instant& now, std::string& message)
{
    std::unique_ptr<ChildProcess> started{};
    std::string error{};
    const LaunchCode code = launcher->Launch(BuildRequest(), started, error);
    if (code != LaunchCode::Ok)
    {
        LogError("service " + config.name + " could not start: " + error);
        lastError = error;
        lastExitCode = 127;
        lastExitTime = now.wallNs;
        message = error;
        ApplyRestartPolicy(now, false);
        return CommandResult::LaunchFailed;
    }

    child = std::move(started);
    state = ServiceState::Starting;
    launchedAt = now.steady;
    lastBeat = now.steady;
    startTime = now.wallNs;
    lastSeen = now.wallNs;
    missedBeats = 0;
    killSent = false;
    stuckReported = false;
    nextRestartTime = 0;
    lastError.clear();
    hasPreviousSample = false;
    usageValid = false;
    cpuPercent = -1.0;
    LogInfo("service " + config.name + " started (pid " + std::to_string(child->Pid()) + ")");
    message = "started, pid " + std::to_string(child->Pid());
    return CommandResult::Ok;
}

void Service::Relaunch(const Instant& now, bool dependenciesReady)
{
    if (!dependenciesReady)
    {
        LogInfo("service " + config.name + " waits for its dependencies before starting again");
        state = ServiceState::Waiting;
        return;
    }

    std::string message{};
    Launch(now, message);
}

void Service::HandleExit(const Instant& now, const ExitStatus& status, bool launchAllowed, bool dependenciesReady)
{
    child.reset();
    lastExitCode = WireExitCode(status);
    lastExitTime = now.wallNs;
    usage = UsageSample{};
    usageValid = false;
    cpuPercent = -1.0;
    hasPreviousSample = false;

    if (state == ServiceState::Stopping)
    {
        LogInfo("service " + config.name + " stopped (" + DescribeExitStatus(status) + ")");
        state = ServiceState::Stopped;
        const bool restartDue = restartAfterStop;
        const bool startDue = pendingStart;
        pendingStart = false;
        restartAfterStop = false;
        if (!launchAllowed)
        {
            return;
        }
        if (restartDue)
        {
            ScheduleRestart(now);
        }
        else if (startDue)
        {
            if (pendingStartIsRestart)
            {
                ++restartCount;
            }
            Relaunch(now, dependenciesReady);
        }
        return;
    }

    const bool clean = ExitedCleanly(status);
    if (clean)
    {
        LogInfo("service " + config.name + " exited (" + DescribeExitStatus(status) + ")");
    }
    else
    {
        LogWarning("service " + config.name + " exited unexpectedly (" + DescribeExitStatus(status) + ")");
    }
    ApplyRestartPolicy(now, clean);
}

void Service::ApplyRestartPolicy(const Instant& now, bool cleanExit)
{
    const bool restart = config.restart == RestartMode::Always ||
                         (config.restart == RestartMode::OnFailure && !cleanExit);
    if (!restart)
    {
        state = cleanExit ? ServiceState::Stopped : ServiceState::Failed;
        return;
    }
    ScheduleRestart(now);
}

void Service::ScheduleRestart(const Instant& now)
{
    while (!restartHistory.empty() && now.steady - restartHistory.front() > config.restartWindow)
    {
        restartHistory.pop_front();
    }
    if (config.maxRestarts > 0 && restartHistory.size() >= static_cast<std::size_t>(config.maxRestarts))
    {
        LogError("service " + config.name + " restarted " + std::to_string(restartHistory.size()) + " times within " +
                 std::to_string(config.restartWindow.count()) + " s; giving up");
        state = ServiceState::Failed;
        return;
    }

    const std::chrono::milliseconds delay = BackoffDelay(config, restartHistory.size());
    restartHistory.push_back(now.steady);
    restartAt = now.steady + delay;
    nextRestartTime = Advance(now, delay).wallNs;
    state = ServiceState::Backoff;
    LogInfo("service " + config.name + " restarts in " + Seconds(delay));
}

void Service::BeginStop(const Instant& now)
{
    state = ServiceState::Stopping;
    pendingStart = false;
    restartAfterStop = false;
    stopDeadline = now.steady + config.stopTimeout;
    killSent = config.stopSignal == StopSignal::Kill;
    killDeadline = now.steady + kill_grace;
    stuckReported = false;
    child->RequestStop(config.stopSignal);
}

void Service::CheckHeartbeat(const Instant& now)
{
    const std::int64_t interval = config.heartbeatInterval.count();
    const std::int64_t silent =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.steady - lastBeat).count();
    missedBeats = static_cast<int>(silent / interval);
    if (missedBeats < config.heartbeatTolerance || state == ServiceState::Unhealthy)
    {
        return;
    }

    LogWarning("service " + config.name + " missed " + std::to_string(missedBeats) + " heartbeats");
    state = ServiceState::Unhealthy;
    if (config.unhealthyAction == UnhealthyAction::Restart)
    {
        // Restarted like a crash once it has stopped: with backoff, within max_restarts.
        LogWarning("service " + config.name + " is unhealthy; restarting it");
        BeginStop(now);
        restartAfterStop = true;
    }
}

bool Service::HeartbeatEnabled() const
{
    return config.heartbeatInterval.count() > 0;
}

LaunchRequest Service::BuildRequest() const
{
    LaunchRequest request{};
    request.serviceName = config.name;
    request.binary = config.binary;
    request.arguments = config.arguments;
    request.workingDirectory = config.workingDirectory;
    request.environment = managerEnvironment;
    request.environment.push_back(EnvironmentVariable{"BPM_SERVICE_NAME", config.name});
    if (HeartbeatEnabled())
    {
        request.environment.push_back(
            EnvironmentVariable{"BPM_HEARTBEAT_INTERVAL_MS", std::to_string(config.heartbeatInterval.count())});
    }
    request.environment.insert(request.environment.end(), config.environment.begin(), config.environment.end());
    request.output = config.output;
    request.outputPath = config.outputPath;
    request.memoryMax = config.memoryMax;
    request.cpuMaxPercent = config.cpuMaxPercent;
    return request;
}

} // namespace process_manager
