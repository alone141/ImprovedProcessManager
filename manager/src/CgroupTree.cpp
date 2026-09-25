#include "CgroupTree.hpp"
#include "ProcFs.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// POSIX only: kill() for kernels without cgroup.kill, access() for the writability check.
#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

namespace process_manager
{

namespace
{

constexpr std::string_view task_prefix = "task_";
constexpr std::string_view supervisor_leaf = "supervisor";
constexpr std::uint64_t cpu_period_usec = 100000;
constexpr int kill_attempts = 10;
// A killed process leaves cgroup.procs only once it has exited.
constexpr std::chrono::milliseconds kill_pass_pause{2};

std::vector<std::string> Words(std::string_view text)
{
    std::vector<std::string> words{};
    std::string current{};
    for (const char c : text)
    {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
        {
            if (!current.empty())
            {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
    {
        words.push_back(current);
    }
    return words;
}

std::vector<int> ReadPids(const std::string& path)
{
    std::vector<int> pids{};
    const std::optional<std::string> text = ReadTextFile(path);
    if (!text.has_value())
    {
        return pids;
    }

    for (const std::string& word : Words(*text))
    {
        int pid = 0;
        bool digits = !word.empty();
        for (const char c : word)
        {
            digits = digits && c >= '0' && c <= '9';
            pid = digits ? pid * 10 + (c - '0') : 0;
        }
        if (digits && pid > 0)
        {
            pids.push_back(pid);
        }
    }
    return pids;
}

// Returns 0 or the errno of the failing call; cgroup files report errors on write or close.
int WriteText(const std::string& path, std::string_view text)
{
    errno = 0;
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (file == nullptr)
    {
        return errno != 0 ? errno : EIO;
    }

    int failure = 0;
    errno = 0;
    if (std::fwrite(text.data(), 1, text.size(), file) != text.size())
    {
        failure = errno != 0 ? errno : EIO;
    }
    errno = 0;
    if (std::fclose(file) != 0 && failure == 0)
    {
        failure = errno != 0 ? errno : EIO;
    }
    return failure;
}

std::string ErrorText(int error)
{
    return std::generic_category().message(error);
}

CgroupCode CodeFor(int error)
{
    return error == EACCES || error == EPERM || error == EROFS ? CgroupCode::NoPermission : CgroupCode::Failed;
}

} // namespace

CgroupCode CgroupTree::Locate(const std::string& procRoot, std::string& directory, std::string& error)
{
    const std::optional<std::string> mountInfo = ReadTextFile(procRoot + "/self/mountinfo");
    if (!mountInfo.has_value())
    {
        error = "cannot read " + procRoot + "/self/mountinfo";
        return CgroupCode::Unsupported;
    }
    const std::optional<std::string> mount = FindCgroup2Mount(*mountInfo);
    if (!mount.has_value())
    {
        error = "no cgroup v2 file system is mounted";
        return CgroupCode::Unsupported;
    }
    const std::optional<std::string> selfCgroup = ReadTextFile(procRoot + "/self/cgroup");
    const std::optional<std::string> path =
        selfCgroup.has_value() ? FindUnifiedCgroupPath(*selfCgroup) : std::optional<std::string>{};
    if (!path.has_value())
    {
        error = "this process has no cgroup v2 path";
        return CgroupCode::Unsupported;
    }

    const std::string candidate = *path == "/" ? *mount : *mount + *path;
    std::error_code status{};
    if (!std::filesystem::is_directory(candidate, status))
    {
        error = candidate + " does not exist";
        return CgroupCode::Unsupported;
    }
#ifndef _WIN32
    if (access(candidate.c_str(), W_OK) != 0 || access((candidate + "/cgroup.procs").c_str(), W_OK) != 0)
    {
        error = candidate + " is not writable (run as root, or with Delegate=yes under systemd)";
        return CgroupCode::NoPermission;
    }
#endif
    directory = candidate;
    return CgroupCode::Ok;
}

CgroupTree::CgroupTree(std::string root, std::string blockDevices)
    : root{std::move(root)}, blockDevices{std::move(blockDevices)}
{
    LoadEnabled();
}

CgroupCode CgroupTree::Prepare(int managerPid, std::string& error)
{
    const std::vector<std::string> available = Words(ReadTextFile(root + "/cgroup.controllers").value_or(""));
    std::vector<std::string> missing{};
    for (const std::string_view controller : {"cpu", "memory", "io", "pids"})
    {
        const bool offered = std::find(available.begin(), available.end(), controller) != available.end();
        if (offered && !ControllerEnabled(controller))
        {
            missing.emplace_back(controller);
        }
    }
    if (missing.empty())
    {
        return CgroupCode::Ok;
    }

    // Only the root cgroup may hold processes and still give controllers to its children.
    // It is also the only cgroup without a cgroup.type file.
    std::error_code status{};
    if (std::filesystem::exists(root + "/cgroup.type", status))
    {
        const std::vector<int> members = ReadPids(root + "/cgroup.procs");
        const bool shared = std::any_of(members.begin(), members.end(),
                                        [managerPid](int pid) { return pid != managerPid; });
        if (shared)
        {
            // A login session, for example: those processes are not the manager's to move.
            error = root + " also holds other processes, so the manager leaves its controllers off " +
                    "and memory_max and cpu_max are not applied";
            return CgroupCode::Ok;
        }
        const std::string leaf = root + "/" + std::string{supervisor_leaf};
        std::filesystem::create_directory(leaf, status);
        if (status)
        {
            error = "cannot create " + leaf + ": " + status.message();
            return CodeFor(status.value());
        }
        const int failure = WriteText(leaf + "/cgroup.procs", std::to_string(managerPid));
        if (failure != 0)
        {
            error = "cannot move the manager to " + leaf + ": " + ErrorText(failure);
            return CodeFor(failure);
        }
    }

    for (const std::string& controller : missing)
    {
        const int failure = WriteText(root + "/cgroup.subtree_control", "+" + controller);
        if (failure != 0)
        {
            error += (error.empty() ? "" : "; ") + controller + " controller: " + ErrorText(failure);
        }
    }
    LoadEnabled();
    return CgroupCode::Ok;
}

CgroupCode CgroupTree::CreateGroup(std::string_view service, std::string& error)
{
    const std::string path = GroupPath(service);
    std::error_code status{};
    std::filesystem::create_directory(path, status);
    if (status)
    {
        error = "cannot create " + path + ": " + status.message();
        return CodeFor(status.value());
    }
    return CgroupCode::Ok;
}

CgroupCode CgroupTree::ApplyLimits(std::string_view service, std::uint64_t memoryMax, std::uint32_t cpuMaxPercent,
                                   std::string& error)
{
    const std::string group = GroupPath(service);
    CgroupCode result = CgroupCode::Ok;
    if (ControllerEnabled("memory"))
    {
        const std::string value = memoryMax > 0 ? std::to_string(memoryMax) : "max";
        const int failure = WriteText(group + "/memory.max", value);
        if (failure != 0)
        {
            error = "memory.max: " + ErrorText(failure);
            result = CodeFor(failure);
        }
    }
    else if (memoryMax > 0)
    {
        error = "the memory controller is not enabled, so memory_max is not applied";
        result = CgroupCode::Unsupported;
    }

    if (ControllerEnabled("cpu"))
    {
        const std::uint64_t quota = static_cast<std::uint64_t>(cpuMaxPercent) * cpu_period_usec / 100;
        const std::string value = (cpuMaxPercent > 0 ? std::to_string(quota) : std::string{"max"}) + " " +
                                  std::to_string(cpu_period_usec);
        const int failure = WriteText(group + "/cpu.max", value);
        if (failure != 0)
        {
            error += (error.empty() ? "" : "; ") + std::string{"cpu.max: "} + ErrorText(failure);
            result = result == CgroupCode::Ok ? CodeFor(failure) : result;
        }
    }
    else if (cpuMaxPercent > 0)
    {
        error += (error.empty() ? "" : "; ") + std::string{"the cpu controller is not enabled, so cpu_max is not applied"};
        result = result == CgroupCode::Ok ? CgroupCode::Unsupported : result;
    }
    return result;
}

CgroupStats CgroupTree::ReadStats(std::string_view service) const
{
    const std::string group = GroupPath(service);
    CgroupStats stats{};
    const std::optional<std::string> cpu = ReadTextFile(group + "/cpu.stat");
    stats.hasCpu = cpu.has_value() && ParseCgroupCpuStat(*cpu, stats.cpuUsageUsec);

    const std::optional<std::string> io = ReadTextFile(group + "/io.stat");
    if (io.has_value())
    {
        stats.hasIo = true;
        for (const DeviceIo& device : ParseCgroupIoStat(*io))
        {
            if (!IsStacked(device.device))
            {
                stats.ioReadBytes += device.readBytes;
                stats.ioWriteBytes += device.writeBytes;
            }
        }
    }

    const std::optional<std::string> events = ReadTextFile(group + "/memory.events");
    if (events.has_value())
    {
        ParseMemoryEvents(*events, stats.oomKills);
    }
    return stats;
}

std::vector<int> CgroupTree::ReadMembers(std::string_view service) const
{
    return ReadPids(ProcsPath(service));
}

CgroupCode CgroupTree::KillMembers(std::string_view service) const
{
    const std::string group = GroupPath(service);
    std::error_code status{};
    if (!std::filesystem::is_directory(group, status) || ReadMembers(service).empty())
    {
        return CgroupCode::Ok;
    }
    if (std::filesystem::exists(group + "/cgroup.kill", status) && WriteText(group + "/cgroup.kill", "1") == 0)
    {
        return CgroupCode::Ok;
    }

#ifndef _WIN32
    for (int attempt = 0; attempt < kill_attempts; ++attempt)
    {
        const std::vector<int> members = ReadMembers(service);
        if (members.empty())
        {
            return CgroupCode::Ok;
        }
        for (const int pid : members)
        {
            ::kill(pid, SIGKILL);
        }
        // A pass straight after the last one would only find the same dying processes.
        std::this_thread::sleep_for(kill_pass_pause);
    }
    return ReadMembers(service).empty() ? CgroupCode::Ok : CgroupCode::Failed;
#else
    return CgroupCode::Unsupported;
#endif
}

CgroupCode CgroupTree::RemoveGroup(std::string_view service) const
{
    const std::string group = GroupPath(service);
    std::error_code status{};
    if (!std::filesystem::exists(group, status))
    {
        return CgroupCode::Ok;
    }
    std::filesystem::remove(group, status);
    return status ? CgroupCode::Failed : CgroupCode::Ok;
}

std::vector<std::string> CgroupTree::ListGroups() const
{
    std::vector<std::string> groups{};
    std::error_code status{};
    std::filesystem::directory_iterator entries{root, status};
    if (status)
    {
        return groups;
    }

    for (const std::filesystem::directory_entry& entry : entries)
    {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(status) && name.size() > task_prefix.size() &&
            name.compare(0, task_prefix.size(), task_prefix) == 0)
        {
            groups.push_back(name.substr(task_prefix.size()));
        }
    }
    std::sort(groups.begin(), groups.end());
    return groups;
}

bool CgroupTree::ControllerEnabled(std::string_view controller) const
{
    return std::find(enabled.begin(), enabled.end(), controller) != enabled.end();
}

std::string CgroupTree::GroupPath(std::string_view service) const
{
    return root + "/" + std::string{task_prefix} + std::string{service};
}

std::string CgroupTree::ProcsPath(std::string_view service) const
{
    return GroupPath(service) + "/cgroup.procs";
}

const std::string& CgroupTree::Root() const
{
    return root;
}

void CgroupTree::LoadEnabled()
{
    enabled = Words(ReadTextFile(root + "/cgroup.subtree_control").value_or(""));
}

bool CgroupTree::IsStacked(const std::string& device) const
{
    // Digits and a colon only: the name becomes part of a path.
    if (device.empty() || device.find_first_not_of("0123456789:") != std::string::npos)
    {
        return false;
    }
    std::error_code status{};
    const std::filesystem::directory_iterator slaves{blockDevices + "/" + device + "/slaves", status};
    return !status && slaves != std::filesystem::directory_iterator{};
}

} // namespace process_manager
