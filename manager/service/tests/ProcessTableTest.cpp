#include <gtest/gtest.h>

#include "ProcessTable.hpp"

#include <vector>

namespace
{

process_manager::ProcessEntry Entry(int pid, int parent, int session)
{
    process_manager::ProcessEntry entry{};
    entry.pid = pid;
    entry.parentPid = parent;
    entry.processGroup = session;
    entry.session = session;
    return entry;
}

process_manager::ProcessEntry Zombie(int pid, int parent, int session)
{
    process_manager::ProcessEntry entry = Entry(pid, parent, session);
    entry.zombie = true;
    return entry;
}

} // namespace

TEST(ProcessTableTest, FindsEntries)
{
    process_manager::ProcessTable table{};
    table.Add(Entry(10, 1, 10));
    ASSERT_NE(table.Find(10), nullptr);
    EXPECT_EQ(table.Find(10)->parentPid, 1);
    EXPECT_EQ(table.Find(11), nullptr);
    EXPECT_EQ(table.Size(), 1u);
}

TEST(ProcessTableTest, MembersAreTheSessionAndEveryDescendant)
{
    process_manager::ProcessTable table{};
    table.Add(Entry(100, 1, 100));   // main process, session leader
    table.Add(Entry(101, 100, 100)); // child
    table.Add(Entry(102, 101, 100)); // grandchild
    table.Add(Entry(103, 1, 100));   // orphan that stayed in the session
    table.Add(Entry(104, 100, 104)); // child that started its own session
    table.Add(Entry(105, 104, 104)); // its child
    table.Add(Entry(200, 1, 200));   // unrelated
    const std::vector<int> expected{100, 101, 102, 103, 104, 105};
    EXPECT_EQ(table.Members(100), expected);
}

TEST(ProcessTableTest, MembersLeaveOutZombies)
{
    process_manager::ProcessTable table{};
    table.Add(Entry(100, 1, 100));
    table.Add(Zombie(101, 100, 100)); // exited, not collected yet
    table.Add(Entry(102, 100, 100));
    EXPECT_EQ(table.Members(100), (std::vector<int>{100, 102}));
}

TEST(ProcessTableTest, AnExitedLeaderLeavesItsLiveSession)
{
    process_manager::ProcessTable table{};
    table.Add(Zombie(100, 1, 100));
    table.Add(Entry(103, 1, 100)); // orphaned, still in the session
    EXPECT_EQ(table.Members(100), std::vector<int>{103});
}

TEST(ProcessTableTest, MembersOfAnInvalidLeaderAreEmpty)
{
    process_manager::ProcessTable table{};
    table.Add(Entry(1, 0, 1));
    EXPECT_TRUE(table.Members(0).empty());
    EXPECT_TRUE(table.Members(-5).empty());
}

TEST(ProcessTableTest, CaptureListsThisProcess)
{
    const process_manager::ProcessTable table = process_manager::ProcessTable::Capture();
    EXPECT_GT(table.Size(), 0u);
}
