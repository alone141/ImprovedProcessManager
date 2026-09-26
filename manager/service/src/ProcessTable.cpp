#include "ProcessTable.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace process_manager
{

void ProcessTable::Add(const ProcessEntry& entry)
{
    entries[entry.pid] = entry;
}

const ProcessEntry* ProcessTable::Find(int pid) const
{
    const std::unordered_map<int, ProcessEntry>::const_iterator found = entries.find(pid);
    return found == entries.end() ? nullptr : &found->second;
}

std::vector<int> ProcessTable::Members(int leader) const
{
    std::vector<int> members{};
    if (leader <= 0)
    {
        return members;
    }

    std::unordered_map<int, std::vector<int>> children{};
    std::set<int> found{};
    for (const std::pair<const int, ProcessEntry>& item : entries)
    {
        if (item.second.zombie)
        {
            continue;
        }
        children[item.second.parentPid].push_back(item.first);
        if (item.second.session == leader)
        {
            found.insert(item.first);
        }
    }

    std::vector<int> pending{};
    const ProcessEntry* main = Find(leader);
    if (main != nullptr && !main->zombie)
    {
        pending.push_back(leader);
    }
    for (const int pid : found)
    {
        pending.push_back(pid);
    }
    while (!pending.empty())
    {
        const int pid = pending.back();
        pending.pop_back();
        found.insert(pid);
        const std::unordered_map<int, std::vector<int>>::const_iterator kids = children.find(pid);
        if (kids == children.end())
        {
            continue;
        }
        for (const int child : kids->second)
        {
            if (found.count(child) == 0)
            {
                pending.push_back(child);
            }
        }
    }

    members.assign(found.begin(), found.end());
    return members;
}

std::size_t ProcessTable::Size() const
{
    return entries.size();
}

} // namespace process_manager
