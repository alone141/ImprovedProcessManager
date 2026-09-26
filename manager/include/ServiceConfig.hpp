#pragma once

#include "Logger.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace process_manager
{

// A name must fit the 32-byte ServiceName field of a command, NUL included.
constexpr std::size_t max_service_name_length = 31;

enum class RestartMode : std::uint8_t
{
    Never = 0,
    OnFailure = 1,
    Always = 2,
};

enum class OutputMode : std::uint8_t
{
    Auto,
    Journal,
    Inherit,
    Null,
    File,
};

enum class StopSignal : std::uint8_t
{
    Terminate,
    Interrupt,
    Hangup,
    Quit,
    Kill,
    User1,
    User2,
};

enum class UnhealthyAction : std::uint8_t
{
    None,
    Restart,
};

enum class CgroupMode : std::uint8_t
{
    Auto,
    Off,
    Required,
};

enum class GpuMode : std::uint8_t
{
    Auto,
    Off,
};

struct EnvironmentVariable
{
    std::string name;
    std::string value;
};

struct ServiceConfig
{
    std::string name;
    std::string description;
    std::string binary;
    std::vector<std::string> arguments;
    std::string workingDirectory;
    std::vector<EnvironmentVariable> environment;
    bool autostart{true};
    RestartMode restart{RestartMode::OnFailure};
    std::chrono::milliseconds restartDelay{1000};
    std::chrono::milliseconds restartDelayMax{30000};
    int maxRestarts{5};
    std::chrono::seconds restartWindow{60};
    std::chrono::milliseconds startGrace{1000};
    StopSignal stopSignal{StopSignal::Terminate};
    std::chrono::milliseconds stopTimeout{5000};
    std::chrono::milliseconds heartbeatInterval{0};
    int heartbeatTolerance{3};
    UnhealthyAction unhealthyAction{UnhealthyAction::None};
    std::vector<std::string> dependsOn;
    OutputMode output{OutputMode::Auto};
    std::string outputPath;
    std::uint64_t memoryMax{0};
    std::uint32_t cpuMaxPercent{0};
};

// The manager's name on a router when the configuration gives none.
constexpr std::string_view default_manager_identity = "berayprocessmanager";

struct ManagerSettings
{
    std::string healthEndpoint{"tcp://*:6667"};
    std::string reportEndpoint{"tcp://*:6668"};
    std::string commandEndpoint{"tcp://*:5557"};
    std::string routerEndpoint;                     // empty: commands arrive on the ROUTER only
    std::string identity{default_manager_identity}; // the manager's name on the router
    std::chrono::milliseconds publishInterval{1000};
    LogLevel logLevel{LogLevel::Info};
    CgroupMode cgroups{CgroupMode::Auto};
    GpuMode gpu{GpuMode::Auto};
};

struct Config
{
    std::string path;
    ManagerSettings manager;
    std::vector<ServiceConfig> services;
};

/**
 * @brief Compare two service configurations field by field.
 * @param a First configuration.
 * @param b Second configuration.
 * @return true when every field matches.
 */
bool SameServiceConfig(const ServiceConfig& a, const ServiceConfig& b);

// A pointer, so callers read the stored entry without copying it.
/**
 * @brief Find a service by name.
 * @param config Configuration to search.
 * @param name Service name.
 * @return The service, or nullptr when there is none by that name.
 */
const ServiceConfig* FindService(const Config& config, std::string_view name);

/**
 * @brief Tell whether text is a valid service name: 1 to 31 of A-Z a-z 0-9 _ . -
 * @param name Candidate name. "." and ".." are rejected.
 * @return true when the name is valid.
 */
bool IsValidServiceName(std::string_view name);

/**
 * @brief Tell whether text can be an identity on a router: 1 to 255 printable ASCII
 *        characters without spaces, so it fits a libzmq identity and reads well in logs.
 * @param identity Candidate identity.
 * @return true when the identity is valid.
 */
bool IsValidIdentity(std::string_view identity);

/**
 * @brief Name of a restart mode as the configuration spells it.
 * @param mode Mode to name.
 * @return "never", "on-failure" or "always".
 */
std::string_view RestartModeName(RestartMode mode);

/**
 * @brief Parse a restart mode.
 * @param text "never" (or "no"), "on-failure" or "always".
 * @return The mode, or std::nullopt for any other text.
 */
std::optional<RestartMode> ParseRestartMode(std::string_view text);

/**
 * @brief Map a wire byte to a restart mode.
 * @param value Byte from the wire.
 * @return The mode, or std::nullopt for a value outside the enum.
 */
std::optional<RestartMode> RestartModeFromByte(std::uint8_t value);

/**
 * @brief Name of a stop signal as the configuration spells it.
 * @param signal Signal to name.
 * @return The name without the SIG prefix, for example "TERM".
 */
std::string_view StopSignalName(StopSignal signal);

/**
 * @brief Parse a stop signal.
 * @param text Signal name with or without the SIG prefix, for example "TERM" or "SIGINT".
 * @return The signal, or std::nullopt for an unsupported name.
 */
std::optional<StopSignal> ParseStopSignal(std::string_view text);

} // namespace process_manager
