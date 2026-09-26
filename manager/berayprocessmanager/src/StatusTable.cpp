#include "StatusTable.hpp"
#include "DetailedReport.hpp"
#include "ProcessLauncher.hpp"
#include "ServiceConfig.hpp"
#include "ServiceState.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace process_manager
{

namespace
{

constexpr const char* unknown_cell = "-";
constexpr std::int64_t ns_per_second = 1'000'000'000;

struct Column
{
    std::string header;
    bool right;
};

std::string Fixed(double value, int decimals)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.*f", decimals, value);
    return std::string{text};
}

std::string Pad(const std::string& text, std::size_t width, bool right)
{
    if (text.size() >= width)
    {
        return text;
    }

    std::string fill{};
    fill.append(width - text.size(), ' ');
    return right ? fill + text : text + fill;
}

bool Has(const ServiceRecord& service, std::uint8_t flag)
{
    return (service.flags & flag) != 0;
}

std::string StateText(const ServiceRecord& service, std::int64_t snapshot)
{
    std::string text{ServiceStateName(service.state)};
    if (service.state == ServiceState::Backoff && service.nextRestartTime > snapshot)
    {
        text += " " + FormatDuration(service.nextRestartTime - snapshot);
    }
    if (service.missedBeats > 0 && IsAlive(service.state))
    {
        text += " (" + std::to_string(service.missedBeats) + " missed)";
    }
    if (Has(service, service_flag_removing))
    {
        text += " (removing)";
    }
    return text;
}

std::string LastExitText(const ServiceRecord& service, std::int64_t snapshot, bool signals)
{
    if (service.lastExitTime == 0)
    {
        return unknown_cell;
    }

    return DescribeExit(service.lastExitCode, signals) + ", " + FormatDuration(snapshot - service.lastExitTime) +
           " ago";
}

std::vector<std::string> Row(const ServiceRecord& service, std::int64_t snapshot, bool wide, bool signals)
{
    const bool usage = Has(service, service_flag_usage);
    const bool gpu = Has(service, service_flag_gpu);
    const bool alive = IsAlive(service.state);
    std::vector<std::string> cells{};
    cells.push_back(service.name);
    cells.push_back(StateText(service, snapshot));
    cells.push_back(service.pid > 0 ? std::to_string(service.pid) : unknown_cell);
    cells.push_back(alive && service.startTime > 0 ? FormatDuration(snapshot - service.startTime) : unknown_cell);
    cells.push_back(std::to_string(service.restartCount));
    cells.push_back(usage ? FormatPercent(service.cpuPercent) : unknown_cell);
    cells.push_back(usage ? FormatBytes(service.memoryBytes) : unknown_cell);
    cells.push_back(gpu ? FormatPercent(service.gpuPercent) : unknown_cell);
    cells.push_back(gpu ? FormatBytes(service.gpuMemoryBytes) : unknown_cell);
    cells.push_back(LastExitText(service, snapshot, signals));
    if (wide)
    {
        cells.push_back(std::string{RestartModeName(service.restartMode)});
        cells.push_back(usage ? std::to_string(service.threadCount) : unknown_cell);
        cells.push_back(usage && service.openFiles >= 0 ? std::to_string(service.openFiles) : unknown_cell);
        cells.push_back(usage ? FormatBytes(service.ioReadBytes) : unknown_cell);
        cells.push_back(usage ? FormatBytes(service.ioWriteBytes) : unknown_cell);
        cells.push_back(Has(service, service_flag_heartbeat) && alive
                            ? FormatDuration(snapshot - service.lastSeen) + " ago"
                            : unknown_cell);
        cells.push_back(service.binary);
    }
    return cells;
}

void RenderSummary(const DetailedReport& report, std::ostringstream& out)
{
    const std::int64_t snapshot = report.snapshotTime;
    out << "berayprocessmanager " << report.managerVersion << " on " << (report.hostName.empty() ? "?" : report.hostName)
        << ", pid " << report.managerPid << ", up " << FormatDuration(snapshot - report.managerStartTime);
    if ((report.flags & report_flag_stopping) != 0)
    {
        out << ", shutting down";
    }
    out << "\n";

    const SystemRecord& system = report.system;
    const std::uint64_t used =
        system.memoryTotalBytes > system.memoryAvailableBytes ? system.memoryTotalBytes - system.memoryAvailableBytes : 0;
    out << "host: CPU " << FormatPercent(system.cpuPercent) << "% of " << system.cpuCount << " cores, memory "
        << FormatBytes(used) << " used of " << FormatBytes(system.memoryTotalBytes) << ", load "
        << Fixed(system.loadAverage1, 2) << " " << Fixed(system.loadAverage5, 2) << " "
        << Fixed(system.loadAverage15, 2) << "\n";
    for (const GpuRecord& gpu : report.gpus)
    {
        out << "gpu" << gpu.index << ": " << gpu.name << ", " << FormatPercent(gpu.utilizationPercent) << "% busy, "
            << FormatBytes(gpu.memoryUsedBytes) << " of " << FormatBytes(gpu.memoryTotalBytes);
        if (gpu.temperatureC > 0)
        {
            out << ", " << gpu.temperatureC << " C";
        }
        if (gpu.powerMilliwatts > 0)
        {
            out << ", " << Fixed(static_cast<double>(gpu.powerMilliwatts) / 1000.0, 1) << " W";
        }
        out << "\n";
    }
}

} // namespace

StatusTable::StatusTable(TableOptions options)
    : options{options}
{
}

std::string StatusTable::Render(const DetailedReport& report) const
{
    std::ostringstream out{};
    RenderSummary(report, out);
    out << "\n";
    if (report.services.empty())
    {
        out << "No services are configured.\n";
        return out.str();
    }

    std::vector<Column> columns{{"SERVICE", false}, {"STATE", false},  {"PID", true},    {"UPTIME", true},
                                {"RESTARTS", true}, {"CPU%", true},    {"MEMORY", true}, {"GPU%", true},
                                {"VRAM", true},     {"LAST EXIT", false}};
    if (options.wide)
    {
        const std::vector<Column> extra{{"MODE", false}, {"THREADS", true}, {"FILES", true}, {"READ", true},
                                        {"WRITE", true}, {"HEARTBEAT", true}, {"BINARY", false}};
        columns.insert(columns.end(), extra.begin(), extra.end());
    }

    std::vector<std::vector<std::string>> rows{};
    for (const ServiceRecord& service : report.services)
    {
        rows.push_back(Row(service, report.snapshotTime, options.wide, (report.flags & report_flag_windows) == 0));
    }
    std::vector<std::size_t> widths{};
    for (const Column& column : columns)
    {
        widths.push_back(column.header.size());
    }
    for (const std::vector<std::string>& row : rows)
    {
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i)
        {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }

    const auto writeLine = [&](std::span<const std::string> cells)
    {
        std::string line{};
        for (std::size_t i = 0; i < cells.size() && i < columns.size(); ++i)
        {
            const bool last = i + 1 == cells.size();
            line += last && !columns[i].right ? cells[i] : Pad(cells[i], widths[i], columns[i].right);
            if (!last)
            {
                line += "  ";
            }
        }
        out << line << "\n";
    };

    std::vector<std::string> headers{};
    for (const Column& column : columns)
    {
        headers.push_back(column.header);
    }
    writeLine(headers);
    for (const std::vector<std::string>& row : rows)
    {
        writeLine(row);
    }
    return out.str();
}

std::string FormatBytes(std::uint64_t bytes)
{
    if (bytes < 1024)
    {
        return std::to_string(bytes) + " B";
    }

    const char* units[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes) / 1024.0;
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0]))
    {
        value /= 1024.0;
        ++unit;
    }
    return Fixed(value, 1) + " " + units[unit];
}

std::string FormatDuration(std::int64_t nanoseconds)
{
    const std::int64_t seconds = std::max<std::int64_t>(nanoseconds, 0) / ns_per_second;
    char text[32]{};
    if (seconds < 60)
    {
        std::snprintf(text, sizeof(text), "%llds", static_cast<long long>(seconds));
    }
    else if (seconds < 3600)
    {
        std::snprintf(text, sizeof(text), "%lldm %02llds", static_cast<long long>(seconds / 60),
                      static_cast<long long>(seconds % 60));
    }
    else if (seconds < 86400)
    {
        std::snprintf(text, sizeof(text), "%lldh %02lldm", static_cast<long long>(seconds / 3600),
                      static_cast<long long>((seconds / 60) % 60));
    }
    else
    {
        std::snprintf(text, sizeof(text), "%lldd %02lldh", static_cast<long long>(seconds / 86400),
                      static_cast<long long>((seconds / 3600) % 24));
    }
    return std::string{text};
}

std::string FormatPercent(double percent)
{
    if (percent < 0.0)
    {
        return unknown_cell;
    }

    return Fixed(percent, 1);
}

} // namespace process_manager
