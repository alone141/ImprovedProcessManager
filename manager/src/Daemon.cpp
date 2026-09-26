#include "Daemon.hpp"
#include "CgroupTree.hpp"
#include "CommandLine.hpp"
#include "CommandMessage.hpp"
#include "CommandServer.hpp"
#include "ConfigParser.hpp"
#include "DetailedReport.hpp"
#include "GpuMonitor.hpp"
#include "Instant.hpp"
#include "Logger.hpp"
#include "NativeLauncher.hpp"
#include "ProcessTable.hpp"
#include "ReportBuilder.hpp"
#include "ReportPublisher.hpp"
#include "Service.hpp"
#include "ServiceConfig.hpp"
#include "ServiceManager.hpp"
#include "SignalWatcher.hpp"
#include "SystemMonitor.hpp"
#include "SystemdNotifier.hpp"
#include "Version.hpp"
#include "ZmqSocket.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace process_manager
{

namespace
{

constexpr std::chrono::milliseconds max_poll_wait{50};
constexpr std::chrono::milliseconds shutdown_margin{2000};
constexpr std::chrono::milliseconds max_shutdown{600000};
constexpr std::chrono::milliseconds group_removal_wait{50};
constexpr int group_removal_attempts = 20;
constexpr int max_commands_per_round = 100;

std::int32_t CurrentPid()
{
#ifdef _WIN32
    return static_cast<std::int32_t>(_getpid());
#else
    return static_cast<std::int32_t>(::getpid());
#endif
}

std::chrono::milliseconds ShutdownBudget(std::span<const ServiceConfig> services)
{
    std::chrono::milliseconds total{0};
    for (const ServiceConfig& service : services)
    {
        total += service.stopTimeout;
    }
    total += kill_grace + shutdown_margin;
    return std::min(total, max_shutdown);
}

std::string DescribeReply(const CommandReply& reply)
{
    std::string text{CommandResultName(reply.result)};
    if (!reply.message.empty())
    {
        text += " (" + reply.message + ")";
    }
    return text;
}

} // namespace

Daemon::Daemon(Config config)
    : config{std::move(config)}, startedAt{Now()}, cgroups{}, launcher{}, manager{}, gpu{}, system{},
      builder{CurrentPid(), startedAt.wallNs}, context{}, publisher{}, server{}, link{}, stopRequested{false},
      reloadRequested{false}, logLevelOverride{}, stopping{false}, shutdownDeadline{}, nextPublish{}
{
}

DaemonCode Daemon::Start(std::string& error)
{
    LogInfo(std::string{manager_name} + " " + std::string{manager_version} + " starting (pid " +
            std::to_string(CurrentPid()) + ", libzmq " + ZmqVersion() + ")");

    if (config.manager.cgroups != CgroupMode::Off)
    {
        SetUpCgroups();
        if (cgroups == nullptr && config.manager.cgroups == CgroupMode::Required)
        {
            error = "cgroups = required, but task cgroups cannot be created here";
            return DaemonCode::CgroupsUnavailable;
        }
    }
    launcher = std::make_unique<NativeLauncher>(cgroups.get());

    if (config.manager.gpu != GpuMode::Off)
    {
        std::string problem{};
        if (gpu.Open(problem) == GpuCode::Ok)
        {
            LogInfo("GPU monitoring through NVML");
        }
        else
        {
            LogInfo("no GPU figures: " + problem);
        }
    }

    publisher = std::make_unique<ReportPublisher>(context);
    if (publisher->Bind(config.manager.healthEndpoint, config.manager.reportEndpoint, error) != ZmqCode::Ok)
    {
        return DaemonCode::BindFailed;
    }
    server = std::make_unique<CommandServer>(context);
    if (server->Bind(config.manager.commandEndpoint, error) != ZmqCode::Ok)
    {
        return DaemonCode::BindFailed;
    }
    LogInfo("health reports on " + publisher->HealthEndpoint() + ", detailed reports on " +
            publisher->ReportEndpoint() + ", commands on " + server->Endpoint());

    std::vector<EnvironmentVariable> environment{
        EnvironmentVariable{"BPM_COMMAND_ENDPOINT", ConnectEndpoint(server->Endpoint())}};
    if (!config.manager.routerEndpoint.empty())
    {
        link = std::make_unique<RouterLink>(context);
        if (link->Connect(config.manager.routerEndpoint, config.manager.identity, error) != ZmqCode::Ok)
        {
            return DaemonCode::RouterFailed;
        }
        LogInfo("commands also through the router at " + link->Endpoint() + " as " + link->Identity());
        environment.push_back(EnvironmentVariable{"BPM_ROUTER_ENDPOINT", link->Endpoint()});
        environment.push_back(EnvironmentVariable{"BPM_MANAGER_IDENTITY", link->Identity()});
    }
    manager = std::make_unique<ServiceManager>(*launcher, std::move(environment));
    manager->Load(config.services);
    LogInfo(std::to_string(config.services.size()) + " services configured" +
            (config.path.empty() ? std::string{} : " in " + config.path));
    const Instant now = Now();
    manager->StartAutostart(now);
    nextPublish = now.steady;
    SystemdNotifier::Notify("READY=1");
    return DaemonCode::Ok;
}

int Daemon::Run()
{
    if (manager == nullptr || server == nullptr || publisher == nullptr)
    {
        return 1;
    }

    while (true)
    {
        Instant now = Now();
        // Both sources are read every round, so one request is never left over as a second one.
        const bool signalledStop = SignalWatcher::TakeStopRequest();
        const bool requestedStop = stopRequested.exchange(false);
        if (signalledStop || requestedStop)
        {
            if (!stopping)
            {
                BeginShutdown(now);
            }
            else
            {
                LogWarning("second stop request: killing every service now");
                manager->KillAll(now);
            }
        }
        const bool signalledReload = SignalWatcher::TakeReloadRequest();
        const bool requestedReload = reloadRequested.exchange(false);
        if (signalledReload || requestedReload)
        {
            const CommandReply reply = Reload(now);
            static_cast<void>(reply);
        }

        const std::chrono::milliseconds untilPublish =
            std::chrono::duration_cast<std::chrono::milliseconds>(nextPublish - now.steady);
        const std::chrono::milliseconds wait =
            std::clamp(untilPublish, std::chrono::milliseconds{0}, max_poll_wait);
        std::vector<ZmqSocket*> sockets{&server->Socket()};
        if (link != nullptr)
        {
            sockets.push_back(&link->Socket());
        }
        std::vector<bool> ready{};
        PollReadable(sockets, wait, ready);

        now = Now();
        if (!ready.empty() && ready[0])
        {
            HandleCommands(*server, now);
        }
        if (link != nullptr && ready.size() > 1 && ready[1])
        {
            HandleCommands(*link, now);
        }
        manager->Tick(now);
        RemoveTaskGroups(manager->TakeRemoved());

        if (now.steady >= nextPublish)
        {
            Publish(now);
            nextPublish += config.manager.publishInterval;
            if (nextPublish <= now.steady)
            {
                nextPublish = now.steady + config.manager.publishInterval;
            }
        }

        if (stopping && (manager->AllStopped() || now.steady >= shutdownDeadline))
        {
            if (!manager->AllStopped())
            {
                LogError("services were still running at the shutdown deadline; killing them");
                manager->KillAll(now);
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                manager->Tick(Now());
            }
            Publish(Now());
            break;
        }
    }

    if (cgroups != nullptr)
    {
        RemoveTaskGroups(cgroups->ListGroups());
    }
    LogInfo(std::string{manager_name} + " stopped");
    return 0;
}

void Daemon::RequestStop()
{
    stopRequested.store(true);
}

void Daemon::RequestReload()
{
    reloadRequested.store(true);
}

void Daemon::OverrideLogLevel(LogLevel level)
{
    logLevelOverride = level;
    GlobalLogger().SetLevel(level);
}

std::string Daemon::HealthEndpoint() const
{
    return publisher != nullptr ? publisher->HealthEndpoint() : std::string{};
}

std::string Daemon::ReportEndpoint() const
{
    return publisher != nullptr ? publisher->ReportEndpoint() : std::string{};
}

std::string Daemon::CommandEndpoint() const
{
    return server != nullptr ? server->Endpoint() : std::string{};
}

void Daemon::SetUpCgroups()
{
    std::string directory{};
    std::string problem{};
    if (CgroupTree::Locate("/proc", directory, problem) != CgroupCode::Ok)
    {
        LogInfo("cgroups unavailable (" + problem + "); services are tracked by session");
        return;
    }

    std::unique_ptr<CgroupTree> tree = std::make_unique<CgroupTree>(directory);
    const CgroupCode prepared = tree->Prepare(CurrentPid(), problem);
    if (prepared != CgroupCode::Ok)
    {
        LogInfo("cgroups unavailable (" + problem + "); services are tracked by session");
        return;
    }
    if (!problem.empty())
    {
        LogWarning("cgroups: " + problem);
    }
    cgroups = std::move(tree);
    LogInfo("services run in " + directory + "/task_<name>");

    const std::vector<std::string> stale = cgroups->ListGroups();
    for (const std::string& name : stale)
    {
        const std::vector<int> leftovers = cgroups->ReadMembers(name);
        if (!leftovers.empty())
        {
            LogWarning("killing " + std::to_string(leftovers.size()) + " processes left in task_" + name +
                       " by an earlier run");
        }
    }
    RemoveTaskGroups(stale);
}

void Daemon::RemoveTaskGroups(std::span<const std::string> names)
{
    if (cgroups == nullptr)
    {
        return;
    }

    for (const std::string& name : names)
    {
        cgroups->KillMembers(name);
        bool removed = false;
        for (int attempt = 0; attempt < group_removal_attempts && !removed; ++attempt)
        {
            removed = cgroups->RemoveGroup(name) == CgroupCode::Ok;
            if (!removed)
            {
                std::this_thread::sleep_for(group_removal_wait);
            }
        }
        if (!removed)
        {
            LogWarning("could not remove " + cgroups->GroupPath(name));
        }
    }
}

void Daemon::HandleCommands(CommandSource& source, const Instant& now)
{
    for (int round = 0; round < max_commands_per_round; ++round)
    {
        CommandRequest request{};
        std::string problem{};
        const RequestCode code = source.Receive(request, problem);
        if (code == RequestCode::Empty)
        {
            return;
        }

        CommandReply reply{};
        if (code == RequestCode::Malformed)
        {
            LogWarning("malformed command from " + DescribeIdentity(request.identity) + ": " + problem);
            reply.result = CommandResult::Malformed;
            reply.message = problem;
            source.Reply(request, reply);
            continue;
        }

        const std::optional<CommandCode> command = CommandCodeFromByte(request.command.command);
        if (command == CommandCode::Reload)
        {
            reply = Reload(now);
            reply.command = request.command.command;
            reply.serviceName = request.command.serviceName;
        }
        else
        {
            reply = manager->Execute(request.command, now);
        }

        const std::string summary = (command.has_value() ? std::string{CommandName(*command)}
                                                         : "command " + std::to_string(request.command.command)) +
                                    " " + request.command.serviceName + " from " +
                                    DescribeIdentity(request.identity) + ": " + DescribeReply(reply);
        if (command == CommandCode::Heartbeat && IsSuccess(reply.result))
        {
            LogDebug(summary);
        }
        else
        {
            LogInfo(summary);
        }
        source.Reply(request, reply);
    }
}

CommandReply Daemon::Reload(const Instant& now)
{
    CommandReply reply{};
    reply.command = static_cast<std::uint8_t>(CommandCode::Reload);
    if (config.path.empty())
    {
        reply.result = CommandResult::ReloadFailed;
        reply.message = "the manager was started without a configuration file";
        return reply;
    }
    if (stopping)
    {
        reply.result = CommandResult::ShuttingDown;
        reply.message = "the manager is shutting down";
        return reply;
    }

    Config fresh{};
    ConfigError problem{};
    const ConfigParser parser{};
    if (parser.ParseFile(config.path, fresh, problem) != ConfigCode::Ok)
    {
        const std::string where = problem.line > 0 ? "line " + std::to_string(problem.line) + ": " : std::string{};
        LogError("reload of " + config.path + " failed, keeping the running configuration: " + where +
                 problem.message);
        reply.result = CommandResult::ReloadFailed;
        reply.message = where + problem.message;
        return reply;
    }

    SystemdNotifier::Notify("RELOADING=1");
    const ManagerSettings& next = fresh.manager;
    if (next.healthEndpoint != config.manager.healthEndpoint || next.reportEndpoint != config.manager.reportEndpoint ||
        next.commandEndpoint != config.manager.commandEndpoint || next.routerEndpoint != config.manager.routerEndpoint ||
        next.identity != config.manager.identity || next.cgroups != config.manager.cgroups ||
        next.gpu != config.manager.gpu)
    {
        LogWarning("endpoint, router, identity, cgroups and gpu changes take effect when the manager restarts");
    }
    config.manager.publishInterval = next.publishInterval;
    config.manager.logLevel = next.logLevel;
    GlobalLogger().SetLevel(logLevelOverride.value_or(next.logLevel));

    const ReloadSummary summary = manager->Apply(fresh.services, now);
    config.services = fresh.services;
    SystemdNotifier::Notify("READY=1");

    reply.result = CommandResult::Ok;
    reply.message = std::to_string(summary.added) + " added, " + std::to_string(summary.removed) + " removed, " +
                    std::to_string(summary.changed) + " changed";
    LogInfo("configuration reloaded: " + reply.message);
    return reply;
}

void Daemon::Publish(const Instant& now)
{
    const ProcessTable table = ProcessTable::Capture();
    const GpuSnapshot gpuSnapshot = gpu.Sample();
    const SystemSnapshot systemSnapshot = system.Sample();
    manager->Sample(now, table, gpuSnapshot);

    const std::vector<ServiceStatus> statuses = manager->Statuses();
    std::uint32_t flags = 0;
    flags |= cgroups != nullptr ? report_flag_cgroups : 0u;
    flags |= gpu.Available() ? report_flag_gpu : 0u;
    flags |= stopping ? report_flag_stopping : 0u;
#ifdef _WIN32
    flags |= report_flag_windows;
#endif
    const std::uint32_t interval = static_cast<std::uint32_t>(config.manager.publishInterval.count());
    publisher->Publish(builder.BuildHealth(statuses, now.wallNs),
                       builder.BuildReport(statuses, systemSnapshot, gpuSnapshot, now.wallNs, interval, flags));
}

void Daemon::BeginShutdown(const Instant& now)
{
    stopping = true;
    shutdownDeadline = now.steady + ShutdownBudget(config.services);
    SystemdNotifier::Notify("STOPPING=1");
    LogInfo("stopping every service");
    manager->StopAll(now);
}

} // namespace process_manager
