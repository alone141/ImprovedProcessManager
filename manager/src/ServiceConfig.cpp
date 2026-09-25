#include "ServiceConfig.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace process_manager
{

namespace
{

bool SameEnvironment(std::span<const EnvironmentVariable> a, std::span<const EnvironmentVariable> b)
{
    if (a.size() != b.size())
    {
        return false;
    }

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].name != b[i].name || a[i].value != b[i].value)
        {
            return false;
        }
    }
    return true;
}

bool IsNameCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '-';
}

} // namespace

bool SameServiceConfig(const ServiceConfig& a, const ServiceConfig& b)
{
    return a.name == b.name && a.description == b.description && a.binary == b.binary &&
           a.arguments == b.arguments && a.workingDirectory == b.workingDirectory &&
           SameEnvironment(a.environment, b.environment) && a.autostart == b.autostart && a.restart == b.restart &&
           a.restartDelay == b.restartDelay && a.restartDelayMax == b.restartDelayMax &&
           a.maxRestarts == b.maxRestarts && a.restartWindow == b.restartWindow && a.startGrace == b.startGrace &&
           a.stopSignal == b.stopSignal && a.stopTimeout == b.stopTimeout &&
           a.heartbeatInterval == b.heartbeatInterval && a.heartbeatTolerance == b.heartbeatTolerance &&
           a.unhealthyAction == b.unhealthyAction && a.dependsOn == b.dependsOn && a.output == b.output &&
           a.outputPath == b.outputPath && a.memoryMax == b.memoryMax && a.cpuMaxPercent == b.cpuMaxPercent;
}

const ServiceConfig* FindService(const Config& config, std::string_view name)
{
    for (const ServiceConfig& service : config.services)
    {
        if (service.name == name)
        {
            return &service;
        }
    }
    return nullptr;
}

bool IsValidServiceName(std::string_view name)
{
    if (name.empty() || name.size() > max_service_name_length || name == "." || name == "..")
    {
        return false;
    }

    for (const char c : name)
    {
        if (!IsNameCharacter(c))
        {
            return false;
        }
    }
    return true;
}

std::string_view RestartModeName(RestartMode mode)
{
    switch (mode)
    {
    case RestartMode::Never:
        return "never";
    case RestartMode::OnFailure:
        return "on-failure";
    case RestartMode::Always:
        return "always";
    }
    return "never";
}

std::optional<RestartMode> ParseRestartMode(std::string_view text)
{
    if (text == "never" || text == "no")
    {
        return RestartMode::Never;
    }
    if (text == "on-failure")
    {
        return RestartMode::OnFailure;
    }
    if (text == "always")
    {
        return RestartMode::Always;
    }
    return std::nullopt;
}

std::optional<RestartMode> RestartModeFromByte(std::uint8_t value)
{
    if (value > static_cast<std::uint8_t>(RestartMode::Always))
    {
        return std::nullopt;
    }

    return static_cast<RestartMode>(value);
}

std::string_view StopSignalName(StopSignal signal)
{
    switch (signal)
    {
    case StopSignal::Terminate:
        return "TERM";
    case StopSignal::Interrupt:
        return "INT";
    case StopSignal::Hangup:
        return "HUP";
    case StopSignal::Quit:
        return "QUIT";
    case StopSignal::Kill:
        return "KILL";
    case StopSignal::User1:
        return "USR1";
    case StopSignal::User2:
        return "USR2";
    }
    return "TERM";
}

std::optional<StopSignal> ParseStopSignal(std::string_view text)
{
    if (text.size() > 3 && text.substr(0, 3) == "SIG")
    {
        text.remove_prefix(3);
    }

    const StopSignal all[] = {StopSignal::Terminate, StopSignal::Interrupt, StopSignal::Hangup, StopSignal::Quit,
                              StopSignal::Kill,      StopSignal::User1,     StopSignal::User2};
    for (const StopSignal signal : all)
    {
        if (StopSignalName(signal) == text)
        {
            return signal;
        }
    }
    return std::nullopt;
}

} // namespace process_manager
