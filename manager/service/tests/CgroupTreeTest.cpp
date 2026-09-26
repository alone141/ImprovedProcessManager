#include <gtest/gtest.h>

#include "CgroupTree.hpp"
#include "ProcFs.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace
{

// The kernel's cgroup files are plain files in these tests, so writes can be read back.
class FakeTree
{
public:
    explicit FakeTree(const std::string& name)
        : root{std::filesystem::temp_directory_path() / name}
    {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }

    ~FakeTree()
    {
        std::error_code ignored{};
        std::filesystem::remove_all(root, ignored);
    }

    void Write(const std::string& relative, const std::string& text) const
    {
        const std::filesystem::path path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file{path, std::ios::binary};
        file << text;
    }

    std::string Read(const std::string& relative) const
    {
        return process_manager::ReadTextFile((root / relative).string()).value_or("<missing>");
    }

    std::string Path() const
    {
        return root.string();
    }

private:
    std::filesystem::path root;
};

} // namespace

TEST(CgroupTreeTest, PrepareMovesTheManagerAndEnablesOfferedControllers)
{
    const FakeTree tree{"process_manager_cgroup_prepare"};
    tree.Write("cgroup.controllers", "cpuset cpu io memory pids\n");
    tree.Write("cgroup.subtree_control", "");
    tree.Write("cgroup.type", "domain\n");
    tree.Write("cgroup.procs", "4242\n");

    process_manager::CgroupTree cgroups{tree.Path()};
    std::string error{};
    ASSERT_EQ(cgroups.Prepare(4242, error), process_manager::CgroupCode::Ok);
    EXPECT_EQ(tree.Read("supervisor/cgroup.procs"), "4242");
    // A real kernel keeps every enabled controller; the plain file keeps the last write.
    EXPECT_EQ(tree.Read("cgroup.subtree_control"), "+pids");
}

TEST(CgroupTreeTest, PrepareLeavesATreeWithoutControllersAlone)
{
    const FakeTree tree{"process_manager_cgroup_plain"};
    tree.Write("cgroup.controllers", "");
    tree.Write("cgroup.type", "domain\n");
    tree.Write("cgroup.procs", "1\n");

    process_manager::CgroupTree cgroups{tree.Path()};
    std::string error{};
    ASSERT_EQ(cgroups.Prepare(1, error), process_manager::CgroupCode::Ok);
    EXPECT_FALSE(std::filesystem::exists(tree.Path() + "/supervisor"));
}

TEST(CgroupTreeTest, PrepareLeavesACgroupItSharesAlone)
{
    const FakeTree tree{"process_manager_cgroup_shared"};
    tree.Write("cgroup.controllers", "cpu memory\n");
    tree.Write("cgroup.subtree_control", "");
    tree.Write("cgroup.type", "domain\n");
    tree.Write("cgroup.procs", "4242\n777\n"); // 777: a login shell, say

    process_manager::CgroupTree cgroups{tree.Path()};
    std::string error{};
    ASSERT_EQ(cgroups.Prepare(4242, error), process_manager::CgroupCode::Ok);
    EXPECT_FALSE(std::filesystem::exists(tree.Path() + "/supervisor"));
    EXPECT_EQ(tree.Read("cgroup.subtree_control"), "");
    EXPECT_NE(error.find("other processes"), std::string::npos) << error;
    EXPECT_FALSE(cgroups.ControllerEnabled("memory"));
}

TEST(CgroupTreeTest, PrepareInTheRootCgroupMovesNothing)
{
    const FakeTree tree{"process_manager_cgroup_root"};
    tree.Write("cgroup.controllers", "cpu memory\n");
    tree.Write("cgroup.subtree_control", "");
    tree.Write("cgroup.procs", "1\n4242\n"); // the root has no cgroup.type

    process_manager::CgroupTree cgroups{tree.Path()};
    std::string error{};
    ASSERT_EQ(cgroups.Prepare(4242, error), process_manager::CgroupCode::Ok) << error;
    EXPECT_FALSE(std::filesystem::exists(tree.Path() + "/supervisor"));
    EXPECT_EQ(tree.Read("cgroup.subtree_control"), "+memory");
}

TEST(CgroupTreeTest, CreatesTaskGroupsNamedLikeTheGuiExpects)
{
    const FakeTree tree{"process_manager_cgroup_create"};
    process_manager::CgroupTree cgroups{tree.Path()};
    std::string error{};
    ASSERT_EQ(cgroups.CreateGroup("vision", error), process_manager::CgroupCode::Ok);
    EXPECT_TRUE(std::filesystem::is_directory(tree.Path() + "/task_vision"));
    EXPECT_EQ(cgroups.GroupPath("vision"), tree.Path() + "/task_vision");
    EXPECT_EQ(cgroups.ProcsPath("vision"), tree.Path() + "/task_vision/cgroup.procs");
    EXPECT_EQ(cgroups.CreateGroup("vision", error), process_manager::CgroupCode::Ok);
}

TEST(CgroupTreeTest, WritesLimitsWhenControllersAreEnabled)
{
    const FakeTree tree{"process_manager_cgroup_limits"};
    tree.Write("cgroup.subtree_control", "cpu memory\n");
    process_manager::CgroupTree cgroups{tree.Path()};
    std::string error{};
    ASSERT_EQ(cgroups.CreateGroup("svc", error), process_manager::CgroupCode::Ok);
    ASSERT_EQ(cgroups.ApplyLimits("svc", 512ull << 20, 150, error), process_manager::CgroupCode::Ok) << error;
    EXPECT_EQ(tree.Read("task_svc/memory.max"), "536870912");
    EXPECT_EQ(tree.Read("task_svc/cpu.max"), "150000 100000");
    ASSERT_EQ(cgroups.ApplyLimits("svc", 0, 0, error), process_manager::CgroupCode::Ok);
    EXPECT_EQ(tree.Read("task_svc/memory.max"), "max");
    EXPECT_EQ(tree.Read("task_svc/cpu.max"), "max 100000");
}

TEST(CgroupTreeTest, ReportsLimitsItCannotApply)
{
    const FakeTree tree{"process_manager_cgroup_nolimits"};
    tree.Write("cgroup.subtree_control", "");
    process_manager::CgroupTree cgroups{tree.Path()};
    std::string error{};
    ASSERT_EQ(cgroups.CreateGroup("svc", error), process_manager::CgroupCode::Ok);
    EXPECT_EQ(cgroups.ApplyLimits("svc", 1024, 0, error), process_manager::CgroupCode::Unsupported);
    EXPECT_NE(error.find("memory"), std::string::npos);
    EXPECT_EQ(cgroups.ApplyLimits("svc", 0, 0, error), process_manager::CgroupCode::Ok);
}

TEST(CgroupTreeTest, ReadsStatsAndMembers)
{
    const FakeTree tree{"process_manager_cgroup_stats"};
    tree.Write("task_svc/cpu.stat", "usage_usec 777\nuser_usec 700\n");
    tree.Write("task_svc/io.stat", "8:0 rbytes=10 wbytes=20\n");
    tree.Write("task_svc/memory.events", "oom_kill 2\n");
    tree.Write("task_svc/cgroup.procs", "12\n13\n");
    const process_manager::CgroupTree cgroups{tree.Path(), tree.Path() + "/no-devices"};

    const process_manager::CgroupStats stats = cgroups.ReadStats("svc");
    EXPECT_TRUE(stats.hasCpu);
    EXPECT_EQ(stats.cpuUsageUsec, 777u);
    EXPECT_TRUE(stats.hasIo);
    EXPECT_EQ(stats.ioReadBytes, 10u);
    EXPECT_EQ(stats.ioWriteBytes, 20u);
    EXPECT_EQ(stats.oomKills, 2u);
    EXPECT_EQ(cgroups.ReadMembers("svc"), (std::vector<int>{12, 13}));
    EXPECT_FALSE(cgroups.ReadStats("other").hasCpu);
}

#ifndef _WIN32

// Device numbers such as 8:0 are not valid file names on Windows.
TEST(CgroupTreeTest, CountsIoOnStackedDevicesOnce)
{
    const FakeTree tree{"process_manager_cgroup_stacked"};
    const FakeTree devices{"process_manager_block_devices"};
    devices.Write("253:0/slaves/sda2", ""); // LVM volume on a partition of 8:0
    devices.Write("8:0/size", "1000\n");
    tree.Write("task_svc/io.stat", "253:0 rbytes=100 wbytes=200\n8:0 rbytes=110 wbytes=210\n8:16 rbytes=1 wbytes=2\n");
    const process_manager::CgroupTree cgroups{tree.Path(), devices.Path()};

    const process_manager::CgroupStats stats = cgroups.ReadStats("svc");
    EXPECT_TRUE(stats.hasIo);
    EXPECT_EQ(stats.ioReadBytes, 111u);
    EXPECT_EQ(stats.ioWriteBytes, 212u);
}

#endif

TEST(CgroupTreeTest, ListsOnlyTaskGroups)
{
    const FakeTree tree{"process_manager_cgroup_list"};
    tree.Write("task_b/cgroup.procs", "");
    tree.Write("task_a/cgroup.procs", "");
    tree.Write("supervisor/cgroup.procs", "");
    tree.Write("task_file", "");
    const process_manager::CgroupTree cgroups{tree.Path()};
    EXPECT_EQ(cgroups.ListGroups(), (std::vector<std::string>{"a", "b"}));
}

TEST(CgroupTreeTest, RemovesEmptyGroups)
{
    const FakeTree tree{"process_manager_cgroup_remove"};
    process_manager::CgroupTree cgroups{tree.Path()};
    std::string error{};
    ASSERT_EQ(cgroups.CreateGroup("gone", error), process_manager::CgroupCode::Ok);
    EXPECT_EQ(cgroups.RemoveGroup("gone"), process_manager::CgroupCode::Ok);
    EXPECT_FALSE(std::filesystem::exists(tree.Path() + "/task_gone"));
    EXPECT_EQ(cgroups.RemoveGroup("never-there"), process_manager::CgroupCode::Ok);
}

TEST(CgroupTreeTest, KillingAnEmptyGroupSucceeds)
{
    const FakeTree tree{"process_manager_cgroup_kill"};
    tree.Write("task_idle/cgroup.procs", "");
    const process_manager::CgroupTree cgroups{tree.Path()};
    EXPECT_EQ(cgroups.KillMembers("idle"), process_manager::CgroupCode::Ok);
    EXPECT_EQ(cgroups.KillMembers("missing"), process_manager::CgroupCode::Ok);
}

TEST(CgroupTreeTest, LocateFailsWithoutACgroupHierarchy)
{
    const FakeTree tree{"process_manager_cgroup_locate"};
    tree.Write("self/mountinfo", "22 1 0:21 / /proc rw - proc proc rw\n");
    tree.Write("self/cgroup", "0::/\n");
    std::string directory{};
    std::string error{};
    EXPECT_EQ(process_manager::CgroupTree::Locate(tree.Path(), directory, error), process_manager::CgroupCode::Unsupported);
    EXPECT_FALSE(error.empty());
}
