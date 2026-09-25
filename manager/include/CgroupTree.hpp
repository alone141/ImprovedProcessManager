#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace process_manager
{

enum class CgroupCode
{
    Ok,
    Unsupported,
    NoPermission,
    Failed,
};

// Memory is not read here: services report resident memory on every platform,
// while memory.current also counts page cache.
struct CgroupStats
{
    std::uint64_t cpuUsageUsec{0};
    std::uint64_t ioReadBytes{0};
    std::uint64_t ioWriteBytes{0};
    std::uint32_t oomKills{0};
    bool hasCpu{false};
    bool hasIo{false};
};

// Each service runs in <root>/task_<name>, the layout the GUI's journal and
// cgroup PID views look for. The manager itself moves to <root>/supervisor when
// controllers have to be enabled, because cgroup v2 only enables controllers
// for the children of a cgroup that holds no processes itself; the root cgroup
// is the exception.
class CgroupTree
{
public:
    /**
     * @brief Find the cgroup v2 directory this process belongs to.
     * @param procRoot Proc file system root, usually "/proc".
     * @param directory Receives the directory, for example
     *        /sys/fs/cgroup/system.slice/berayprocessmanager.service.
     * @param error Receives the reason on failure.
     * @return CgroupCode::Ok; CgroupCode::Unsupported without a cgroup v2 hierarchy;
     *         CgroupCode::NoPermission when the directory is not writable (no delegation).
     */
    static CgroupCode Locate(const std::string& procRoot, std::string& directory, std::string& error);

    /**
     * @brief Manage task cgroups below a directory.
     * @param root The manager's own cgroup directory.
     * @param blockDevices Block devices by number, where a stacked device lists the devices
     *        below it in slaves/; tests point it at fixture files.
     */
    explicit CgroupTree(std::string root, std::string blockDevices = "/sys/dev/block");

    /**
     * @brief Enable the cpu, memory, io and pids controllers for the task cgroups, where
     *        the kernel offers them. Below the root cgroup this first moves the manager to
     *        the supervisor leaf, and only when no other process shares its cgroup.
     * @param managerPid PID of this process, the one process that is moved.
     * @param error Receives a note when controllers stay off.
     * @return CgroupCode::Ok when task cgroups can be created, with or without controllers;
     *         CgroupCode::NoPermission or CgroupCode::Failed otherwise.
     */
    CgroupCode Prepare(int managerPid, std::string& error);

    /**
     * @brief Create the task cgroup of a service. An existing one is reused.
     * @param service Service name.
     * @param error Receives the reason on failure.
     * @return CgroupCode::Ok on success.
     */
    CgroupCode CreateGroup(std::string_view service, std::string& error);

    /**
     * @brief Write memory.max and cpu.max of a task cgroup.
     * @param service Service name.
     * @param memoryMax Limit in bytes; 0 writes "max".
     * @param cpuMaxPercent Limit in percent of one core; 0 writes "max".
     * @param error Receives the reason on failure.
     * @return CgroupCode::Ok; CgroupCode::Unsupported when a requested limit needs a
     *         controller that is not enabled.
     */
    CgroupCode ApplyLimits(std::string_view service, std::uint64_t memoryMax, std::uint32_t cpuMaxPercent,
                           std::string& error);

    /**
     * @brief Read the accounting files of a task cgroup. I/O through a stacked device
     *        such as LVM, dm-crypt or md is counted on the devices below it only, since
     *        io.stat lists it at every level.
     * @param service Service name.
     * @return The figures; each has* flag says whether its file was readable.
     */
    CgroupStats ReadStats(std::string_view service) const;

    /**
     * @brief Read the processes of a task cgroup.
     * @param service Service name.
     * @return PIDs from cgroup.procs; empty when the group does not exist.
     */
    std::vector<int> ReadMembers(std::string_view service) const;

    /**
     * @brief Kill every process of a task cgroup: cgroup.kill where the kernel has it
     *        (5.14 and later), SIGKILL to each member otherwise.
     * @param service Service name.
     * @return CgroupCode::Ok when the kill was sent or the group is empty.
     */
    CgroupCode KillMembers(std::string_view service) const;

    /**
     * @brief Remove an empty task cgroup.
     * @param service Service name.
     * @return CgroupCode::Ok when it was removed or did not exist; CgroupCode::Failed while
     *         processes remain.
     */
    CgroupCode RemoveGroup(std::string_view service) const;

    /**
     * @brief List the task cgroups that exist, including ones left by an earlier run.
     * @return Service names, taken from the task_ directory names.
     */
    std::vector<std::string> ListGroups() const;

    /**
     * @brief Tell whether a controller is enabled for the task cgroups.
     * @param controller Controller name, for example "memory".
     * @return true when it is listed in cgroup.subtree_control.
     */
    bool ControllerEnabled(std::string_view controller) const;

    /**
     * @brief Directory of a task cgroup.
     * @param service Service name.
     * @return <root>/task_<service>.
     */
    std::string GroupPath(std::string_view service) const;

    /**
     * @brief cgroup.procs file of a task cgroup; a process joins by writing to it.
     * @param service Service name.
     * @return <root>/task_<service>/cgroup.procs.
     */
    std::string ProcsPath(std::string_view service) const;

    /**
     * @brief The manager's cgroup directory.
     * @return The root given to the constructor.
     */
    const std::string& Root() const;

private:
    void LoadEnabled();
    bool IsStacked(const std::string& device) const;

    std::string root;
    std::string blockDevices;
    std::vector<std::string> enabled;
};

} // namespace process_manager
