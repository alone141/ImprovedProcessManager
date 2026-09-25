#pragma once

#include "ProcessLauncher.hpp"

#include <memory>
#include <string>

namespace process_manager
{

class CgroupTree;

class NativeLauncher : public ProcessLauncher
{
public:
    /**
     * @brief Launch services with the operating system's own primitives: fork and exec
     *        into a new session on POSIX, one job object per service on Windows.
     * @param cgroups Task cgroup tree to run services in, or nullptr to track them by
     *        session only. Ignored on Windows. It must outlive the launcher.
     */
    explicit NativeLauncher(CgroupTree* cgroups);

    /**
     * @brief Start the main process of a service.
     * @param request What to run and how.
     * @param out Receives the running process on success.
     * @param error Receives a readable reason on failure.
     * @return LaunchCode::Ok once the program runs, otherwise the step that failed.
     */
    LaunchCode Launch(const LaunchRequest& request, std::unique_ptr<ChildProcess>& out,
                      std::string& error) override;

private:
    CgroupTree* cgroups;
};

} // namespace process_manager
