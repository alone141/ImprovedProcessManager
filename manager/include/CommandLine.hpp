#pragma once

#include "Logger.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace process_manager
{

enum class Mode
{
    Run,
    Status,
    Start,
    Stop,
    Restart,
    Reload,
    Heartbeat,
    Check,
    Help,
    Version,
};

struct Options
{
    Mode mode{Mode::Run};
    std::string serviceName;
    std::string configPath;
    bool configGiven{false};
    std::string commandEndpoint; // empty: taken from the configuration, or the default
    std::string reportEndpoint;  // empty: taken from the configuration, or the default
    std::string routerEndpoint;  // empty: commands go to the command endpoint, not through a router
    std::string managerIdentity; // with a router; empty: taken from the configuration, or the default
    std::chrono::milliseconds timeout{3000};
    bool timeoutGiven{false};
    bool watch{false};
    bool wide{false};
    std::optional<LogLevel> logLevel;
};

enum class ParseCode
{
    Ok,
    UnknownOption,
    MissingValue,
    ConflictingModes,
    InvalidValue,
};

/**
 * @brief Parse the command line.
 * @param arguments Arguments without the program name.
 * @param out Receives the options on success.
 * @param error Receives a readable reason on failure.
 * @return ParseCode::Ok on success, otherwise the kind of problem.
 */
ParseCode ParseCommandLine(std::span<const std::string> arguments, Options& out, std::string& error);

/**
 * @brief The --help text.
 * @param program Name the program was started as.
 * @return The text, ending in a newline.
 */
std::string UsageText(std::string_view program);

/**
 * @brief The configuration file used when --config is not given.
 * @return /etc/berayprocessmanager/services.conf on POSIX; services.conf next to the
 *         executable on Windows.
 */
std::string DefaultConfigPath();

/**
 * @brief Turn an endpoint the manager binds into one a client on the same host connects to.
 * @param bindEndpoint For example tcp://0.0.0.0:5557, which clients reach as tcp://127.0.0.1:5557.
 * @return For example tcp://127.0.0.1:5557; other endpoints are returned unchanged.
 */
std::string ConnectEndpoint(std::string_view bindEndpoint);

} // namespace process_manager
