#pragma once

#include "DetailedReport.hpp"
#include "GpuMonitor.hpp"
#include "HealthRecord.hpp"
#include "Service.hpp"
#include "SystemMonitor.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace process_manager
{

class ReportBuilder
{
public:
    /**
     * @brief Build reports on behalf of one manager process.
     * @param managerPid PID of the manager.
     * @param managerStartTime Wall time the manager started, in ns since the Unix epoch.
     */
    ReportBuilder(std::int32_t managerPid, std::int64_t managerStartTime);

    /**
     * @brief Build the simplified health report the GUI reads.
     * @param statuses One status per service.
     * @param snapshotTime Wall time of the snapshot, in ns since the Unix epoch.
     * @return One record per service. Detailed states are folded into the five legacy
     *         ones, and PID and start time are zero unless a process is alive.
     */
    std::vector<HealthRecord> BuildHealth(std::span<const ServiceStatus> statuses, std::int64_t snapshotTime) const;

    /**
     * @brief Build the detailed report.
     * @param statuses One status per service.
     * @param system Host figures.
     * @param gpu Device figures; no GPU records without NVML.
     * @param snapshotTime Wall time of the snapshot, in ns since the Unix epoch.
     * @param publishIntervalMs Time between reports.
     * @param flags report_flag_* bits.
     * @return The report.
     */
    DetailedReport BuildReport(std::span<const ServiceStatus> statuses, const SystemSnapshot& system,
                               const GpuSnapshot& gpu, std::int64_t snapshotTime, std::uint32_t publishIntervalMs,
                               std::uint32_t flags) const;

private:
    std::int32_t managerPid;
    std::int64_t managerStartTime;
};

} // namespace process_manager
