#pragma once

#include "Logger.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace process_manager
{

enum class RouterMode
{
    Run,
    Help,
    Version,
};

struct RouterOptions
{
    RouterMode mode{RouterMode::Run};
    std::string endpoint; // empty: default_router_endpoint
    std::optional<LogLevel> logLevel;
};

enum class RouterParseCode
{
    Ok,
    UnknownOption,
    MissingValue,
    InvalidValue,
};

/**
 * @brief Parse the router's command line.
 * @param arguments Arguments without the program name.
 * @param out Receives the options on success.
 * @param error Receives a readable reason on failure.
 * @return RouterParseCode::Ok on success, otherwise the kind of problem.
 */
RouterParseCode ParseRouterCommandLine(std::span<const std::string> arguments, RouterOptions& out,
                                       std::string& error);

/**
 * @brief The router's --help text.
 * @param program Name the program was started as.
 * @return The text, ending in a newline.
 */
std::string RouterUsageText(std::string_view program);

} // namespace process_manager
