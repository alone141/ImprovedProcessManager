#include "GpuMonitor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The platform's dynamic loader: LoadLibrary on Windows, dlopen elsewhere.
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace process_manager
{

namespace
{

// The few NVML declarations used here, as nvml.h defines them.
using NvmlReturn = int;
using NvmlDevice = void*;
constexpr NvmlReturn nvml_success = 0;
constexpr NvmlReturn nvml_insufficient_size = 7;
constexpr unsigned int nvml_temperature_gpu = 0;
constexpr unsigned int nvml_text_size = 96;
constexpr unsigned long long nvml_value_not_available = ~0ULL;
constexpr std::size_t process_info_v1_size = 16;
constexpr std::size_t process_info_v2_size = 24;
constexpr std::size_t process_memory_offset = 8;
constexpr unsigned int process_slack = 8;

struct NvmlUtilization
{
    unsigned int gpu;
    unsigned int memory;
};

struct NvmlMemory
{
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct NvmlProcessUtilizationSample
{
    unsigned int pid;
    unsigned long long timeStamp;
    unsigned int smUtil;
    unsigned int memUtil;
    unsigned int encUtil;
    unsigned int decUtil;
};

using PlainFunction = NvmlReturn (*)();
using CountFunction = NvmlReturn (*)(unsigned int*);
using HandleFunction = NvmlReturn (*)(unsigned int, NvmlDevice*);
using TextFunction = NvmlReturn (*)(NvmlDevice, char*, unsigned int);
using UtilizationFunction = NvmlReturn (*)(NvmlDevice, NvmlUtilization*);
using MemoryFunction = NvmlReturn (*)(NvmlDevice, NvmlMemory*);
using TemperatureFunction = NvmlReturn (*)(NvmlDevice, unsigned int, unsigned int*);
using PowerFunction = NvmlReturn (*)(NvmlDevice, unsigned int*);
using ProcessesFunction = NvmlReturn (*)(NvmlDevice, unsigned int*, void*);
using ProcessUtilizationFunction =
    NvmlReturn (*)(NvmlDevice, NvmlProcessUtilizationSample*, unsigned int*, unsigned long long);

void* OpenLibrary()
{
#ifdef _WIN32
    HMODULE module = LoadLibraryW(L"nvml.dll");
    if (module == nullptr)
    {
        wchar_t programFiles[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
        if (length > 0 && length < MAX_PATH)
        {
            const std::wstring path = std::wstring{programFiles} + L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
            module = LoadLibraryW(path.c_str());
        }
    }
    return reinterpret_cast<void*>(module);
#else
    void* handle = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        handle = dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_LOCAL);
    }
    return handle;
#endif
}

void CloseLibrary(void* handle)
{
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

template <typename Function>
Function Resolve(void* handle, const char* name)
{
#ifdef _WIN32
    return reinterpret_cast<Function>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return reinterpret_cast<Function>(dlsym(handle, name));
#endif
}

std::string Text(TextFunction function, NvmlDevice device)
{
    char text[nvml_text_size]{};
    if (function == nullptr || function(device, text, nvml_text_size) != nvml_success)
    {
        return std::string{};
    }
    text[nvml_text_size - 1] = '\0';
    return std::string{text};
}

} // namespace

struct GpuMonitor::Library
{
    void* handle{nullptr};
    PlainFunction shutdown{nullptr};
    CountFunction count{nullptr};
    HandleFunction handleByIndex{nullptr};
    TextFunction name{nullptr};
    TextFunction uuid{nullptr};
    UtilizationFunction utilization{nullptr};
    MemoryFunction memory{nullptr};
    TemperatureFunction temperature{nullptr};
    PowerFunction power{nullptr};
    ProcessesFunction computeProcesses{nullptr};
    ProcessesFunction graphicsProcesses{nullptr};
    std::size_t processInfoSize{0};
    ProcessUtilizationFunction processUtilization{nullptr};
    std::map<std::uint32_t, unsigned long long> lastSampleTime{};

    void CollectProcesses(ProcessesFunction function, NvmlDevice device, std::uint32_t index,
                          std::vector<GpuProcessSample>& samples) const
    {
        if (function == nullptr)
        {
            return;
        }

        unsigned int count = 0;
        const NvmlReturn probe = function(device, &count, nullptr);
        if ((probe != nvml_success && probe != nvml_insufficient_size) || count == 0)
        {
            return;
        }
        count += process_slack;
        std::vector<unsigned char> buffer{};
        buffer.resize(static_cast<std::size_t>(count) * processInfoSize);
        if (function(device, &count, buffer.data()) != nvml_success)
        {
            return;
        }

        for (unsigned int i = 0; i < count; ++i)
        {
            unsigned int pid = 0;
            unsigned long long used = 0;
            std::memcpy(&pid, buffer.data() + i * processInfoSize, sizeof(pid));
            std::memcpy(&used, buffer.data() + i * processInfoSize + process_memory_offset, sizeof(used));
            GpuProcessSample sample{};
            sample.device = index;
            sample.pid = static_cast<int>(pid);
            sample.hasMemory = used != nvml_value_not_available;
            sample.memoryBytes = sample.hasMemory ? used : 0;
            samples.push_back(sample);
        }
    }

    void CollectUtilization(NvmlDevice device, std::uint32_t index, std::vector<GpuProcessSample>& samples)
    {
        if (processUtilization == nullptr)
        {
            return;
        }

        const unsigned long long since = lastSampleTime[index];
        unsigned int count = 0;
        const NvmlReturn probe = processUtilization(device, nullptr, &count, since);
        if ((probe != nvml_success && probe != nvml_insufficient_size) || count == 0)
        {
            return;
        }
        std::vector<NvmlProcessUtilizationSample> buffer{};
        buffer.resize(count);
        if (processUtilization(device, buffer.data(), &count, since) != nvml_success)
        {
            return;
        }

        unsigned long long newest = since;
        for (unsigned int i = 0; i < count && i < buffer.size(); ++i)
        {
            newest = std::max(newest, buffer[i].timeStamp);
            GpuProcessSample sample{};
            sample.device = index;
            sample.pid = static_cast<int>(buffer[i].pid);
            sample.hasUtilization = true;
            sample.utilizationPercent = static_cast<double>(buffer[i].smUtil);
            samples.push_back(sample);
        }
        lastSampleTime[index] = newest;
    }
};

GpuMonitor::GpuMonitor()
    : library{}
{
}

GpuMonitor::~GpuMonitor()
{
    if (library != nullptr && library->handle != nullptr)
    {
        if (library->shutdown != nullptr)
        {
            library->shutdown();
        }
        CloseLibrary(library->handle);
    }
}

GpuMonitor::GpuMonitor(GpuMonitor&& other) noexcept
    : library{std::move(other.library)}
{
}

GpuMonitor& GpuMonitor::operator=(GpuMonitor&& other) noexcept
{
    if (this != &other)
    {
        GpuMonitor closing{std::move(*this)};
        library = std::move(other.library);
    }
    return *this;
}

GpuCode GpuMonitor::Open(std::string& error)
{
    std::unique_ptr<Library> loaded = std::make_unique<Library>();
    loaded->handle = OpenLibrary();
    if (loaded->handle == nullptr)
    {
        error = "NVML was not found (it comes with the NVIDIA driver)";
        return GpuCode::LibraryMissing;
    }

    void* handle = loaded->handle;
    PlainFunction init = Resolve<PlainFunction>(handle, "nvmlInit_v2");
    if (init == nullptr)
    {
        init = Resolve<PlainFunction>(handle, "nvmlInit");
    }
    loaded->shutdown = Resolve<PlainFunction>(handle, "nvmlShutdown");
    loaded->count = Resolve<CountFunction>(handle, "nvmlDeviceGetCount_v2");
    if (loaded->count == nullptr)
    {
        loaded->count = Resolve<CountFunction>(handle, "nvmlDeviceGetCount");
    }
    loaded->handleByIndex = Resolve<HandleFunction>(handle, "nvmlDeviceGetHandleByIndex_v2");
    if (loaded->handleByIndex == nullptr)
    {
        loaded->handleByIndex = Resolve<HandleFunction>(handle, "nvmlDeviceGetHandleByIndex");
    }
    loaded->name = Resolve<TextFunction>(handle, "nvmlDeviceGetName");
    loaded->uuid = Resolve<TextFunction>(handle, "nvmlDeviceGetUUID");
    loaded->utilization = Resolve<UtilizationFunction>(handle, "nvmlDeviceGetUtilizationRates");
    loaded->memory = Resolve<MemoryFunction>(handle, "nvmlDeviceGetMemoryInfo");
    loaded->temperature = Resolve<TemperatureFunction>(handle, "nvmlDeviceGetTemperature");
    loaded->power = Resolve<PowerFunction>(handle, "nvmlDeviceGetPowerUsage");
    loaded->processUtilization = Resolve<ProcessUtilizationFunction>(handle, "nvmlDeviceGetProcessUtilization");

    // Newer drivers add versioned entry points with a 24-byte record; the plain names keep the 16-byte one.
    const char* versions[][2] = {
        {"nvmlDeviceGetComputeRunningProcesses_v3", "nvmlDeviceGetGraphicsRunningProcesses_v3"},
        {"nvmlDeviceGetComputeRunningProcesses_v2", "nvmlDeviceGetGraphicsRunningProcesses_v2"},
        {"nvmlDeviceGetComputeRunningProcesses", "nvmlDeviceGetGraphicsRunningProcesses"}};
    const std::size_t sizes[] = {process_info_v2_size, process_info_v2_size, process_info_v1_size};
    for (std::size_t i = 0; i < 3 && loaded->computeProcesses == nullptr; ++i)
    {
        loaded->computeProcesses = Resolve<ProcessesFunction>(handle, versions[i][0]);
        loaded->graphicsProcesses = Resolve<ProcessesFunction>(handle, versions[i][1]);
        loaded->processInfoSize = sizes[i];
    }

    if (init == nullptr || loaded->count == nullptr || loaded->handleByIndex == nullptr)
    {
        CloseLibrary(handle);
        error = "the NVML library lacks the functions this manager needs";
        return GpuCode::LibraryMissing;
    }
    const NvmlReturn started = init();
    if (started != nvml_success)
    {
        CloseLibrary(handle);
        error = "NVML failed to start (code " + std::to_string(started) + ")";
        return GpuCode::InitFailed;
    }

    GpuMonitor closing{std::move(*this)};
    library = std::move(loaded);
    return GpuCode::Ok;
}

bool GpuMonitor::Available() const
{
    return library != nullptr;
}

GpuSnapshot GpuMonitor::Sample()
{
    GpuSnapshot snapshot{};
    unsigned int count = 0;
    if (library == nullptr || library->count(&count) != nvml_success)
    {
        return snapshot;
    }

    snapshot.available = true;
    std::vector<GpuProcessSample> samples{};
    for (unsigned int index = 0; index < count; ++index)
    {
        NvmlDevice device = nullptr;
        if (library->handleByIndex(index, &device) != nvml_success)
        {
            continue;
        }

        GpuDevice info{};
        info.index = index;
        info.name = Text(library->name, device);
        info.uuid = Text(library->uuid, device);
        NvmlUtilization utilization{};
        if (library->utilization != nullptr && library->utilization(device, &utilization) == nvml_success)
        {
            info.utilizationPercent = static_cast<double>(utilization.gpu);
            info.memoryUtilizationPercent = static_cast<double>(utilization.memory);
        }
        NvmlMemory memory{};
        if (library->memory != nullptr && library->memory(device, &memory) == nvml_success)
        {
            info.memoryTotalBytes = memory.total;
            info.memoryUsedBytes = memory.used;
        }
        unsigned int temperature = 0;
        if (library->temperature != nullptr &&
            library->temperature(device, nvml_temperature_gpu, &temperature) == nvml_success)
        {
            info.temperatureC = temperature;
        }
        unsigned int milliwatts = 0;
        if (library->power != nullptr && library->power(device, &milliwatts) == nvml_success)
        {
            info.powerMilliwatts = milliwatts;
        }
        snapshot.devices.push_back(info);

        library->CollectProcesses(library->computeProcesses, device, index, samples);
        library->CollectProcesses(library->graphicsProcesses, device, index, samples);
        library->CollectUtilization(device, index, samples);
    }
    snapshot.processes = AggregateGpuProcesses(samples);
    return snapshot;
}

std::unordered_map<int, GpuProcessUsage> AggregateGpuProcesses(std::span<const GpuProcessSample> samples)
{
    std::map<std::pair<std::uint32_t, int>, GpuProcessSample> perDevice{};
    for (const GpuProcessSample& sample : samples)
    {
        if (sample.pid <= 0)
        {
            continue;
        }
        GpuProcessSample& entry = perDevice[std::make_pair(sample.device, sample.pid)];
        entry.device = sample.device;
        entry.pid = sample.pid;
        if (sample.hasMemory)
        {
            entry.memoryBytes = entry.hasMemory ? std::max(entry.memoryBytes, sample.memoryBytes) : sample.memoryBytes;
            entry.hasMemory = true;
        }
        if (sample.hasUtilization)
        {
            entry.utilizationPercent = entry.hasUtilization
                                           ? std::max(entry.utilizationPercent, sample.utilizationPercent)
                                           : sample.utilizationPercent;
            entry.hasUtilization = true;
        }
    }

    std::unordered_map<int, GpuProcessUsage> usage{};
    for (const std::pair<const std::pair<std::uint32_t, int>, GpuProcessSample>& item : perDevice)
    {
        const GpuProcessSample& entry = item.second;
        GpuProcessUsage& total = usage[entry.pid];
        if (entry.hasMemory)
        {
            total.memoryBytes += entry.memoryBytes;
        }
        if (entry.hasUtilization)
        {
            total.utilizationPercent = std::max(total.utilizationPercent, 0.0) + entry.utilizationPercent;
        }
    }
    return usage;
}

} // namespace process_manager
