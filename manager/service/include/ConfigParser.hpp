#pragma once

#include "ServiceConfig.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace process_manager
{

enum class ConfigCode
{
    Ok,
    Unreadable,
    Syntax,
    UnknownSection,
    UnknownKey,
    DuplicateKey,
    InvalidValue,
    MissingValue,
    InvalidName,
    DuplicateService,
    UnknownDependency,
    DependencyCycle,
};

struct ConfigError
{
    ConfigCode code{ConfigCode::Ok};
    int line{0}; // 1-based; 0 when the problem is not on one line
    std::string message;
};

enum class SplitCode
{
    Ok,
    UnterminatedQuote,
};

class ConfigParser
{
public:
    /**
     * @brief Parse configuration text: a [manager] section, an optional [defaults]
     *        section and one [service NAME] section per service.
     * @param text File contents.
     * @param out Receives the configuration on success.
     * @param error Receives the first problem on failure.
     * @return ConfigCode::Ok on success, otherwise the kind of problem.
     */
    ConfigCode ParseText(std::string_view text, Config& out, ConfigError& error) const;

    /**
     * @brief Read and parse a configuration file.
     * @param path File to read. It is kept in Config::path for reloads.
     * @param out Receives the configuration on success.
     * @param error Receives the first problem on failure.
     * @return ConfigCode::Unreadable when the file cannot be read, otherwise as ParseText.
     */
    ConfigCode ParseFile(const std::string& path, Config& out, ConfigError& error) const;
};

/**
 * @brief Split an argument string like a shell, without any expansion.
 * @param text Arguments separated by blanks. Single quotes keep text as it is;
 *             double quotes also allow \" and \\. A backslash elsewhere is literal,
 *             so Windows paths need no quoting.
 * @param out Receives the arguments on success.
 * @return SplitCode::Ok, or SplitCode::UnterminatedQuote.
 */
SplitCode SplitArguments(std::string_view text, std::vector<std::string>& out);

/**
 * @brief Parse a byte size such as 512M, 1 GiB, 64KB or 1048576. Suffixes are powers of 1024.
 * @param text Size text.
 * @return The size in bytes, or std::nullopt when the text is not a size.
 */
std::optional<std::uint64_t> ParseByteSize(std::string_view text);

} // namespace process_manager
