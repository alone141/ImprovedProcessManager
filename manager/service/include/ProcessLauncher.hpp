#pragma once

#include "ProcessTable.hpp"
#include "ServiceConfig.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace process_manager
{

// How long killed processes may take to disappear. A stop that takes longer is reported
// as stuck, and an exit is reported without waiting longer for the service's leftovers.
constexpr std::chrono::milliseconds kill_grace{5000};

struct ExitStatus
{
    int code{0};   // exit code when signal is 0
    int signal{0}; // terminating signal on POSIX, otherwise 0
};

struct UsageSample
{
    std::uint64_t cpuTimeUsec{0};
    std::uint64_t memoryBytes{0};
    std::uint64_t memoryPeakBytes{0};
    std::uint64_t ioReadBytes{0};
    std::uint64_t ioWriteBytes{0};
    std::uint32_t oomKills{0};
    int processCount{0};
    int threadCount{0};
    int openFiles{-1};
    bool cgroup{false};
};

struct LaunchRequest
{
    std::string serviceName;
    std::string binary;
    std::vector<std::string> arguments;
    std::string workingDirectory;
    std::vector<EnvironmentVariable> environment; // added to, or replacing, the manager's own
    OutputMode output{OutputMode::Auto};
    std::string outputPath;
    std::uint64_t memoryMax{0};
    std::uint32_t cpuMaxPercent{0};
};

enum class LaunchCode
{
    Ok,
    BinaryNotFound,
    WorkingDirectoryMissing,
    OutputFailed,
    SpawnFailed,
};

class ChildProcess
{
public:
    virtual ~ChildProcess() = default;

    /**
     * @brief Operating system ID of the main process.
     * @return The PID.
     */
    virtual int Pid() const = 0;

    /**
     * @brief Check, without waiting, whether the main process has exited. On exit the
     *        rest of the service's processes are killed, as systemd does for a simple unit,
     *        and the exit counts once they are gone too, or kill_grace later, so a restart
     *        never meets the old instance.
     * @return The exit status once the service is gone, otherwise std::nullopt.
     */
    virtual std::optional<ExitStatus> Poll() = 0;

    /**
     * @brief Ask the service to stop: the signal to its process group on POSIX; Ctrl+Break
     *        and WM_CLOSE on Windows.
     * @param signal Signal to send on POSIX.
     */
    virtual void RequestStop(StopSignal signal) = 0;

    /**
     * @brief Kill every process of the service at once.
     */
    virtual void Kill() = 0;

    /**
     * @brief Measure the service: all of its processes, not just the main one.
     * @param table Snapshot of every process on the host, taken for this round of samples.
     * @param out Receives the figures.
     */
    virtual void Sample(const ProcessTable& table, UsageSample& out) = 0;

    /**
     * @brief List the processes that belong to the service.
     * @param table Snapshot of every process on the host.
     * @return PIDs, the main process included while it runs.
     */
    virtual std::vector<int> MemberPids(const ProcessTable& table) const = 0;
};

class ProcessLauncher
{
public:
    virtual ~ProcessLauncher() = default;

    /**
     * @brief Start the main process of a service.
     * @param request What to run and how.
     * @param out Receives the running process on success.
     * @param error Receives a readable reason on failure.
     * @return LaunchCode::Ok once the program is running, otherwise the failed step.
     */
    virtual LaunchCode Launch(const LaunchRequest& request, std::unique_ptr<ChildProcess>& out,
                              std::string& error) = 0;
};

/**
 * @brief Tell whether a process ended cleanly.
 * @param status Exit status to check.
 * @return true for exit code 0 without a signal.
 */
bool ExitedCleanly(const ExitStatus& status);

/**
 * @brief The exit code as the reports carry it.
 * @param status Exit status to encode.
 * @return The exit code, or minus the signal number.
 */
std::int32_t WireExitCode(const ExitStatus& status);

/**
 * @brief Describe an exit status for the manager's log.
 * @param status Exit status of a process of this host.
 * @return "exit 3", "signal 9 (KILL)", or "exit 0xC0000005" for a negative Windows code.
 */
std::string DescribeExitStatus(const ExitStatus& status);

/**
 * @brief Describe an exit code taken from a report.
 * @param wireCode Exit code, or minus a POSIX signal number.
 * @param signals Whether negative codes are signals: false for a manager on Windows,
 *        whose exit codes are 32-bit status codes.
 * @return "exit 3", "signal 9 (KILL)", or "exit 0xC0000005" for a Windows status code.
 */
std::string DescribeExit(std::int32_t wireCode, bool signals);

} // namespace process_manager
