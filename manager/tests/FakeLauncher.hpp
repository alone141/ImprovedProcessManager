#pragma once

#include "ProcessLauncher.hpp"
#include "ProcessTable.hpp"
#include "ServiceConfig.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace process_manager_test
{

// What a fake process did and will do; the test drives it.
struct FakeProcess
{
    int pid{0};
    bool stopRequested{false};
    process_manager::StopSignal lastSignal{process_manager::StopSignal::Terminate};
    bool killed{false};
    bool exitOnStop{true};
    std::optional<process_manager::ExitStatus> exit{};
    process_manager::UsageSample usage{};
    std::vector<int> members{};
};

class FakeChild : public process_manager::ChildProcess
{
public:
    explicit FakeChild(std::shared_ptr<FakeProcess> process)
        : process{std::move(process)}
    {
    }

    int Pid() const override
    {
        return process->pid;
    }

    std::optional<process_manager::ExitStatus> Poll() override
    {
        return process->exit;
    }

    void RequestStop(process_manager::StopSignal signal) override
    {
        process->stopRequested = true;
        process->lastSignal = signal;
        if (process->exitOnStop)
        {
            process->exit = process_manager::ExitStatus{0, 15};
        }
    }

    void Kill() override
    {
        process->killed = true;
        process->exit = process_manager::ExitStatus{0, 9};
    }

    void Sample(const process_manager::ProcessTable& table, process_manager::UsageSample& out) override
    {
        static_cast<void>(table);
        out = process->usage;
    }

    std::vector<int> MemberPids(const process_manager::ProcessTable& table) const override
    {
        static_cast<void>(table);
        return process->members.empty() ? std::vector<int>{process->pid} : process->members;
    }

private:
    std::shared_ptr<FakeProcess> process;
};

class FakeLauncher : public process_manager::ProcessLauncher
{
public:
    process_manager::LaunchCode Launch(const process_manager::LaunchRequest& request,
                                       std::unique_ptr<process_manager::ChildProcess>& out,
                                       std::string& error) override
    {
        requests.push_back(request);
        if (nextCode != process_manager::LaunchCode::Ok)
        {
            error = "fake launch failure";
            return nextCode;
        }

        std::shared_ptr<FakeProcess> process = std::make_shared<FakeProcess>();
        process->pid = nextPid++;
        process->exitOnStop = exitOnStop;
        processes.push_back(process);
        out = std::make_unique<FakeChild>(process);
        return process_manager::LaunchCode::Ok;
    }

    std::shared_ptr<FakeProcess> Last() const
    {
        return processes.empty() ? nullptr : processes.back();
    }

    std::vector<process_manager::LaunchRequest> requests{};
    std::vector<std::shared_ptr<FakeProcess>> processes{};
    process_manager::LaunchCode nextCode{process_manager::LaunchCode::Ok};
    bool exitOnStop{true};
    int nextPid{1000};
};

inline process_manager::ServiceConfig MakeService(const std::string& name)
{
    process_manager::ServiceConfig config{};
    config.name = name;
    config.binary = "/bin/" + name;
    return config;
}

} // namespace process_manager_test
