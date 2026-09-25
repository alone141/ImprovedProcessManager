#include <gtest/gtest.h>

#include "NativeLauncher.hpp"
#include "ProcFs.hpp"
#include "ProcessLauncher.hpp"
#include "ProcessTable.hpp"
#include "ServiceConfig.hpp"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::optional<process_manager::ExitStatus> WaitForExit(process_manager::ChildProcess& child,
                                                       std::chrono::milliseconds timeout)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const std::optional<process_manager::ExitStatus> status = child.Poll();
        if (status.has_value())
        {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return std::nullopt;
}

std::filesystem::path TempDirectory(const std::string& name)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::string ReadAll(const std::filesystem::path& path)
{
    return process_manager::ReadTextFile(path.string()).value_or("");
}

// A command run by the platform shell: /bin/sh -c on POSIX, cmd.exe /c on Windows.
process_manager::LaunchRequest Shell(const std::string& command)
{
    process_manager::LaunchRequest request{};
    request.serviceName = "test";
#ifdef _WIN32
    request.binary = "cmd.exe";
    request.arguments = {"/d", "/c", command};
#else
    request.binary = "/bin/sh";
    request.arguments = {"-c", command};
#endif
    request.output = process_manager::OutputMode::Inherit;
    return request;
}

#ifdef _WIN32
const char* const long_command = "ping -n 30 127.0.0.1 > NUL";
#else
const char* const long_command = "sleep 30";
#endif

} // namespace

TEST(NativeLauncherTest, ReportsTheExitCode)
{
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    ASSERT_EQ(launcher.Launch(Shell("exit 3"), child, error), process_manager::LaunchCode::Ok) << error;
    ASSERT_NE(child, nullptr);
    EXPECT_GT(child->Pid(), 0);
    const std::optional<process_manager::ExitStatus> status = WaitForExit(*child, std::chrono::seconds{10});
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->code, 3);
    EXPECT_EQ(status->signal, 0);
    EXPECT_EQ(child->Poll()->code, 3);
}

TEST(NativeLauncherTest, ReportsAMissingBinary)
{
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    process_manager::LaunchRequest request = Shell("");
    request.binary = (std::filesystem::temp_directory_path() / "no_such_program_4711").string();
    EXPECT_EQ(launcher.Launch(request, child, error), process_manager::LaunchCode::BinaryNotFound);
    EXPECT_NE(error.find("does not exist"), std::string::npos) << error;

    request.binary = "no_such_program_4711";
    EXPECT_EQ(launcher.Launch(request, child, error), process_manager::LaunchCode::BinaryNotFound);
    EXPECT_NE(error.find("PATH"), std::string::npos) << error;
    EXPECT_EQ(child, nullptr);
}

TEST(NativeLauncherTest, ReportsAMissingWorkingDirectory)
{
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    process_manager::LaunchRequest request = Shell("exit 0");
    request.workingDirectory = (std::filesystem::temp_directory_path() / "no_such_directory_4711").string();
    EXPECT_EQ(launcher.Launch(request, child, error), process_manager::LaunchCode::WorkingDirectoryMissing);
}

TEST(NativeLauncherTest, PassesEnvironmentAndWorkingDirectory)
{
    const std::filesystem::path directory = TempDirectory("process_manager_launch_env");
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
#ifdef _WIN32
    process_manager::LaunchRequest request = Shell("echo %PM_TEST_VALUE%> out.txt");
#else
    process_manager::LaunchRequest request = Shell("echo \"$PM_TEST_VALUE\" > out.txt");
#endif
    request.workingDirectory = directory.string();
    request.environment.push_back(process_manager::EnvironmentVariable{"PM_TEST_VALUE", "hello"});
    ASSERT_EQ(launcher.Launch(request, child, error), process_manager::LaunchCode::Ok) << error;
    ASSERT_TRUE(WaitForExit(*child, std::chrono::seconds{10}).has_value());
    const std::string text = ReadAll(directory / "out.txt");
    EXPECT_EQ(text.substr(0, 5), "hello") << text;
    std::filesystem::remove_all(directory);
}

TEST(NativeLauncherTest, WritesOutputToAFile)
{
    const std::filesystem::path directory = TempDirectory("process_manager_launch_output");
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    process_manager::LaunchRequest request = Shell("echo to-stdout && echo to-stderr 1>&2");
    request.output = process_manager::OutputMode::File;
    request.outputPath = (directory / "service.log").string();
    ASSERT_EQ(launcher.Launch(request, child, error), process_manager::LaunchCode::Ok) << error;
    ASSERT_TRUE(WaitForExit(*child, std::chrono::seconds{10}).has_value());
    const std::string text = ReadAll(directory / "service.log");
    EXPECT_NE(text.find("to-stdout"), std::string::npos) << text;
    EXPECT_NE(text.find("to-stderr"), std::string::npos) << text;
    std::filesystem::remove_all(directory);
}

TEST(NativeLauncherTest, KillEndsALongRunningProcess)
{
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    ASSERT_EQ(launcher.Launch(Shell(long_command), child, error), process_manager::LaunchCode::Ok) << error;
    EXPECT_FALSE(child->Poll().has_value());
    child->Kill();
    EXPECT_TRUE(WaitForExit(*child, std::chrono::seconds{10}).has_value());
}

TEST(NativeLauncherTest, SamplesTheRunningProcess)
{
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    ASSERT_EQ(launcher.Launch(Shell(long_command), child, error), process_manager::LaunchCode::Ok) << error;
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    const process_manager::ProcessTable table = process_manager::ProcessTable::Capture();
    process_manager::UsageSample usage{};
    child->Sample(table, usage);
    EXPECT_GE(usage.processCount, 1);
    EXPECT_GT(usage.memoryBytes, 0u);
    EXPECT_GE(usage.threadCount, 1);
    EXPECT_EQ(usage.memoryPeakBytes, usage.memoryBytes);
    const std::vector<int> members = child->MemberPids(table);
    EXPECT_FALSE(members.empty());
    child->Kill();
    WaitForExit(*child, std::chrono::seconds{10});
}

#ifndef _WIN32

TEST(NativeLauncherTest, StopSignalReachesTheProcessGroup)
{
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    ASSERT_EQ(launcher.Launch(Shell("sleep 30"), child, error), process_manager::LaunchCode::Ok) << error;
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    child->RequestStop(process_manager::StopSignal::Terminate);
    const std::optional<process_manager::ExitStatus> status = WaitForExit(*child, std::chrono::seconds{10});
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->signal, 15);
}

// True once the process is gone or only waits to be collected.
bool Gone(int pid)
{
    const process_manager::ProcFs procFs{"/proc"};
    process_manager::ProcessStat stat{};
    return !procFs.ReadProcessStat(pid, stat) || stat.state == 'Z';
}

TEST(NativeLauncherTest, KillsLeftoversBeforeReportingTheExit)
{
    const std::filesystem::path directory = TempDirectory("process_manager_launch_leftover");
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    process_manager::LaunchRequest request = Shell("sleep 30 & echo $! > pid; exit 3");
    request.workingDirectory = directory.string();
    ASSERT_EQ(launcher.Launch(request, child, error), process_manager::LaunchCode::Ok) << error;
    const std::optional<process_manager::ExitStatus> status = WaitForExit(*child, std::chrono::seconds{10});
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->code, 3);

    const int leftover = std::stoi(ReadAll(directory / "pid"));
    EXPECT_TRUE(Gone(leftover)) << "the exit was reported while process " << leftover << " still ran";
    std::filesystem::remove_all(directory);
}

TEST(NativeLauncherTest, KillsLeftoversThatLeftTheProcessGroup)
{
    // bash gives a background job a process group of its own under set -m, even without a
    // terminal; dash turns job control off there.
    if (!std::filesystem::exists("/bin/bash"))
    {
        SUCCEED() << "bash is not installed";
        return;
    }
    const std::filesystem::path directory = TempDirectory("process_manager_launch_group");
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    process_manager::LaunchRequest request =
        Shell("set -m; sleep 30 & echo $! > pid; cut -d' ' -f5 /proc/$!/stat > group; exit 0");
    request.binary = "/bin/bash";
    request.workingDirectory = directory.string();
    ASSERT_EQ(launcher.Launch(request, child, error), process_manager::LaunchCode::Ok) << error;
    const int main = child->Pid();
    ASSERT_TRUE(WaitForExit(*child, std::chrono::seconds{10}).has_value());

    const int leftover = std::stoi(ReadAll(directory / "pid"));
    EXPECT_NE(std::stoi(ReadAll(directory / "group")), main) << "the job stayed in the service's process group";
    EXPECT_TRUE(Gone(leftover)) << "process " << leftover << " outlived its service";
    std::filesystem::remove_all(directory);
}

TEST(NativeLauncherTest, StopSignalReachesAStoppedProcess)
{
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    ASSERT_EQ(launcher.Launch(Shell("trap 'exit 7' TERM; while :; do sleep 0.1; done"), child, error),
              process_manager::LaunchCode::Ok)
        << error;
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    ASSERT_EQ(::kill(child->Pid(), SIGSTOP), 0);
    const process_manager::ProcFs procFs{"/proc"};
    process_manager::ProcessStat stat{};
    for (int attempt = 0; attempt < 100 && !(procFs.ReadProcessStat(child->Pid(), stat) && stat.state == 'T');
         ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    ASSERT_EQ(stat.state, 'T');

    // The trap can only run once the shell is continued.
    child->RequestStop(process_manager::StopSignal::Terminate);
    const std::optional<process_manager::ExitStatus> status = WaitForExit(*child, std::chrono::seconds{5});
    ASSERT_TRUE(status.has_value()) << "the stopped shell never ran its trap";
    EXPECT_EQ(status->code, 7);
}

TEST(NativeLauncherTest, ServicesStartWithDefaultSignals)
{
    const std::filesystem::path directory = TempDirectory("process_manager_launch_signals");
    sigset_t blocked{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigset_t previous{};
    ::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    struct sigaction saved{};
    ::sigaction(SIGUSR2, &ignore, &saved);

    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    process_manager::LaunchRequest request{};
    request.serviceName = "test";
    request.binary = "grep";
    request.arguments = {"-E", "^Sig(Blk|Ign):", "/proc/self/status"};
    request.output = process_manager::OutputMode::File;
    request.outputPath = (directory / "signals").string();
    const process_manager::LaunchCode launched = launcher.Launch(request, child, error);
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    ::sigaction(SIGUSR2, &saved, nullptr);

    ASSERT_EQ(launched, process_manager::LaunchCode::Ok) << error;
    ASSERT_TRUE(WaitForExit(*child, std::chrono::seconds{10}).has_value());
    EXPECT_EQ(ReadAll(directory / "signals"), "SigBlk:\t0000000000000000\nSigIgn:\t0000000000000000\n");
    std::filesystem::remove_all(directory);
}

TEST(NativeLauncherTest, RelativePathEntriesAreReadFromTheManagersDirectory)
{
    const std::filesystem::path tools = TempDirectory("process_manager_launch_tools");
    const std::filesystem::path work = TempDirectory("process_manager_launch_work");
    {
        std::ofstream file{tools / "pm_tool"};
        file << "#!/bin/sh\necho ran > marker\n";
    }
    std::filesystem::permissions(tools / "pm_tool", std::filesystem::perms::owner_all);
    const std::filesystem::path relative = std::filesystem::relative(tools);
    ASSERT_TRUE(!relative.empty() && relative.is_relative()) << relative;

    process_manager::LaunchRequest request{};
    request.serviceName = "test";
    request.binary = "pm_tool";
    request.workingDirectory = work.string();
    request.environment.push_back(process_manager::EnvironmentVariable{"PATH", relative.string()});
    request.output = process_manager::OutputMode::Inherit;
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    ASSERT_EQ(launcher.Launch(request, child, error), process_manager::LaunchCode::Ok) << error;
    const std::optional<process_manager::ExitStatus> status = WaitForExit(*child, std::chrono::seconds{10});
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->code, 0);
    EXPECT_EQ(ReadAll(work / "marker"), "ran\n");
    std::filesystem::remove_all(tools);
    std::filesystem::remove_all(work);
}

TEST(NativeLauncherTest, RejectsFilesThatAreNotExecutable)
{
    const std::filesystem::path directory = TempDirectory("process_manager_launch_noexec");
    const std::filesystem::path script = directory / "script.sh";
    {
        std::ofstream file{script};
        file << "#!/bin/sh\nexit 0\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    process_manager::LaunchRequest request = Shell("");
    request.binary = script.string();
    request.arguments.clear();
    EXPECT_EQ(launcher.Launch(request, child, error), process_manager::LaunchCode::BinaryNotFound);
    EXPECT_NE(error.find("not an executable"), std::string::npos) << error;
    std::filesystem::remove_all(directory);
}

TEST(NativeLauncherTest, DestroyingAChildKillsIt)
{
    process_manager::NativeLauncher launcher{nullptr};
    std::unique_ptr<process_manager::ChildProcess> child{};
    std::string error{};
    ASSERT_EQ(launcher.Launch(Shell("sleep 30"), child, error), process_manager::LaunchCode::Ok) << error;
    const int pid = child->Pid();
    child.reset();
    const process_manager::ProcFs procFs{"/proc"};
    process_manager::ProcessStat stat{};
    EXPECT_FALSE(procFs.ReadProcessStat(pid, stat) && stat.state != 'Z');
}

#endif
