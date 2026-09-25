#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace process_manager
{

struct GpuDevice
{
    std::uint32_t index{0};
    std::string name;
    std::string uuid;
    double utilizationPercent{-1.0};
    double memoryUtilizationPercent{-1.0};
    std::uint64_t memoryTotalBytes{0};
    std::uint64_t memoryUsedBytes{0};
    std::uint32_t temperatureC{0};
    std::uint32_t powerMilliwatts{0};
};

struct GpuProcessUsage
{
    double utilizationPercent{-1.0}; // negative when the driver has no per-process figure
    std::uint64_t memoryBytes{0};
};

struct GpuSnapshot
{
    bool available{false};
    std::vector<GpuDevice> devices;
    std::unordered_map<int, GpuProcessUsage> processes;
};

// One reading for one process on one device, before aggregation.
struct GpuProcessSample
{
    std::uint32_t device{0};
    int pid{0};
    bool hasMemory{false};
    std::uint64_t memoryBytes{0};
    bool hasUtilization{false};
    double utilizationPercent{0.0};
};

enum class GpuCode
{
    Ok,
    LibraryMissing,
    InitFailed,
};

// NVML comes with the NVIDIA driver and is loaded at run time, so building
// needs neither the CUDA toolkit nor NVIDIA headers, and hosts without an
// NVIDIA GPU simply report no GPU data.
class GpuMonitor
{
public:
    /**
     * @brief Create a monitor that has not loaded NVML yet.
     */
    GpuMonitor();

    /**
     * @brief Shut NVML down and unload it.
     */
    ~GpuMonitor();

    GpuMonitor(const GpuMonitor&) = delete;
    GpuMonitor& operator=(const GpuMonitor&) = delete;

    /**
     * @brief Take over another monitor's loaded library.
     * @param other Monitor to move from. It is left closed.
     */
    GpuMonitor(GpuMonitor&& other) noexcept;

    /**
     * @brief Take over another monitor's loaded library, closing this one's first.
     * @param other Monitor to move from. It is left closed.
     * @return This monitor.
     */
    GpuMonitor& operator=(GpuMonitor&& other) noexcept;

    /**
     * @brief Load NVML and initialise it.
     * @param error Receives the reason on failure.
     * @return GpuCode::Ok, GpuCode::LibraryMissing without the driver library, or
     *         GpuCode::InitFailed when NVML refuses to start.
     */
    GpuCode Open(std::string& error);

    /**
     * @brief Tell whether NVML is loaded.
     * @return true after a successful Open.
     */
    bool Available() const;

    /**
     * @brief Read every device and the GPU use of every process on them.
     * @return The snapshot; GpuSnapshot::available is false without NVML.
     */
    GpuSnapshot Sample();

private:
    struct Library;

    std::unique_ptr<Library> library;
};

/**
 * @brief Combine readings per process: the largest reading per device, summed over
 *        devices. The GUI's nvidia-smi sampler applies the same rule.
 * @param samples Readings from any number of devices.
 * @return Usage per PID. A process with memory readings only keeps a negative utilisation.
 */
std::unordered_map<int, GpuProcessUsage> AggregateGpuProcesses(std::span<const GpuProcessSample> samples);

} // namespace process_manager
