#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace process_manager
{

struct ProcessStat
{
    int pid{0};
    std::string comm;
    char state{'?'};
    int parentPid{0};
    int processGroup{0};
    int session{0};
    std::uint64_t userTicks{0};
    std::uint64_t systemTicks{0};
    std::uint64_t childUserTicks{0};
    std::uint64_t childSystemTicks{0};
    int threads{0};
    std::uint64_t startTicks{0};
    std::uint64_t rssPages{0};
};

struct CpuTimes
{
    std::uint64_t busy{0};
    std::uint64_t total{0};
};

struct MemoryInfo
{
    std::uint64_t totalBytes{0};
    std::uint64_t availableBytes{0};
};

struct LoadAverage
{
    double one{0.0};
    double five{0.0};
    double fifteen{0.0};
};

struct IoCounters
{
    std::uint64_t readBytes{0};
    std::uint64_t writeBytes{0};
};

struct DeviceIo
{
    std::string device; // MAJOR:MINOR, as io.stat names it
    std::uint64_t readBytes{0};
    std::uint64_t writeBytes{0};
};

// The parsers below return bool: callers only branch on success.

/**
 * @brief Parse /proc/PID/stat. The command name may hold spaces and parentheses.
 * @param text File contents.
 * @param out Receives the fields on success.
 * @return true when every needed field was present.
 */
bool ParseProcessStat(std::string_view text, ProcessStat& out);

/**
 * @brief Parse the aggregate "cpu" line of /proc/stat.
 * @param text File contents.
 * @param out Receives busy and total jiffies on success.
 * @return true when the line was found.
 */
bool ParseSystemCpu(std::string_view text, CpuTimes& out);

/**
 * @brief Parse /proc/meminfo.
 * @param text File contents.
 * @param out Receives total and available memory in bytes on success.
 * @return true when MemTotal was found. Without MemAvailable, free plus caches is used.
 */
bool ParseMemInfo(std::string_view text, MemoryInfo& out);

/**
 * @brief Parse /proc/loadavg.
 * @param text File contents.
 * @param out Receives the three averages on success.
 * @return true when three numbers were found.
 */
bool ParseLoadAverage(std::string_view text, LoadAverage& out);

/**
 * @brief Parse /proc/PID/io.
 * @param text File contents.
 * @param out Receives read_bytes and write_bytes on success.
 * @return true when both fields were found.
 */
bool ParseProcessIo(std::string_view text, IoCounters& out);

/**
 * @brief Parse the usage_usec line of a cgroup's cpu.stat.
 * @param text File contents.
 * @param usageUsec Receives the CPU time in microseconds on success.
 * @return true when the field was found.
 */
bool ParseCgroupCpuStat(std::string_view text, std::uint64_t& usageUsec);

/**
 * @brief Read the rbytes and wbytes of each device in a cgroup's io.stat.
 * @param text File contents. Empty means no I/O yet.
 * @return One entry per device line, in file order.
 */
std::vector<DeviceIo> ParseCgroupIoStat(std::string_view text);

/**
 * @brief Parse the oom_kill count of a cgroup's memory.events.
 * @param text File contents.
 * @param oomKills Receives the count on success.
 * @return true when the field was found.
 */
bool ParseMemoryEvents(std::string_view text, std::uint32_t& oomKills);

/**
 * @brief Find where the cgroup v2 hierarchy is mounted.
 * @param mountInfo Contents of /proc/self/mountinfo.
 * @return The mount point, or std::nullopt when no cgroup2 file system is mounted.
 */
std::optional<std::string> FindCgroup2Mount(std::string_view mountInfo);

/**
 * @brief Find this process's cgroup v2 path.
 * @param selfCgroup Contents of /proc/self/cgroup.
 * @return The path of the "0::" line, or std::nullopt without one.
 */
std::optional<std::string> FindUnifiedCgroupPath(std::string_view selfCgroup);

/**
 * @brief Make a file system path from UTF-8 text. Windows otherwise reads narrow
 *        strings in the ANSI code page, which breaks paths such as C:\Users\Şahin.
 * @param text UTF-8 path.
 * @return The path.
 */
std::filesystem::path Utf8Path(const std::string& text);

/**
 * @brief Read a whole file.
 * @param path UTF-8 path of the file to read.
 * @return The contents, or std::nullopt when the file cannot be opened.
 */
std::optional<std::string> ReadTextFile(const std::string& path);

class ProcFs
{
public:
    /**
     * @brief Read process information below a proc file system root.
     * @param root Usually "/proc"; tests point it at a directory of fixture files.
     */
    explicit ProcFs(std::string root);

    /**
     * @brief List the numeric entries of the root.
     * @return Process IDs in no particular order.
     */
    std::vector<int> ListPids() const;

    /**
     * @brief Read and parse PID/stat.
     * @param pid Process to read.
     * @param out Receives the fields on success.
     * @return true when the process exists and its stat file parsed.
     */
    bool ReadProcessStat(int pid, ProcessStat& out) const;

    /**
     * @brief Read and parse PID/io.
     * @param pid Process to read.
     * @param out Receives the counters on success.
     * @return true when the file was readable. It needs the same user or root.
     */
    bool ReadProcessIo(int pid, IoCounters& out) const;

    /**
     * @brief Count the open file descriptors of a process.
     * @param pid Process to inspect.
     * @return The count, or -1 when PID/fd cannot be listed.
     */
    int CountOpenFiles(int pid) const;

    /**
     * @brief Tell whether a process is still listed.
     * @param pid Process to look for.
     * @return true while the root has a directory for PID, zombies included.
     */
    bool ProcessExists(int pid) const;

    /**
     * @brief Read the host CPU times from stat.
     * @param out Receives busy and total jiffies on success.
     * @return true on success.
     */
    bool ReadSystemCpu(CpuTimes& out) const;

    /**
     * @brief Read the host memory from meminfo.
     * @param out Receives the sizes on success.
     * @return true on success.
     */
    bool ReadMemory(MemoryInfo& out) const;

    /**
     * @brief Read the load averages from loadavg.
     * @param out Receives the averages on success.
     * @return true on success.
     */
    bool ReadLoadAverage(LoadAverage& out) const;

    /**
     * @brief Read the host uptime from uptime.
     * @param seconds Receives whole seconds since boot on success.
     * @return true on success.
     */
    bool ReadUptime(std::uint64_t& seconds) const;

    /**
     * @brief The directory this reader is rooted at.
     * @return The root path.
     */
    const std::string& Root() const;

private:
    std::string root;
};

} // namespace process_manager
