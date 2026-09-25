#pragma once

#include "CommandMessage.hpp"
#include "Instant.hpp"
#include "ProcessLauncher.hpp"
#include "ProcessTable.hpp"
#include "ServiceConfig.hpp"
#include "ServiceState.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace process_manager
{

struct ServiceStatus
{
    std::string name;
    std::string description;
    std::string binary;
    RestartMode restartMode{RestartMode::Never};
    bool autostart{false};
    bool heartbeat{false};
    bool removing{false};
    std::uint64_t memoryLimitBytes{0};
    std::uint32_t cpuLimitPercent{0};
    ServiceState state{ServiceState::Stopped};
    int pid{0};
    std::int64_t startTime{0};       // ns since the Unix epoch
    std::int64_t lastSeen{0};        // ns
    std::int64_t lastExitTime{0};    // ns; 0 before the first exit
    std::int64_t nextRestartTime{0}; // ns; 0 unless a restart is scheduled
    std::int32_t lastExitCode{0};
    int restartCount{0};
    int missedBeats{0};
    bool usageValid{false};
    UsageSample usage{};
    double cpuPercent{-1.0};
    bool gpuValid{false};
    double gpuPercent{-1.0};
    std::uint64_t gpuMemoryBytes{0};
    std::string lastError;
};

class Service
{
public:
    /**
     * @brief Create a stopped service.
     * @param config Its configuration.
     * @param launcher Starts its processes. It must outlive the service.
     * @param managerEnvironment Variables every launch gets, such as the manager's command endpoint.
     */
    Service(ServiceConfig config, ProcessLauncher& launcher, std::vector<EnvironmentVariable> managerEnvironment);

    /**
     * @brief The service name.
     * @return The name from the configuration.
     */
    const std::string& Name() const;

    /**
     * @brief The configuration in use.
     * @return The configuration; a replacement takes effect at the next launch.
     */
    const ServiceConfig& Config() const;

    /**
     * @brief The current state.
     * @return The state.
     */
    ServiceState State() const;

    /**
     * @brief A snapshot of everything the reports need.
     * @return The status.
     */
    ServiceStatus Status() const;

    /**
     * @brief Start the service, or note that it should start once its dependencies run.
     * @param now Current time.
     * @param dependenciesReady Whether every service it depends on is running.
     * @param message Receives a short description of what happened.
     * @return CommandResult::Ok when started or queued, CommandResult::AlreadyInState when it
     *         already runs, CommandResult::LaunchFailed when the program could not be started.
     */
    CommandResult Start(const Instant& now, bool dependenciesReady, std::string& message);

    /**
     * @brief Stop the service: the stop signal, then a kill after stop_timeout_ms.
     * @param now Current time.
     * @param message Receives a short description of what happened.
     * @return CommandResult::Ok when stopping or stopped, CommandResult::AlreadyInState when
     *         it is stopped or stopping already.
     */
    CommandResult Stop(const Instant& now, std::string& message);

    /**
     * @brief Stop the service and start it again; a stopped service just starts.
     * @param now Current time.
     * @param dependenciesReady Whether every service it depends on is running.
     * @param message Receives a short description of what happened.
     * @return As Start.
     */
    CommandResult Restart(const Instant& now, bool dependenciesReady, std::string& message);

    /**
     * @brief Kill the service at once, skipping the stop signal and its timeout.
     * @param now Current time.
     */
    void Kill(const Instant& now);

    /**
     * @brief Record a heartbeat from the service's own process.
     * @param now Current time.
     * @param message Receives a short description of what happened.
     * @return CommandResult::Ok, or CommandResult::InvalidState when it is not running.
     */
    CommandResult Heartbeat(const Instant& now, std::string& message);

    /**
     * @brief Advance the state machine: notice exits, apply timeouts and restart delays.
     * @param now Current time.
     * @param launchAllowed false while the manager shuts down, so no restart is launched.
     * @param dependenciesReady Whether every service it depends on is running. A restart
     *        that falls due without them waits in ServiceState::Waiting.
     */
    void Tick(const Instant& now, bool launchAllowed, bool dependenciesReady);

    /**
     * @brief Give up on a service that waits for a dependency which will not come up.
     * @param reason Why, for example "dependency broker failed". It becomes the last error.
     */
    void Abandon(const std::string& reason);

    /**
     * @brief Measure the running service and update its CPU percentage.
     * @param now Current time.
     * @param table Snapshot of every process on the host.
     */
    void Sample(const Instant& now, const ProcessTable& table);

    /**
     * @brief Store the GPU use measured for the service's processes.
     * @param valid Whether GPU figures exist on this host.
     * @param percent Utilisation summed over GPUs; negative when unknown.
     * @param memoryBytes GPU memory in use.
     */
    void SetGpuUsage(bool valid, double percent, std::uint64_t memoryBytes);

    /**
     * @brief The processes of the running service.
     * @param table Snapshot of every process on the host.
     * @return PIDs; empty while nothing runs.
     */
    std::vector<int> MemberPids(const ProcessTable& table) const;

    /**
     * @brief Swap in a new configuration. The running process keeps its settings until it
     *        is launched again; timeouts and heartbeat checks follow the new one at once.
     * @param config New configuration with the same name.
     */
    void Replace(ServiceConfig config);

    /**
     * @brief Mark the service for removal after a reload dropped it.
     * @param removing Whether it is being removed.
     */
    void SetRemoving(bool removing);

    /**
     * @brief Tell whether a reload dropped the service.
     * @return true when it is removed once stopped.
     */
    bool Removing() const;

private:
    CommandResult Launch(const Instant& now, std::string& message);
    void Relaunch(const Instant& now, bool dependenciesReady);
    void HandleExit(const Instant& now, const ExitStatus& status, bool launchAllowed, bool dependenciesReady);
    void ApplyRestartPolicy(const Instant& now, bool cleanExit);
    void ScheduleRestart(const Instant& now);
    void BeginStop(const Instant& now);
    void CheckHeartbeat(const Instant& now);
    bool HeartbeatEnabled() const;
    LaunchRequest BuildRequest() const;

    ServiceConfig config;
    ProcessLauncher* launcher;
    std::vector<EnvironmentVariable> managerEnvironment;
    std::unique_ptr<ChildProcess> child{};
    ServiceState state{ServiceState::Stopped};
    int restartCount{0};
    int missedBeats{0};
    bool pendingStart{false};
    bool pendingStartIsRestart{false};
    // Stopped for missing heartbeats: once it exits, it restarts like a failure.
    bool restartAfterStop{false};
    bool killSent{false};
    bool stuckReported{false};
    bool removing{false};
    std::chrono::steady_clock::time_point launchedAt{};
    std::chrono::steady_clock::time_point lastBeat{};
    std::chrono::steady_clock::time_point stopDeadline{};
    std::chrono::steady_clock::time_point killDeadline{};
    std::chrono::steady_clock::time_point restartAt{};
    std::deque<std::chrono::steady_clock::time_point> restartHistory{};
    std::int64_t startTime{0};
    std::int64_t lastSeen{0};
    std::int64_t lastExitTime{0};
    std::int64_t nextRestartTime{0};
    std::int32_t lastExitCode{0};
    std::string lastError{};
    UsageSample usage{};
    bool usageValid{false};
    double cpuPercent{-1.0};
    std::uint64_t previousCpu{0};
    std::chrono::steady_clock::time_point previousSampleAt{};
    bool hasPreviousSample{false};
    bool gpuValid{false};
    double gpuPercent{-1.0};
    std::uint64_t gpuMemoryBytes{0};
};

} // namespace process_manager
