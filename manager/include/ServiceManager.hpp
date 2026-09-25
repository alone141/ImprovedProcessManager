#pragma once

#include "CommandMessage.hpp"
#include "GpuMonitor.hpp"
#include "Instant.hpp"
#include "ProcessLauncher.hpp"
#include "ProcessTable.hpp"
#include "Service.hpp"
#include "ServiceConfig.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace process_manager
{

struct ReloadSummary
{
    int added{0};
    int removed{0};
    int changed{0};
};

class ServiceManager
{
public:
    /**
     * @brief Create a manager without services.
     * @param launcher Starts the processes. It must outlive the manager.
     * @param managerEnvironment Variables every service gets, such as the command endpoint.
     */
    ServiceManager(ProcessLauncher& launcher, std::vector<EnvironmentVariable> managerEnvironment);

    /**
     * @brief Replace every service with stopped ones built from @p services.
     * @param services Configurations in the order they are listed.
     */
    void Load(std::span<const ServiceConfig> services);

    /**
     * @brief Start every service marked autostart, each once its dependencies run.
     * @param now Current time.
     */
    void StartAutostart(const Instant& now);

    /**
     * @brief Carry out a start, stop, restart or heartbeat command. Reload is the daemon's.
     * @param message Command from a client. The name "*" means every service.
     * @param now Current time.
     * @return The reply to send back.
     */
    CommandReply Execute(const CommandMessage& message, const Instant& now);

    /**
     * @brief Apply a reloaded configuration: new services are added (and started when
     *        autostart), dropped ones are stopped and then removed, changed ones take their
     *        new settings at their next launch.
     * @param services The new list.
     * @param now Current time.
     * @return How many services were added, removed and changed.
     */
    ReloadSummary Apply(std::span<const ServiceConfig> services, const Instant& now);

    /**
     * @brief Advance every service, start waiting ones whose dependencies run, finish
     *        removals and, during shutdown, stop services whose dependents have stopped.
     * @param now Current time.
     */
    void Tick(const Instant& now);

    /**
     * @brief Measure every running service.
     * @param now Current time.
     * @param table Snapshot of every process on the host.
     * @param gpu GPU readings per process.
     */
    void Sample(const Instant& now, const ProcessTable& table, const GpuSnapshot& gpu);

    /**
     * @brief Begin shutting down: nothing starts any more and every service is stopped,
     *        dependents before the services they depend on.
     * @param now Current time.
     */
    void StopAll(const Instant& now);

    /**
     * @brief Kill every service at once, for a second stop request or a missed deadline.
     * @param now Current time.
     */
    void KillAll(const Instant& now);

    /**
     * @brief Tell whether no service has a live process.
     * @return true when every service is stopped, failed or waiting.
     */
    bool AllStopped() const;

    /**
     * @brief Tell whether StopAll was called.
     * @return true during shutdown.
     */
    bool ShuttingDown() const;

    /**
     * @brief Snapshot every service.
     * @return Statuses in configuration order.
     */
    std::vector<ServiceStatus> Statuses() const;

    // A pointer, so callers read the stored service without copying it.
    /**
     * @brief Look up a service.
     * @param name Service name.
     * @return The service, or nullptr when there is none by that name.
     */
    const Service* Find(std::string_view name) const;

    /**
     * @brief Number of services, including ones being removed.
     * @return The count.
     */
    std::size_t Count() const;

    /**
     * @brief Hand over the names of services removed since the last call, so their
     *        task cgroups can be deleted.
     * @return The names; the list is empty afterwards.
     */
    std::vector<std::string> TakeRemoved();

private:
    Service* FindMutable(std::string_view name);
    bool DependenciesRunning(const Service& service) const;
    bool DependencyBlocked(const Service& service, std::string& reason) const;
    bool DependentsStopped(const Service& service) const;
    CommandResult StartWithDependencies(Service& service, const Instant& now, bool restart, std::string& message,
                                        std::vector<std::string>& started);
    void StartDependencies(const Service& service, const Instant& now, std::vector<std::string>& visiting,
                           std::vector<std::string>& started);
    CommandResult ExecuteOne(CommandCode code, Service& service, const Instant& now, std::string& message,
                             std::vector<std::string>& started);

    ProcessLauncher* launcher;
    std::vector<EnvironmentVariable> managerEnvironment;
    std::vector<Service> services;
    std::vector<std::string> removed;
    bool shuttingDown;
};

} // namespace process_manager
