#include "NativeLauncher.hpp"
#include "CgroupTree.hpp"
#include "Logger.hpp"
#include "ProcFs.hpp"
#include "ProcessLauncher.hpp"
#include "ProcessTable.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

// Linux only: the child asks for SIGTERM when the manager dies.
#ifdef __linux__
#include <sys/prctl.h>
#endif

extern char** environ;

namespace process_manager
{

namespace
{

constexpr char journal_socket[] = "/run/systemd/journal/stdout";
constexpr int stage_cgroup = 1;
constexpr int stage_chdir = 2;
constexpr int stage_exec = 3;

// Variables that describe the manager's own systemd unit; a service must not see them.
constexpr const char* unit_variables[] = {"NOTIFY_SOCKET", "WATCHDOG_USEC", "WATCHDOG_PID",
                                          "LISTEN_FDS",    "LISTEN_PID",    "LISTEN_FDNAMES"};

struct ExecFailure
{
    int stage;
    int error;
};

class FileDescriptor
{
public:
    explicit FileDescriptor(int fd)
        : fd{fd}
    {
    }

    ~FileDescriptor()
    {
        Reset();
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : fd{other.fd}
    {
        other.fd = -1;
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }

    int Get() const
    {
        return fd;
    }

    void Reset()
    {
        if (fd >= 0)
        {
            ::close(fd);
            fd = -1;
        }
    }

private:
    int fd;
};

// Everything the child needs, prepared before fork: after it, the child may only
// make async-signal-safe calls.
struct ChildPlan
{
    const char* path{nullptr};
    char* const* argv{nullptr};
    char* const* envp{nullptr};
    const char* workingDirectory{nullptr};
    int devNull{-1};
    int outputFd{-1};
    int cgroupFd{-1};
    int reportFd{-1};
    bool journal{false};
    sockaddr_un journalAddress{};
    const char* journalHeader{nullptr};
    std::size_t journalHeaderSize{0};
    pid_t parent{0};
};

std::string ErrorText(int error)
{
    return std::generic_category().message(error);
}

bool IsExecutableFile(const std::string& path)
{
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

bool IsDirectory(const std::string& path)
{
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// The child runs execve after chdir, so a relative path would be looked up from the
// working directory instead of the directory it was found in.
std::string AbsolutePath(const std::string& path)
{
    if (path.empty() || path.front() == '/')
    {
        return path;
    }
    std::error_code error{};
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    return error ? path : absolute.string();
}

bool JournalAvailable()
{
    struct stat info{};
    return ::stat(journal_socket, &info) == 0 && S_ISSOCK(info.st_mode);
}

std::string VariableFor(std::span<const EnvironmentVariable> overrides, const std::string& name)
{
    for (std::size_t i = overrides.size(); i > 0; --i)
    {
        if (overrides[i - 1].name == name)
        {
            return overrides[i - 1].value;
        }
    }
    const char* value = std::getenv(name.c_str());
    return value != nullptr ? std::string{value} : std::string{};
}

LaunchCode ResolveBinary(const LaunchRequest& request, std::string& path, std::string& error)
{
    const std::string& binary = request.binary;
    if (binary.find('/') == std::string::npos)
    {
        std::string search = VariableFor(request.environment, "PATH");
        if (search.empty())
        {
            search = "/usr/local/bin:/usr/bin:/bin";
        }
        std::size_t start = 0;
        while (start <= search.size())
        {
            const std::size_t end = search.find(':', start);
            const std::string directory =
                search.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const std::string candidate = (directory.empty() ? std::string{"."} : directory) + "/" + binary;
            if (IsExecutableFile(candidate))
            {
                path = AbsolutePath(candidate);
                return LaunchCode::Ok;
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
        error = binary + " was not found on PATH";
        return LaunchCode::BinaryNotFound;
    }

    path = AbsolutePath(binary.front() == '/' || request.workingDirectory.empty()
                            ? binary
                            : request.workingDirectory + "/" + binary);
    if (!IsExecutableFile(path))
    {
        struct stat info{};
        error = path + (::stat(path.c_str(), &info) == 0 ? " is not an executable file" : " does not exist");
        return LaunchCode::BinaryNotFound;
    }
    return LaunchCode::Ok;
}

std::vector<std::string> BuildEnvironment(std::span<const EnvironmentVariable> overrides)
{
    std::map<std::string, std::string> variables{};
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
    {
        const std::string text{*entry};
        const std::size_t equals = text.find('=');
        if (equals != std::string::npos && equals > 0)
        {
            variables[text.substr(0, equals)] = text.substr(equals + 1);
        }
    }
    for (const char* name : unit_variables)
    {
        variables.erase(name);
    }
    for (const EnvironmentVariable& variable : overrides)
    {
        variables[variable.name] = variable.value;
    }

    std::vector<std::string> entries{};
    entries.reserve(variables.size());
    for (const std::pair<const std::string, std::string>& variable : variables)
    {
        entries.push_back(variable.first + "=" + variable.second);
    }
    return entries;
}

std::vector<char*> Pointers(std::vector<std::string>& strings)
{
    std::vector<char*> pointers{};
    pointers.reserve(strings.size() + 1);
    for (std::string& text : strings)
    {
        pointers.push_back(text.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

int SignalNumber(StopSignal signal)
{
    switch (signal)
    {
    case StopSignal::Terminate:
        return SIGTERM;
    case StopSignal::Interrupt:
        return SIGINT;
    case StopSignal::Hangup:
        return SIGHUP;
    case StopSignal::Quit:
        return SIGQUIT;
    case StopSignal::Kill:
        return SIGKILL;
    case StopSignal::User1:
        return SIGUSR1;
    case StopSignal::User2:
        return SIGUSR2;
    }
    return SIGTERM;
}

[[noreturn]] void ReportAndExit(const ChildPlan& plan, int stage, int error)
{
    const ExecFailure failure{stage, error};
    const ssize_t written = ::write(plan.reportFd, &failure, sizeof(failure));
    static_cast<void>(written);
    ::_exit(127);
}

int ConnectJournal(const ChildPlan& plan)
{
    // No close-on-exec: this socket becomes the program's standard output.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&plan.journalAddress), sizeof(plan.journalAddress)) != 0)
    {
        ::close(fd);
        return -1;
    }
    ::shutdown(fd, SHUT_RD);

    std::size_t sent = 0;
    while (sent < plan.journalHeaderSize)
    {
        const ssize_t written = ::write(fd, plan.journalHeader + sent, plan.journalHeaderSize - sent);
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        if (written <= 0)
        {
            ::close(fd);
            return -1;
        }
        sent += static_cast<std::size_t>(written);
    }
    return fd;
}

// The parent forks with every signal blocked, and they stay blocked until just before
// exec: no handler of the manager may run here, and a parent-death signal that comes
// early must find SIG_DFL.
[[noreturn]] void RunChild(const ChildPlan& plan)
{
    // Handlers are reset by exec, but ignored signals and the blocked mask are inherited.
    for (int number = 1; number < NSIG; ++number)
    {
        if (number != SIGKILL && number != SIGSTOP)
        {
            ::signal(number, SIG_DFL);
        }
    }

    ::setsid();
#ifdef __linux__
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() != plan.parent)
    {
        ::_exit(1);
    }
#endif

    // Joining the task cgroup comes first, so every later child of the service lands in it.
    if (plan.cgroupFd >= 0 && ::write(plan.cgroupFd, "0", 1) != 1)
    {
        ReportAndExit(plan, stage_cgroup, errno);
    }

    ::dup2(plan.devNull, STDIN_FILENO);
    int output = plan.outputFd;
    if (plan.journal)
    {
        output = ConnectJournal(plan);
    }
    if (output >= 0)
    {
        ::dup2(output, STDOUT_FILENO);
        ::dup2(output, STDERR_FILENO);
        if (plan.journal && output > STDERR_FILENO)
        {
            ::close(output);
        }
    }

    if (plan.workingDirectory != nullptr && ::chdir(plan.workingDirectory) != 0)
    {
        ReportAndExit(plan, stage_chdir, errno);
    }

    sigset_t none{};
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    ::execve(plan.path, plan.argv, plan.envp);
    ReportAndExit(plan, stage_exec, errno);
}

class PosixChild : public ChildProcess
{
public:
    PosixChild(pid_t pid, std::string service, CgroupTree* cgroups)
        : pid{pid}, service{std::move(service)}, cgroups{cgroups}, procFs{"/proc"}, exit{}, mainExited{false},
          drainDeadline{}, peakMemory{0}
    {
    }

    ~PosixChild() override
    {
        if (!exit.has_value())
        {
            Kill();
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
    }

    // Held through std::unique_ptr: a process is neither copied nor moved.
    PosixChild(const PosixChild&) = delete;
    PosixChild& operator=(const PosixChild&) = delete;
    PosixChild(PosixChild&&) = delete;
    PosixChild& operator=(PosixChild&&) = delete;

    int Pid() const override
    {
        return static_cast<int>(pid);
    }

    std::optional<ExitStatus> Poll() override
    {
        if (exit.has_value())
        {
            return exit;
        }
        if (!mainExited)
        {
            // WNOWAIT leaves the zombie in place, so the process group ID cannot be reused
            // while the rest of the service is killed.
            siginfo_t info{};
            if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0)
            {
                return LostTrack(errno);
            }
            if (info.si_pid == 0)
            {
                return std::nullopt;
            }
            mainExited = true;
            drainDeadline = std::chrono::steady_clock::now() + kill_grace;
        }

        // The old instance's processes may still hold ports and files a restart needs.
        const std::size_t left = KillLeftovers();
        if (left > 0)
        {
            if (std::chrono::steady_clock::now() < drainDeadline)
            {
                return std::nullopt;
            }
            LogWarning("service " + service + ": " + std::to_string(left) + " processes still run " +
                       std::to_string(kill_grace.count() / 1000) + " s after SIGKILL");
        }

        int status = 0;
        if (::waitpid(pid, &status, 0) < 0)
        {
            return LostTrack(errno);
        }
        if (WIFEXITED(status))
        {
            exit = ExitStatus{WEXITSTATUS(status), 0};
        }
        else if (WIFSIGNALED(status))
        {
            exit = ExitStatus{0, WTERMSIG(status)};
        }
        else
        {
            exit = ExitStatus{255, 0};
        }
        return exit;
    }

    void RequestStop(StopSignal signal) override
    {
        if (exit.has_value())
        {
            return;
        }
        const int number = SignalNumber(signal);
        ::kill(-pid, number);
        if (number != SIGKILL)
        {
            // A stopped process only acts on the signal once it runs again.
            ::kill(-pid, SIGCONT);
        }
    }

    void Kill() override
    {
        if (!exit.has_value())
        {
            KillLeftovers();
        }
    }

    void Sample(const ProcessTable& table, UsageSample& out) override
    {
        UsageSample sample{};
        const std::vector<int> members = MemberPids(table);
        sample.processCount = static_cast<int>(members.size());
        int files = 0;
        bool filesKnown = !members.empty();
        for (const int member : members)
        {
            const ProcessEntry* entry = table.Find(member);
            if (entry != nullptr)
            {
                // What a member has waited for counts too, such as a shell's finished commands.
                sample.cpuTimeUsec += entry->cpuTimeUsec + entry->childCpuTimeUsec;
                sample.memoryBytes += entry->rssBytes;
                sample.threadCount += entry->threads;
            }
            const int open = procFs.CountOpenFiles(member);
            if (open >= 0)
            {
                files += open;
            }
            else if (procFs.ProcessExists(member))
            {
                filesKnown = false; // another user's process; one that just exited is skipped
            }
            IoCounters io{};
            if (procFs.ReadProcessIo(member, io))
            {
                sample.ioReadBytes += io.readBytes;
                sample.ioWriteBytes += io.writeBytes;
            }
        }
        sample.openFiles = filesKnown ? files : -1;

        if (cgroups != nullptr)
        {
            const CgroupStats stats = cgroups->ReadStats(service);
            if (stats.hasCpu)
            {
                sample.cpuTimeUsec = stats.cpuUsageUsec;
            }
            if (stats.hasIo)
            {
                sample.ioReadBytes = stats.ioReadBytes;
                sample.ioWriteBytes = stats.ioWriteBytes;
            }
            sample.oomKills = stats.oomKills;
            sample.cgroup = true;
        }
        peakMemory = std::max(peakMemory, sample.memoryBytes);
        sample.memoryPeakBytes = peakMemory;
        out = sample;
    }

    std::vector<int> MemberPids(const ProcessTable& table) const override
    {
        if (cgroups != nullptr)
        {
            std::vector<int> members = cgroups->ReadMembers(service);
            if (!members.empty())
            {
                std::sort(members.begin(), members.end());
                return members;
            }
        }
        return table.Members(static_cast<int>(pid));
    }

private:
    // Sends SIGKILL to whatever is left of the service; returns how many processes that was.
    std::size_t KillLeftovers() const
    {
        ::kill(-pid, SIGKILL);
        if (cgroups != nullptr)
        {
            const std::size_t left = cgroups->ReadMembers(service).size();
            if (left > 0)
            {
                cgroups->KillMembers(service);
            }
            return left;
        }
        // Without a cgroup, members that moved to a process group of their own are found
        // through the session and their parents.
        const std::vector<int> members = ProcessTable::Capture().Members(static_cast<int>(pid));
        for (const int member : members)
        {
            ::kill(member, SIGKILL);
        }
        return members.size();
    }

    // Someone else collected the exit status, so the process ID may be in use again:
    // only the cgroup is still known to hold this service.
    std::optional<ExitStatus> LostTrack(int error)
    {
        LogWarning("service " + service + ": lost track of process " + std::to_string(pid) + ": " + ErrorText(error));
        if (cgroups != nullptr)
        {
            cgroups->KillMembers(service);
        }
        exit = ExitStatus{255, 0};
        return exit;
    }

    pid_t pid;
    std::string service;
    CgroupTree* cgroups;
    ProcFs procFs;
    std::optional<ExitStatus> exit;
    bool mainExited;
    std::chrono::steady_clock::time_point drainDeadline;
    std::uint64_t peakMemory;
};

} // namespace

NativeLauncher::NativeLauncher(CgroupTree* cgroups)
    : cgroups{cgroups}
{
}

LaunchCode NativeLauncher::Launch(const LaunchRequest& request, std::unique_ptr<ChildProcess>& out,
                                  std::string& error)
{
    std::string path{};
    const LaunchCode resolved = ResolveBinary(request, path, error);
    if (resolved != LaunchCode::Ok)
    {
        return resolved;
    }
    if (!request.workingDirectory.empty() && !IsDirectory(request.workingDirectory))
    {
        error = "working directory " + request.workingDirectory + " does not exist";
        return LaunchCode::WorkingDirectoryMissing;
    }

    std::vector<std::string> arguments{path};
    arguments.insert(arguments.end(), request.arguments.begin(), request.arguments.end());
    std::vector<char*> argv = Pointers(arguments);
    std::vector<std::string> environment = BuildEnvironment(request.environment);
    std::vector<char*> envp = Pointers(environment);

    const FileDescriptor devNull{::open("/dev/null", O_RDWR | O_CLOEXEC)};
    if (devNull.Get() < 0)
    {
        error = "cannot open /dev/null: " + ErrorText(errno);
        return LaunchCode::OutputFailed;
    }

    const bool journalWanted =
        request.output == OutputMode::Journal ||
        (request.output == OutputMode::Auto && std::getenv("JOURNAL_STREAM") != nullptr);
    const bool journal = journalWanted && JournalAvailable();
    if (request.output == OutputMode::Journal && !journal)
    {
        LogWarning("service " + request.serviceName + ": journald is not available; output goes to the manager's");
    }
    FileDescriptor outputFile{-1};
    if (request.output == OutputMode::File)
    {
        outputFile = FileDescriptor{::open(request.outputPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)};
        if (outputFile.Get() < 0)
        {
            error = "cannot open " + request.outputPath + ": " + ErrorText(errno);
            return LaunchCode::OutputFailed;
        }
    }

    FileDescriptor cgroupFile{-1};
    if (cgroups != nullptr)
    {
        std::string problem{};
        if (cgroups->CreateGroup(request.serviceName, problem) != CgroupCode::Ok)
        {
            error = problem;
            return LaunchCode::SpawnFailed;
        }
        if (cgroups->ApplyLimits(request.serviceName, request.memoryMax, request.cpuMaxPercent, problem) !=
            CgroupCode::Ok)
        {
            LogWarning("service " + request.serviceName + ": " + problem);
        }
        cgroupFile = FileDescriptor{::open(cgroups->ProcsPath(request.serviceName).c_str(), O_WRONLY | O_CLOEXEC)};
        if (cgroupFile.Get() < 0)
        {
            error = "cannot open " + cgroups->ProcsPath(request.serviceName) + ": " + ErrorText(errno);
            return LaunchCode::SpawnFailed;
        }
    }
    else if (request.memoryMax > 0 || request.cpuMaxPercent > 0)
    {
        LogWarning("service " + request.serviceName + ": memory_max and cpu_max need cgroups; not applied");
    }

    int pipeFds[2]{-1, -1};
    if (::pipe2(pipeFds, O_CLOEXEC) != 0)
    {
        error = "cannot create a pipe: " + ErrorText(errno);
        return LaunchCode::SpawnFailed;
    }
    FileDescriptor reportRead{pipeFds[0]};
    FileDescriptor reportWrite{pipeFds[1]};

    const std::string journalHeader = request.serviceName + "\n\n6\n1\n0\n0\n0\n";
    ChildPlan plan{};
    plan.path = path.c_str();
    plan.argv = argv.data();
    plan.envp = envp.data();
    plan.workingDirectory = request.workingDirectory.empty() ? nullptr : request.workingDirectory.c_str();
    plan.devNull = devNull.Get();
    plan.outputFd = request.output == OutputMode::Null ? devNull.Get() : outputFile.Get();
    plan.cgroupFd = cgroupFile.Get();
    plan.reportFd = reportWrite.Get();
    plan.journal = journal;
    plan.journalAddress.sun_family = AF_UNIX;
    std::memcpy(plan.journalAddress.sun_path, journal_socket, sizeof(journal_socket));
    plan.journalHeader = journalHeader.c_str();
    plan.journalHeaderSize = journalHeader.size();
    plan.parent = ::getpid();

    // Every signal stays blocked across fork, until the child has reset the manager's handlers.
    sigset_t all{};
    sigset_t previous{};
    sigfillset(&all);
    ::pthread_sigmask(SIG_SETMASK, &all, &previous);
    const pid_t child = ::fork();
    const int forkError = errno;
    if (child == 0)
    {
        RunChild(plan);
    }
    ::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (child < 0)
    {
        error = "fork failed: " + ErrorText(forkError);
        return LaunchCode::SpawnFailed;
    }

    reportWrite.Reset();
    ExecFailure failure{0, 0};
    ssize_t received = 0;
    do
    {
        received = ::read(reportRead.Get(), &failure, sizeof(failure));
    } while (received < 0 && errno == EINTR);

    if (received == static_cast<ssize_t>(sizeof(failure)))
    {
        int status = 0;
        ::waitpid(child, &status, 0);
        if (failure.stage == stage_cgroup)
        {
            error = "cannot join " + cgroups->GroupPath(request.serviceName) + ": " + ErrorText(failure.error);
            return LaunchCode::SpawnFailed;
        }
        if (failure.stage == stage_chdir)
        {
            error = "cannot enter " + request.workingDirectory + ": " + ErrorText(failure.error);
            return LaunchCode::WorkingDirectoryMissing;
        }
        error = "cannot execute " + path + ": " + ErrorText(failure.error);
        return LaunchCode::BinaryNotFound;
    }

    out = std::make_unique<PosixChild>(child, request.serviceName, cgroups);
    return LaunchCode::Ok;
}

} // namespace process_manager
