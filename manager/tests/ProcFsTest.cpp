#include <gtest/gtest.h>

#include "ProcFs.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace
{

// A real line from /proc/PID/stat, with a command name that holds a space and a parenthesis.
const char* const stat_line =
    "4242 (my (odd) app) S 1 4242 4242 0 -1 4194560 1234 0 0 0 150 50 7 3 20 0 5 0 987654 "
    "123456789 2048 18446744073709551615 1 1 0 0 0 0 0 0 0 0 0 0 17 3 0 0 0 0 0\n";

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file << text;
}

// "Sahin" with a cedilla, spelled in UTF-8 bytes.
std::string Sahin()
{
    return std::string{"\xC5\x9F"} + "ahin";
}

} // namespace

TEST(ProcFsTest, ParsesProcessStatWithAnAwkwardName)
{
    process_manager::ProcessStat stat{};
    ASSERT_TRUE(process_manager::ParseProcessStat(stat_line, stat));
    EXPECT_EQ(stat.pid, 4242);
    EXPECT_EQ(stat.comm, "my (odd) app");
    EXPECT_EQ(stat.state, 'S');
    EXPECT_EQ(stat.parentPid, 1);
    EXPECT_EQ(stat.processGroup, 4242);
    EXPECT_EQ(stat.session, 4242);
    EXPECT_EQ(stat.userTicks, 150u);
    EXPECT_EQ(stat.systemTicks, 50u);
    EXPECT_EQ(stat.childUserTicks, 7u);
    EXPECT_EQ(stat.childSystemTicks, 3u);
    EXPECT_EQ(stat.threads, 5);
    EXPECT_EQ(stat.startTicks, 987654u);
    EXPECT_EQ(stat.rssPages, 2048u);
}

TEST(ProcFsTest, RejectsTruncatedStat)
{
    process_manager::ProcessStat stat{};
    EXPECT_FALSE(process_manager::ParseProcessStat("12 (x) S 1 2", stat));
    EXPECT_FALSE(process_manager::ParseProcessStat("garbage", stat));
}

TEST(ProcFsTest, ParsesSystemCpu)
{
    process_manager::CpuTimes times{};
    ASSERT_TRUE(process_manager::ParseSystemCpu("cpu  100 5 50 800 20 3 2 1 0 0\ncpu0 1 2 3 4\n", times));
    EXPECT_EQ(times.busy, 161u);
    EXPECT_EQ(times.total, 981u);
    EXPECT_FALSE(process_manager::ParseSystemCpu("intr 1 2 3\n", times));
}

TEST(ProcFsTest, ParsesMemInfo)
{
    process_manager::MemoryInfo memory{};
    ASSERT_TRUE(process_manager::ParseMemInfo("MemTotal:  1000 kB\nMemFree: 100 kB\nMemAvailable: 600 kB\n", memory));
    EXPECT_EQ(memory.totalBytes, 1024000u);
    EXPECT_EQ(memory.availableBytes, 614400u);
}

TEST(ProcFsTest, EstimatesAvailableMemoryOnOldKernels)
{
    process_manager::MemoryInfo memory{};
    ASSERT_TRUE(process_manager::ParseMemInfo("MemTotal: 1000 kB\nMemFree: 100 kB\nBuffers: 50 kB\nCached: 250 kB\n",
                                              memory));
    EXPECT_EQ(memory.availableBytes, 400u * 1024);
}

TEST(ProcFsTest, ParsesLoadAverage)
{
    process_manager::LoadAverage load{};
    ASSERT_TRUE(process_manager::ParseLoadAverage("0.52 1.25 2.00 1/467 12345\n", load));
    EXPECT_DOUBLE_EQ(load.one, 0.52);
    EXPECT_DOUBLE_EQ(load.fifteen, 2.0);
}

TEST(ProcFsTest, ParsesProcessIo)
{
    process_manager::IoCounters io{};
    ASSERT_TRUE(process_manager::ParseProcessIo("rchar: 1\nwchar: 2\nread_bytes: 4096\nwrite_bytes: 8192\n", io));
    EXPECT_EQ(io.readBytes, 4096u);
    EXPECT_EQ(io.writeBytes, 8192u);
}

TEST(ProcFsTest, ParsesCgroupFiles)
{
    std::uint64_t usage = 0;
    ASSERT_TRUE(process_manager::ParseCgroupCpuStat("usage_usec 123456\nuser_usec 100000\n", usage));
    EXPECT_EQ(usage, 123456u);

    const std::vector<process_manager::DeviceIo> io = process_manager::ParseCgroupIoStat(
        "8:0 rbytes=100 wbytes=200 rios=1 wios=2 dbytes=0 dios=0\n8:16 rbytes=5 wbytes=6\n");
    ASSERT_EQ(io.size(), 2u);
    EXPECT_EQ(io[0].device, "8:0");
    EXPECT_EQ(io[0].readBytes, 100u);
    EXPECT_EQ(io[0].writeBytes, 200u);
    EXPECT_EQ(io[1].device, "8:16");
    EXPECT_EQ(io[1].writeBytes, 6u);
    EXPECT_TRUE(process_manager::ParseCgroupIoStat("").empty());

    std::uint32_t kills = 0;
    ASSERT_TRUE(process_manager::ParseMemoryEvents("low 0\nhigh 0\nmax 3\noom 2\noom_kill 1\n", kills));
    EXPECT_EQ(kills, 1u);
}

TEST(ProcFsTest, FindsTheCgroup2Mount)
{
    const std::string unified =
        "22 1 0:21 / /proc rw - proc proc rw\n"
        "30 22 0:26 / /sys/fs/cgroup rw,nosuid shared:4 - cgroup2 cgroup2 rw,nsdelegate\n";
    EXPECT_EQ(process_manager::FindCgroup2Mount(unified), std::string{"/sys/fs/cgroup"});

    const std::string hybrid =
        "31 25 0:27 / /sys/fs/cgroup/unified rw shared:5 - cgroup2 cgroup2 rw\n"
        "32 25 0:28 / /sys/fs/cgroup/systemd rw shared:6 - cgroup cgroup rw,name=systemd\n";
    EXPECT_EQ(process_manager::FindCgroup2Mount(hybrid), std::string{"/sys/fs/cgroup/unified"});

    EXPECT_EQ(process_manager::FindCgroup2Mount("32 25 0:28 / /sys/fs/cgroup/cpu rw - cgroup cgroup rw,cpu\n"),
              std::nullopt);
    EXPECT_EQ(process_manager::FindCgroup2Mount("40 25 0:30 / /mnt/with\\040space rw - cgroup2 none rw\n"),
              std::string{"/mnt/with space"});
}

TEST(ProcFsTest, FindsTheUnifiedCgroupPath)
{
    EXPECT_EQ(process_manager::FindUnifiedCgroupPath("12:cpu:/x\n1:name=systemd:/y\n0::/system.slice/pm.service\n"),
              std::string{"/system.slice/pm.service"});
    EXPECT_EQ(process_manager::FindUnifiedCgroupPath("1:name=systemd:/y\n"), std::nullopt);
}

TEST(ProcFsTest, ReadsProcessesBelowItsRoot)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "process_manager_procfs_test";
    std::filesystem::remove_all(root);
    WriteFile(root / "4242" / "stat", stat_line);
    WriteFile(root / "4242" / "io", "read_bytes: 1\nwrite_bytes: 2\n");
    WriteFile(root / "4242" / "fd" / "0", "");
    WriteFile(root / "4242" / "fd" / "1", "");
    WriteFile(root / "stat", "cpu  1 2 3 4 5 6 7 8\n");
    WriteFile(root / "meminfo", "MemTotal: 10 kB\nMemAvailable: 5 kB\n");
    WriteFile(root / "loadavg", "1.0 2.0 3.0 1/1 1\n");
    WriteFile(root / "uptime", "12345.67 1000.00\n");
    WriteFile(root / "self" / "stat", "not a pid\n");

    const process_manager::ProcFs procFs{root.string()};
    const std::vector<int> pids = procFs.ListPids();
    EXPECT_EQ(pids, std::vector<int>{4242});
    process_manager::ProcessStat stat{};
    EXPECT_TRUE(procFs.ReadProcessStat(4242, stat));
    EXPECT_FALSE(procFs.ReadProcessStat(1, stat));
    process_manager::IoCounters io{};
    EXPECT_TRUE(procFs.ReadProcessIo(4242, io));
    EXPECT_EQ(procFs.CountOpenFiles(4242), 2);
    EXPECT_EQ(procFs.CountOpenFiles(1), -1);
    EXPECT_TRUE(procFs.ProcessExists(4242));
    EXPECT_FALSE(procFs.ProcessExists(1));
    process_manager::CpuTimes times{};
    EXPECT_TRUE(procFs.ReadSystemCpu(times));
    process_manager::MemoryInfo memory{};
    EXPECT_TRUE(procFs.ReadMemory(memory));
    process_manager::LoadAverage load{};
    EXPECT_TRUE(procFs.ReadLoadAverage(load));
    std::uint64_t uptime = 0;
    ASSERT_TRUE(procFs.ReadUptime(uptime));
    EXPECT_EQ(uptime, 12345u);
    std::filesystem::remove_all(root);
}

TEST(ProcFsTest, ReadTextFileReportsMissingFiles)
{
    EXPECT_EQ(process_manager::ReadTextFile("/definitely/not/here"), std::nullopt);
}

TEST(ProcFsTest, ReadsFilesWithNonAsciiNames)
{
    // Paths arrive as UTF-8, for example a Windows profile directory named after its user.
    const std::u8string directory = std::filesystem::temp_directory_path().u8string();
    const std::string path = std::string{directory.begin(), directory.end()} + "/process_manager_" + Sahin() + ".conf";
    {
        std::ofstream file{process_manager::Utf8Path(path), std::ios::binary};
        file << "ok";
    }
    EXPECT_EQ(process_manager::ReadTextFile(path), std::optional<std::string>{"ok"});
    std::filesystem::remove(process_manager::Utf8Path(path));
}
