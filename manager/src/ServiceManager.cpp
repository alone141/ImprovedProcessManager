#include "ServiceManager.hpp"
#include "CommandMessage.hpp"
#include "GpuMonitor.hpp"
#include "Instant.hpp"
#include "Logger.hpp"
#include "Service.hpp"
#include "ServiceConfig.hpp"
#include "ServiceState.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace process_manager
{

namespace
{

bool Contains(std::span<const std::string> names, std::string_view name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool Listed(std::span<const ServiceConfig> configs, std::string_view name)
{
    for (const ServiceConfig& config : configs)
    {
        if (config.name == name)
        {
            return true;
        }
    }
    return false;
}

} // namespace

ServiceManager::ServiceManager(ProcessLauncher& launcher, std::vector<EnvironmentVariable> managerEnvironment)
    : launcher{&launcher}, managerEnvironment{std::move(managerEnvironment)}, services{}, removed{},
      shuttingDown{false}
{
}

void ServiceManager::Load(std::span<const ServiceConfig> configs)
{
    services.clear();
    services.reserve(configs.size());
    for (const ServiceConfig& config : configs)
    {
        services.emplace_back(config, *launcher, managerEnvironment);
    }
}

void ServiceManager::StartAutostart(const Instant& now)
{
    for (std::size_t i = 0; i < services.size(); ++i)
    {
        Service& service = services[i];
        if (service.Config().autostart && service.State() == ServiceState::Stopped)
        {
            std::string message{};
            std::vector<std::string> started{};
            StartWithDependencies(service, now, false, message, started);
        }
    }
}

CommandReply ServiceManager::Execute(const CommandMessage& message, const Instant& now)
{
    CommandReply reply{};
    reply.command = message.command;
    reply.serviceName = message.serviceName;

    const std::optional<CommandCode> code = CommandCodeFromByte(message.command);
    if (!code.has_value() || *code == CommandCode::Reload)
    {
        reply.result = CommandResult::UnknownCommand;
        reply.message = "unknown command " + std::to_string(message.command);
        return reply;
    }
    if (shuttingDown && *code != CommandCode::Heartbeat)
    {
        reply.result = CommandResult::ShuttingDown;
        reply.message = "the manager is shutting down";
        return reply;
    }

    if (message.serviceName == all_services)
    {
        if (*code == CommandCode::Heartbeat)
        {
            reply.result = CommandResult::Malformed;
            reply.message = "a heartbeat names one service";
            return reply;
        }
        int acted = 0;
        int failed = 0;
        // Services this command already started as someone's dependency are not
        // started or restarted a second time.
        std::vector<std::string> started{};
        for (std::size_t i = 0; i < services.size(); ++i)
        {
            if (services[i].Removing())
            {
                continue;
            }
            if (Contains(started, services[i].Name()))
            {
                ++acted;
                continue;
            }
            std::string ignored{};
            const CommandResult result = ExecuteOne(*code, services[i], now, ignored, started);
            acted += result == CommandResult::Ok ? 1 : 0;
            failed += IsSuccess(result) ? 0 : 1;
        }
        reply.result = failed > 0 ? CommandResult::LaunchFailed : CommandResult::Ok;
        reply.message = std::string{CommandName(*code)} + " sent to " + std::to_string(acted) + " services" +
                        (failed > 0 ? ", " + std::to_string(failed) + " failed" : std::string{});
        return reply;
    }

    Service* service = FindMutable(message.serviceName);
    if (service == nullptr || service->Removing())
    {
        reply.result = CommandResult::UnknownService;
        reply.message = "no service named " + message.serviceName;
        return reply;
    }
    std::vector<std::string> started{};
    reply.result = ExecuteOne(*code, *service, now, reply.message, started);
    return reply;
}

ReloadSummary ServiceManager::Apply(std::span<const ServiceConfig> configs, const Instant& now)
{
    ReloadSummary summary{};
    std::vector<std::string> added{};
    for (const ServiceConfig& config : configs)
    {
        Service* existing = FindMutable(config.name);
        if (existing == nullptr)
        {
            services.emplace_back(config, *launcher, managerEnvironment);
            added.push_back(config.name);
            ++summary.added;
            continue;
        }
        const bool readded = existing->Removing();
        const bool changed = !SameServiceConfig(existing->Config(), config);
        if (changed)
        {
            existing->Replace(config);
        }
        if (readded)
        {
            // Dropped by an earlier reload and still stopping: it comes back like a new service.
            existing->SetRemoving(false);
            added.push_back(config.name);
            ++summary.added;
        }
        else if (changed)
        {
            ++summary.changed;
        }
    }

    for (std::size_t i = 0; i < services.size(); ++i)
    {
        Service& service = services[i];
        if (!Listed(configs, service.Name()) && !service.Removing())
        {
            service.SetRemoving(true);
            std::string message{};
            service.Stop(now, message);
            ++summary.removed;
        }
    }

    for (const std::string& name : added)
    {
        Service* service = FindMutable(name);
        if (service != nullptr && service->Config().autostart && !shuttingDown)
        {
            std::string message{};
            std::vector<std::string> started{};
            StartWithDependencies(*service, now, false, message, started);
        }
    }
    return summary;
}

void ServiceManager::Tick(const Instant& now)
{
    for (std::size_t i = 0; i < services.size(); ++i)
    {
        services[i].Tick(now, !shuttingDown, DependenciesRunning(services[i]));
    }

    for (std::size_t i = 0; i < services.size(); ++i)
    {
        Service& service = services[i];
        std::string message{};
        std::string reason{};
        if (!shuttingDown && service.State() == ServiceState::Waiting)
        {
            if (DependenciesRunning(service))
            {
                service.Start(now, true, message);
            }
            else if (DependencyBlocked(service, reason))
            {
                service.Abandon(reason);
            }
        }
        else if (shuttingDown && service.State() != ServiceState::Stopping && service.State() != ServiceState::Stopped &&
                 DependentsStopped(service))
        {
            service.Stop(now, message);
        }
    }

    bool finished = false;
    for (const Service& service : services)
    {
        finished = finished || (service.Removing() && !IsAlive(service.State()));
    }
    if (!finished)
    {
        return;
    }
    std::vector<Service> kept{};
    kept.reserve(services.size());
    for (std::size_t i = 0; i < services.size(); ++i)
    {
        if (services[i].Removing() && !IsAlive(services[i].State()))
        {
            LogInfo("service " + services[i].Name() + " removed");
            removed.push_back(services[i].Name());
            continue;
        }
        kept.push_back(std::move(services[i]));
    }
    services = std::move(kept);
}

void ServiceManager::Sample(const Instant& now, const ProcessTable& table, const GpuSnapshot& gpu)
{
    for (std::size_t i = 0; i < services.size(); ++i)
    {
        Service& service = services[i];
        service.Sample(now, table);
        const std::vector<int> members = service.MemberPids(table);
        if (!gpu.available || members.empty())
        {
            service.SetGpuUsage(false, -1.0, 0);
            continue;
        }

        bool listed = false;
        bool utilizationKnown = false;
        double percent = 0.0;
        std::uint64_t memory = 0;
        for (const int pid : members)
        {
            const std::unordered_map<int, GpuProcessUsage>::const_iterator usage = gpu.processes.find(pid);
            if (usage == gpu.processes.end())
            {
                continue;
            }
            listed = true;
            memory += usage->second.memoryBytes;
            if (usage->second.utilizationPercent >= 0.0)
            {
                utilizationKnown = true;
                percent += usage->second.utilizationPercent;
            }
        }
        // Absent from the GPU's process list means using none of it.
        service.SetGpuUsage(true, !listed || utilizationKnown ? percent : -1.0, memory);
    }
}

void ServiceManager::StopAll(const Instant& now)
{
    shuttingDown = true;
    for (std::size_t i = 0; i < services.size(); ++i)
    {
        Service& service = services[i];
        if (service.State() != ServiceState::Stopping && service.State() != ServiceState::Stopped &&
            DependentsStopped(service))
        {
            std::string message{};
            service.Stop(now, message);
        }
    }
}

void ServiceManager::KillAll(const Instant& now)
{
    shuttingDown = true;
    for (std::size_t i = 0; i < services.size(); ++i)
    {
        services[i].Kill(now);
    }
}

bool ServiceManager::AllStopped() const
{
    for (const Service& service : services)
    {
        if (IsAlive(service.State()))
        {
            return false;
        }
    }
    return true;
}

bool ServiceManager::ShuttingDown() const
{
    return shuttingDown;
}

std::vector<ServiceStatus> ServiceManager::Statuses() const
{
    std::vector<ServiceStatus> statuses{};
    statuses.reserve(services.size());
    for (const Service& service : services)
    {
        statuses.push_back(service.Status());
    }
    return statuses;
}

const Service* ServiceManager::Find(std::string_view name) const
{
    for (const Service& service : services)
    {
        if (service.Name() == name)
        {
            return &service;
        }
    }
    return nullptr;
}

std::size_t ServiceManager::Count() const
{
    return services.size();
}

std::vector<std::string> ServiceManager::TakeRemoved()
{
    std::vector<std::string> names = std::move(removed);
    removed.clear();
    return names;
}

Service* ServiceManager::FindMutable(std::string_view name)
{
    for (Service& service : services)
    {
        if (service.Name() == name)
        {
            return &service;
        }
    }
    return nullptr;
}

bool ServiceManager::DependenciesRunning(const Service& service) const
{
    for (const std::string& dependency : service.Config().dependsOn)
    {
        const Service* other = Find(dependency);
        if (other == nullptr || other->State() != ServiceState::Running)
        {
            return false;
        }
    }
    return true;
}

bool ServiceManager::DependencyBlocked(const Service& service, std::string& reason) const
{
    for (const std::string& dependency : service.Config().dependsOn)
    {
        const Service* other = Find(dependency);
        if (other == nullptr)
        {
            reason = "dependency " + dependency + " does not exist";
            return true;
        }
        if (other->State() == ServiceState::Failed || other->State() == ServiceState::Stopped)
        {
            reason = "dependency " + dependency + " is " + std::string{ServiceStateName(other->State())};
            return true;
        }
    }
    return false;
}

bool ServiceManager::DependentsStopped(const Service& service) const
{
    for (const Service& other : services)
    {
        if (Contains(other.Config().dependsOn, service.Name()) && IsAlive(other.State()))
        {
            return false;
        }
    }
    return true;
}

CommandResult ServiceManager::StartWithDependencies(Service& service, const Instant& now, bool restart,
                                                   std::string& message, std::vector<std::string>& started)
{
    std::vector<std::string> visiting{service.Name()};
    StartDependencies(service, now, visiting, started);
    const bool ready = DependenciesRunning(service);
    return restart ? service.Restart(now, ready, message) : service.Start(now, ready, message);
}

void ServiceManager::StartDependencies(const Service& service, const Instant& now, std::vector<std::string>& visiting,
                                       std::vector<std::string>& started)
{
    const std::vector<std::string> dependencies = service.Config().dependsOn;
    for (const std::string& name : dependencies)
    {
        Service* dependency = FindMutable(name);
        if (dependency == nullptr || Contains(visiting, name))
        {
            continue;
        }
        visiting.push_back(name);
        StartDependencies(*dependency, now, visiting, started);
        // Start handles every state: nothing to do while it runs, queued while it stops.
        const ServiceState before = dependency->State();
        std::string message{};
        dependency->Start(now, DependenciesRunning(*dependency), message);
        const bool wasUp =
            before == ServiceState::Starting || before == ServiceState::Running || before == ServiceState::Unhealthy;
        if (!wasUp && !Contains(started, name))
        {
            started.push_back(name);
        }
    }
}

CommandResult ServiceManager::ExecuteOne(CommandCode code, Service& service, const Instant& now, std::string& message,
                                         std::vector<std::string>& started)
{
    switch (code)
    {
    case CommandCode::Start:
        return StartWithDependencies(service, now, false, message, started);
    case CommandCode::Restart:
        return StartWithDependencies(service, now, true, message, started);
    case CommandCode::Stop:
        return service.Stop(now, message);
    case CommandCode::Heartbeat:
        return service.Heartbeat(now, message);
    case CommandCode::Reload:
        break;
    }
    message = "unknown command";
    return CommandResult::UnknownCommand;
}

} // namespace process_manager
