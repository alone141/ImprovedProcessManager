#include "NativeLauncher.hpp"
#include "Logger.hpp"
#include "ProcessTable.hpp"

// windows.h must come before the other Win32 headers, which rely on its types.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <psapi.h>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace process_manager
{

namespace
{

constexpr std::size_t initial_job_slots = 256;
constexpr DWORD kill_wait_ms = 5000;

std::wstring Widen(const std::string& text)
{
    if (text.empty())
    {
        return std::wstring{};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide{};
    wide.resize(static_cast<std::size_t>(size));
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty())
    {
        return std::string{};
    }

    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow{};
    narrow.resize(static_cast<std::size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size, nullptr,
                        nullptr);
    return narrow;
}

std::string SystemErrorText(DWORD error)
{
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = buffer != nullptr ? std::wstring{buffer} : std::wstring{};
    LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' '))
    {
        text.pop_back();
    }
    return Narrow(text) + " (error " + std::to_string(error) + ")";
}

class Handle
{
public:
    explicit Handle(HANDLE handle)
        : handle{handle}
    {
    }

    ~Handle()
    {
        Reset();
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept
        : handle{other.handle}
    {
        other.handle = nullptr;
    }

    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    HANDLE Get() const
    {
        return handle;
    }

    bool Valid() const
    {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }

    HANDLE Release()
    {
        const HANDLE released = handle;
        handle = nullptr;
        return released;
    }

    void Reset()
    {
        if (Valid())
        {
            CloseHandle(handle);
        }
        handle = nullptr;
    }

private:
    HANDLE handle;
};

// The quoting rules CommandLineToArgvW and the C runtime apply when they split a command line.
std::wstring QuoteArgument(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        return argument;
    }

    std::wstring quoted{L"\""};
    std::size_t i = 0;
    while (true)
    {
        std::size_t backslashes = 0;
        while (i < argument.size() && argument[i] == L'\\')
        {
            ++i;
            ++backslashes;
        }
        if (i == argument.size())
        {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (argument[i] == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
        }
        else
        {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(argument[i]);
        ++i;
    }
    quoted.push_back(L'"');
    return quoted;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring VariableFor(std::span<const EnvironmentVariable> overrides, const std::wstring& name)
{
    for (std::size_t i = overrides.size(); i > 0; --i)
    {
        if (_wcsicmp(Widen(overrides[i - 1].name).c_str(), name.c_str()) == 0)
        {
            return Widen(overrides[i - 1].value);
        }
    }
    const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (size == 0)
    {
        return std::wstring{};
    }
    std::wstring value{};
    value.resize(size);
    const DWORD length = GetEnvironmentVariableW(name.c_str(), value.data(), size);
    value.resize(length);
    return value;
}

LaunchCode ResolveBinary(const LaunchRequest& request, std::wstring& path, std::string& error)
{
    const std::wstring binary = Widen(request.binary);
    if (binary.find_first_of(L"\\/") == std::wstring::npos)
    {
        const std::wstring search = VariableFor(request.environment, L"PATH");
        std::wstring found{};
        found.resize(32768);
        const DWORD length = SearchPathW(search.empty() ? nullptr : search.c_str(), binary.c_str(), L".exe",
                                         static_cast<DWORD>(found.size()), found.data(), nullptr);
        if (length == 0 || length >= found.size())
        {
            error = request.binary + " was not found on PATH";
            return LaunchCode::BinaryNotFound;
        }
        found.resize(length);
        path = found;
        return LaunchCode::Ok;
    }

    const bool absolute = std::filesystem::path{binary}.is_absolute();
    path = absolute || request.workingDirectory.empty() ? binary : Widen(request.workingDirectory) + L"\\" + binary;
    if (!FileExists(path) && FileExists(path + L".exe"))
    {
        path += L".exe";
    }
    if (!FileExists(path))
    {
        error = Narrow(path) + " does not exist";
        return LaunchCode::BinaryNotFound;
    }
    return LaunchCode::Ok;
}

struct CaseInsensitiveLess
{
    bool operator()(const std::wstring& a, const std::wstring& b) const
    {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

std::vector<wchar_t> BuildEnvironmentBlock(std::span<const EnvironmentVariable> overrides)
{
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> variables{};
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* entry = block; entry != nullptr && *entry != L'\0'; entry += std::wcslen(entry) + 1)
    {
        const std::wstring text{entry};
        // Names such as "=C:" start with '=', so the separator is searched from the second character.
        const std::size_t equals = text.find(L'=', 1);
        if (equals != std::wstring::npos)
        {
            variables[text.substr(0, equals)] = text.substr(equals + 1);
        }
    }
    FreeEnvironmentStringsW(block);
    for (const EnvironmentVariable& variable : overrides)
    {
        variables[Widen(variable.name)] = Widen(variable.value);
    }

    std::vector<wchar_t> environment{};
    for (const std::pair<const std::wstring, std::wstring>& variable : variables)
    {
        environment.insert(environment.end(), variable.first.begin(), variable.first.end());
        environment.push_back(L'=');
        environment.insert(environment.end(), variable.second.begin(), variable.second.end());
        environment.push_back(L'\0');
    }
    environment.push_back(L'\0');
    return environment;
}

Handle InheritableCopy(HANDLE source)
{
    if (source == nullptr || source == INVALID_HANDLE_VALUE)
    {
        return Handle{nullptr};
    }

    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
    {
        return Handle{nullptr};
    }
    return Handle{copy};
}

Handle OpenNul(DWORD access)
{
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    return Handle{CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr)};
}

struct CloseRequest
{
    std::vector<DWORD> pids;
};

BOOL CALLBACK CloseWindowOfMember(HWND window, LPARAM parameter)
{
    const CloseRequest* request = reinterpret_cast<const CloseRequest*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (std::find(request->pids.begin(), request->pids.end(), owner) != request->pids.end())
    {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

// Processes still in the job; the main process may be gone already.
DWORD ActiveProcesses(HANDLE job)
{
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info), nullptr))
    {
        return 0;
    }
    return info.ActiveProcesses;
}

std::vector<DWORD> JobProcessIds(HANDLE job)
{
    std::size_t slots = initial_job_slots;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        std::vector<unsigned char> storage{};
        storage.resize(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + slots * sizeof(ULONG_PTR));
        JOBOBJECT_BASIC_PROCESS_ID_LIST* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
        if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, static_cast<DWORD>(storage.size()),
                                      nullptr) ||
            GetLastError() == ERROR_MORE_DATA)
        {
            if (list->NumberOfProcessIdsInList < list->NumberOfAssignedProcesses)
            {
                slots = static_cast<std::size_t>(list->NumberOfAssignedProcesses) * 2;
                continue;
            }
            std::vector<DWORD> pids{};
            for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i)
            {
                pids.push_back(static_cast<DWORD>(list->ProcessIdList[i]));
            }
            return pids;
        }
        break;
    }
    return std::vector<DWORD>{};
}

class WindowsChild : public ChildProcess
{
public:
    WindowsChild(HANDLE process, HANDLE job, DWORD pid)
        : process{process}, job{job}, pid{pid}, exit{}, ended{}, drainDeadline{}, peakMemory{0}
    {
    }

    ~WindowsChild() override
    {
        if (!exit.has_value())
        {
            TerminateJobObject(job.Get(), 1);
            WaitForSingleObject(process.Get(), kill_wait_ms);
        }
    }

    // Held through std::unique_ptr: a process is neither copied nor moved.
    WindowsChild(const WindowsChild&) = delete;
    WindowsChild& operator=(const WindowsChild&) = delete;
    WindowsChild(WindowsChild&&) = delete;
    WindowsChild& operator=(WindowsChild&&) = delete;

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
        if (!ended.has_value())
        {
            if (WaitForSingleObject(process.Get(), 0) != WAIT_OBJECT_0)
            {
                return std::nullopt;
            }
            DWORD code = 0;
            GetExitCodeProcess(process.Get(), &code);
            ended = ExitStatus{static_cast<int>(code), 0};
            drainDeadline = std::chrono::steady_clock::now() + kill_grace;
            TerminateJobObject(job.Get(), code);
        }

        // The termination runs on after TerminateJobObject returns; a restart must not meet
        // the old instance's processes.
        if (ActiveProcesses(job.Get()) > 0 && std::chrono::steady_clock::now() < drainDeadline)
        {
            return std::nullopt;
        }
        exit = ended;
        return exit;
    }

    void RequestStop(StopSignal signal) override
    {
        if (exit.has_value())
        {
            return;
        }
        if (signal == StopSignal::Kill)
        {
            Kill();
            return;
        }

        // Console programs share the manager's console and lead their own process group.
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);
        CloseRequest request{JobProcessIds(job.Get())};
        EnumWindows(CloseWindowOfMember, reinterpret_cast<LPARAM>(&request));
    }

    void Kill() override
    {
        if (!exit.has_value())
        {
            TerminateJobObject(job.Get(), 1);
        }
    }

    void Sample(const ProcessTable& table, UsageSample& out) override
    {
        UsageSample sample{};
        JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION accounting{};
        if (QueryInformationJobObject(job.Get(), JobObjectBasicAndIoAccountingInformation, &accounting,
                                      sizeof(accounting), nullptr))
        {
            const std::uint64_t hundredNs = static_cast<std::uint64_t>(accounting.BasicInfo.TotalUserTime.QuadPart) +
                                            static_cast<std::uint64_t>(accounting.BasicInfo.TotalKernelTime.QuadPart);
            sample.cpuTimeUsec = hundredNs / 10;
            sample.ioReadBytes = accounting.IoInfo.ReadTransferCount;
            sample.ioWriteBytes = accounting.IoInfo.WriteTransferCount;
        }

        const std::vector<DWORD> members = JobProcessIds(job.Get());
        sample.processCount = static_cast<int>(members.size());
        int handles = 0;
        bool handlesKnown = !members.empty();
        for (const DWORD member : members)
        {
            const ProcessEntry* entry = table.Find(static_cast<int>(member));
            if (entry != nullptr)
            {
                sample.threadCount += entry->threads;
            }
            const Handle opened{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, member)};
            if (!opened.Valid())
            {
                handlesKnown = false;
                continue;
            }
            PROCESS_MEMORY_COUNTERS counters{};
            if (GetProcessMemoryInfo(opened.Get(), &counters, sizeof(counters)))
            {
                sample.memoryBytes += counters.WorkingSetSize;
            }
            DWORD count = 0;
            if (GetProcessHandleCount(opened.Get(), &count))
            {
                handles += static_cast<int>(count);
            }
            else
            {
                handlesKnown = false;
            }
        }
        sample.openFiles = handlesKnown ? handles : -1;
        peakMemory = std::max(peakMemory, sample.memoryBytes);
        sample.memoryPeakBytes = peakMemory;
        out = sample;
    }

    std::vector<int> MemberPids(const ProcessTable& table) const override
    {
        static_cast<void>(table);
        std::vector<int> members{};
        for (const DWORD member : JobProcessIds(job.Get()))
        {
            members.push_back(static_cast<int>(member));
        }
        std::sort(members.begin(), members.end());
        return members;
    }

private:
    Handle process;
    Handle job;
    DWORD pid;
    std::optional<ExitStatus> exit;
    std::optional<ExitStatus> ended; // the main process's status, known before the job is empty
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
    static_cast<void>(cgroups);
    std::wstring path{};
    const LaunchCode resolved = ResolveBinary(request, path, error);
    if (resolved != LaunchCode::Ok)
    {
        return resolved;
    }
    const std::wstring workingDirectory = Widen(request.workingDirectory);
    if (!workingDirectory.empty() && !DirectoryExists(workingDirectory))
    {
        error = "working directory " + request.workingDirectory + " does not exist";
        return LaunchCode::WorkingDirectoryMissing;
    }

    std::wstring commandLine = QuoteArgument(path);
    for (const std::string& argument : request.arguments)
    {
        commandLine += L" " + QuoteArgument(Widen(argument));
    }
    std::vector<wchar_t> environment = BuildEnvironmentBlock(request.environment);

    const Handle input = OpenNul(GENERIC_READ);
    Handle output{nullptr};
    Handle errors{nullptr};
    if (request.output == OutputMode::File)
    {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        output = Handle{CreateFileW(Widen(request.outputPath).c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!output.Valid())
        {
            error = "cannot open " + request.outputPath + ": " + SystemErrorText(GetLastError());
            return LaunchCode::OutputFailed;
        }
    }
    else if (request.output == OutputMode::Null)
    {
        output = OpenNul(GENERIC_WRITE);
    }
    else
    {
        output = InheritableCopy(GetStdHandle(STD_OUTPUT_HANDLE));
        errors = InheritableCopy(GetStdHandle(STD_ERROR_HANDLE));
        if (!output.Valid())
        {
            output = OpenNul(GENERIC_WRITE);
        }
    }
    if (!input.Valid() || !output.Valid())
    {
        error = "cannot prepare standard handles: " + SystemErrorText(GetLastError());
        return LaunchCode::OutputFailed;
    }
    const HANDLE errorHandle = errors.Valid() ? errors.Get() : output.Get();

    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (!job.Valid())
    {
        error = "cannot create a job object: " + SystemErrorText(GetLastError());
        return LaunchCode::SpawnFailed;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (request.memoryMax > 0)
    {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = static_cast<SIZE_T>(request.memoryMax);
    }
    if (!SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        error = "cannot configure the job object: " + SystemErrorText(GetLastError());
        return LaunchCode::SpawnFailed;
    }
    if (request.cpuMaxPercent > 0)
    {
        const DWORD processors = std::max<DWORD>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1);
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
        rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        rate.CpuRate = std::clamp<DWORD>(request.cpuMaxPercent * 100 / processors, 1, 10000);
        if (!SetInformationJobObject(job.Get(), JobObjectCpuRateControlInformation, &rate, sizeof(rate)))
        {
            LogWarning("service " + request.serviceName + ": cpu_max not applied: " + SystemErrorText(GetLastError()));
        }
    }

    std::vector<HANDLE> inherited{input.Get(), output.Get()};
    if (errorHandle != output.Get())
    {
        inherited.push_back(errorHandle);
    }
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> attributeStorage{};
    attributeStorage.resize(attributeSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                   inherited.size() * sizeof(HANDLE), nullptr, nullptr))
    {
        error = "cannot restrict inherited handles: " + SystemErrorText(GetLastError());
        return LaunchCode::SpawnFailed;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.Get();
    startup.StartupInfo.hStdOutput = output.Get();
    startup.StartupInfo.hStdError = errorHandle;
    startup.lpAttributeList = attributes;

    PROCESS_INFORMATION created{};
    const DWORD flags =
        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    const BOOL started = CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags,
                                        environment.data(), workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                        &startup.StartupInfo, &created);
    const DWORD startError = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!started)
    {
        error = "cannot start " + Narrow(path) + ": " + SystemErrorText(startError);
        if (startError == ERROR_DIRECTORY)
        {
            return LaunchCode::WorkingDirectoryMissing;
        }
        if (startError == ERROR_FILE_NOT_FOUND || startError == ERROR_PATH_NOT_FOUND ||
            startError == ERROR_BAD_EXE_FORMAT)
        {
            return LaunchCode::BinaryNotFound;
        }
        return LaunchCode::SpawnFailed;
    }

    Handle process{created.hProcess};
    const Handle thread{created.hThread};
    if (!AssignProcessToJobObject(job.Get(), process.Get()))
    {
        error = "cannot put the process in its job object: " + SystemErrorText(GetLastError());
        TerminateProcess(process.Get(), 1);
        return LaunchCode::SpawnFailed;
    }
    ResumeThread(thread.Get());

    out = std::make_unique<WindowsChild>(process.Release(), job.Release(), created.dwProcessId);
    return LaunchCode::Ok;
}

} // namespace process_manager
