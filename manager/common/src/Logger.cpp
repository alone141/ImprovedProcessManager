#include "Logger.hpp"
#include "Instant.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace process_manager
{

namespace
{

std::string_view LevelName(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Error:
        return "error";
    case LogLevel::Warning:
        return "warning";
    case LogLevel::Info:
        return "info";
    case LogLevel::Debug:
        return "debug";
    }
    return "info";
}

int JournalPriority(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Error:
        return 3;
    case LogLevel::Warning:
        return 4;
    case LogLevel::Info:
        return 6;
    case LogLevel::Debug:
        return 7;
    }
    return 6;
}

std::string LocalTimestamp(std::int64_t wallNs)
{
    const std::time_t seconds = static_cast<std::time_t>(wallNs / 1'000'000'000);
    const int millis = static_cast<int>((wallNs / 1'000'000) % 1000);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &seconds);
#else
    localtime_r(&seconds, &parts);
#endif
    // Sized for the longest output the format can produce with any int arguments
    // (77 bytes), so GCC's -Wformat-truncation has nothing to say under -Werror.
    char text[80]{};
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d.%03d", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec, millis);
    return std::string{text};
}

} // namespace

Logger::Logger(LogLevel level, bool journal)
    : level{level}, journal{journal}
{
}

void Logger::SetLevel(LogLevel level)
{
    this->level = level;
}

bool Logger::Enabled(LogLevel level) const
{
    return static_cast<int>(level) <= static_cast<int>(this->level);
}

void Logger::Write(LogLevel level, std::string_view message) const
{
    if (!Enabled(level))
    {
        return;
    }

    const std::string line = Format(level, message, Now().wallNs);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

std::string Logger::Format(LogLevel level, std::string_view message, std::int64_t wallNs) const
{
    std::string line{};
    if (journal)
    {
        line += "<" + std::to_string(JournalPriority(level)) + ">";
    }
    else
    {
        line += LocalTimestamp(wallNs);
        line += " [";
        line += LevelName(level);
        line += "] ";
    }
    line += message;
    line += "\n";
    return line;
}

Logger& GlobalLogger()
{
    static Logger logger{LogLevel::Info, std::getenv("JOURNAL_STREAM") != nullptr};
    return logger;
}

void LogError(std::string_view message)
{
    GlobalLogger().Write(LogLevel::Error, message);
}

void LogWarning(std::string_view message)
{
    GlobalLogger().Write(LogLevel::Warning, message);
}

void LogInfo(std::string_view message)
{
    GlobalLogger().Write(LogLevel::Info, message);
}

void LogDebug(std::string_view message)
{
    GlobalLogger().Write(LogLevel::Debug, message);
}

std::optional<LogLevel> ParseLogLevel(std::string_view text)
{
    if (text == "error")
    {
        return LogLevel::Error;
    }
    if (text == "warning")
    {
        return LogLevel::Warning;
    }
    if (text == "info")
    {
        return LogLevel::Info;
    }
    if (text == "debug")
    {
        return LogLevel::Debug;
    }
    return std::nullopt;
}

} // namespace process_manager
