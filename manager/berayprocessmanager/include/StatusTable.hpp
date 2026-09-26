#pragma once

#include "DetailedReport.hpp"

#include <cstdint>
#include <string>

namespace process_manager
{

struct TableOptions
{
    bool wide{false}; // adds restart mode, threads, open files, I/O, heartbeat age and binary
};

class StatusTable
{
public:
    /**
     * @brief Create a table renderer.
     * @param options Which columns to show.
     */
    explicit StatusTable(TableOptions options);

    /**
     * @brief Render a report as plain ASCII text: a summary of the host, one line per GPU
     *        and one row per service.
     * @param report Report to render.
     * @return The text, ending in a newline.
     */
    std::string Render(const DetailedReport& report) const;

private:
    TableOptions options;
};

/**
 * @brief Format a byte count with a binary unit.
 * @param bytes Byte count.
 * @return For example "512 B", "1.5 KiB" or "210.3 MiB".
 */
std::string FormatBytes(std::uint64_t bytes);

/**
 * @brief Format a duration compactly.
 * @param nanoseconds Duration; negative durations read as zero.
 * @return For example "45s", "12m 05s", "3h 04m" or "2d 03h".
 */
std::string FormatDuration(std::int64_t nanoseconds);

/**
 * @brief Format a percentage with one decimal.
 * @param percent Value; negative means unknown.
 * @return For example "12.5", or "-" when unknown.
 */
std::string FormatPercent(double percent);

} // namespace process_manager
