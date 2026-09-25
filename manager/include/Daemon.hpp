#pragma once

#include "CgroupTree.hpp"
#include "CommandServer.hpp"
#include "GpuMonitor.hpp"
#include "Instant.hpp"
#include "Logger.hpp"
#include "NativeLauncher.hpp"
#include "ReportBuilder.hpp"
#include "ReportPublisher.hpp"
#include "ServiceConfig.hpp"
#include "ServiceManager.hpp"
#include "SystemMonitor.hpp"
#include "ZmqSocket.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace process_manager
{

enum class DaemonCode
{
    Ok,
    CgroupsUnavailable,
    BindFailed,
};

class Daemon
{
public:
    /**
     * @brief Prepare a manager for a configuration; nothing runs until Start.
     * @param config Configuration, with Config::path set for reloads.
     */
    explicit Daemon(Config config);

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    /**
     * @brief Set up cgroups and GPU monitoring, bind the three sockets and start the
     *        autostart services.
     * @param error Receives the reason on failure.
     * @return DaemonCode::Ok, or the step that failed.
     */
    DaemonCode Start(std::string& error);

    /**
     * @brief Serve commands and publish reports until a stop is requested, then stop every
     *        service and return. A second stop request kills the services at once.
     * @return The process exit code: 0 after a clean shutdown.
     */
    int Run();

    /**
     * @brief Ask Run to shut down. Safe to call from any thread.
     */
    void RequestStop();

    /**
     * @brief Ask Run to re-read the configuration file. Safe to call from any thread.
     */
    void RequestReload();

    /**
     * @brief Keep a log level given on the command line over the file's log_level,
     *        including after reloads.
     * @param level Level to keep.
     */
    void OverrideLogLevel(LogLevel level);

    /**
     * @brief Where the health publisher is bound.
     * @return The endpoint with its actual port.
     */
    std::string HealthEndpoint() const;

    /**
     * @brief Where the detailed report publisher is bound.
     * @return The endpoint with its actual port.
     */
    std::string ReportEndpoint() const;

    /**
     * @brief Where the command server is bound.
     * @return The endpoint with its actual port.
     */
    std::string CommandEndpoint() const;

private:
    void SetUpCgroups();
    void RemoveTaskGroups(std::span<const std::string> names);
    void HandleCommands(const Instant& now);
    CommandReply Reload(const Instant& now);
    void Publish(const Instant& now);
    void BeginShutdown(const Instant& now);

    Config config;
    Instant startedAt;
    std::unique_ptr<CgroupTree> cgroups;
    std::unique_ptr<NativeLauncher> launcher;
    std::unique_ptr<ServiceManager> manager;
    GpuMonitor gpu;
    SystemMonitor system;
    ReportBuilder builder;
    ZmqContext context;
    std::unique_ptr<ReportPublisher> publisher;
    std::unique_ptr<CommandServer> server;
    std::atomic<bool> stopRequested;
    std::atomic<bool> reloadRequested;
    std::optional<LogLevel> logLevelOverride;
    bool stopping;
    std::chrono::steady_clock::time_point shutdownDeadline;
    std::chrono::steady_clock::time_point nextPublish;
};

} // namespace process_manager
