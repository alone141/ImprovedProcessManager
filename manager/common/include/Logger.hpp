#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace process_manager
{

enum class LogLevel
{
    Error,
    Warning,
    Info,
    Debug,
};

class Logger
{
public:
    /**
     * @brief Create a logger that writes to standard error.
     * @param level Most verbose level that is written.
     * @param journal Prefix journald priorities instead of timestamps.
     */
    Logger(LogLevel level, bool journal);

    /**
     * @brief Change the most verbose level that is written.
     * @param level New threshold.
     */
    void SetLevel(LogLevel level);

    /**
     * @brief Tell whether a message at @p level would be written.
     * @param level Level to check.
     * @return true when @p level is at or below the threshold.
     */
    bool Enabled(LogLevel level) const;

    /**
     * @brief Write one message when its level is enabled.
     * @param level Message level.
     * @param message Text without a trailing newline.
     */
    void Write(LogLevel level, std::string_view message) const;

    /**
     * @brief Format one line the way Write prints it.
     * @param level Message level.
     * @param message Text without a trailing newline.
     * @param wallNs Timestamp in nanoseconds since the Unix epoch. Unused with journald prefixes.
     * @return The line, ending in a newline.
     */
    std::string Format(LogLevel level, std::string_view message, std::int64_t wallNs) const;

private:
    LogLevel level;
    bool journal;
};

/**
 * @brief The process-wide logger.
 * @return The shared logger. It starts at LogLevel::Info and uses journald
 *         prefixes when the JOURNAL_STREAM variable is set.
 */
Logger& GlobalLogger();

/**
 * @brief Write an error through the process-wide logger.
 * @param message Text without a trailing newline.
 */
void LogError(std::string_view message);

/**
 * @brief Write a warning through the process-wide logger.
 * @param message Text without a trailing newline.
 */
void LogWarning(std::string_view message);

/**
 * @brief Write an informational message through the process-wide logger.
 * @param message Text without a trailing newline.
 */
void LogInfo(std::string_view message);

/**
 * @brief Write a debug message through the process-wide logger.
 * @param message Text without a trailing newline.
 */
void LogDebug(std::string_view message);

/**
 * @brief Parse a level name.
 * @param text One of error, warning, info or debug.
 * @return The level, or std::nullopt for any other text.
 */
std::optional<LogLevel> ParseLogLevel(std::string_view text);

} // namespace process_manager
