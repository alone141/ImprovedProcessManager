#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace process_manager
{

struct ProcessEntry
{
    int pid{0};
    int parentPid{0};
    int processGroup{0};
    int session{0};
    std::uint64_t cpuTimeUsec{0};      // user + system time of this process
    std::uint64_t childCpuTimeUsec{0}; // time of the children it has waited for
    std::uint64_t rssBytes{0};
    int threads{0};
    bool zombie{false}; // exited; only waits for its parent to collect the status
};

class ProcessTable
{
public:
    /**
     * @brief Snapshot every process the operating system lists.
     * @return The snapshot. On Linux it is read from /proc. On Windows it comes from a
     *         Toolhelp snapshot, which has parents and thread counts but no CPU or memory.
     */
    static ProcessTable Capture();

    /**
     * @brief Add or replace one process.
     * @param entry Process to store, keyed by its PID.
     */
    void Add(const ProcessEntry& entry);

    // A pointer, so callers read the stored entry without copying it.
    /**
     * @brief Look up one process.
     * @param pid Process ID.
     * @return The entry, or nullptr when the process is not in the snapshot.
     */
    const ProcessEntry* Find(int pid) const;

    /**
     * @brief The live processes that belong to a service: the main process, every process
     *        in its session and every descendant. Zombies are left out, since they have
     *        exited already; an exited process has no children either.
     * @param leader Main process of the service. It leads its own session.
     * @return PIDs in ascending order; empty when @p leader is not positive.
     */
    std::vector<int> Members(int leader) const;

    /**
     * @brief Number of processes in the snapshot.
     * @return The count.
     */
    std::size_t Size() const;

private:
    std::unordered_map<int, ProcessEntry> entries;
};

} // namespace process_manager
