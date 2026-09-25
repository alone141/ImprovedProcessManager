#include "ServiceState.hpp"
#include "HealthRecord.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace process_manager
{

std::string_view ServiceStateName(ServiceState state)
{
    switch (state)
    {
    case ServiceState::Stopped:
        return "stopped";
    case ServiceState::Waiting:
        return "waiting";
    case ServiceState::Starting:
        return "starting";
    case ServiceState::Running:
        return "running";
    case ServiceState::Unhealthy:
        return "unhealthy";
    case ServiceState::Stopping:
        return "stopping";
    case ServiceState::Backoff:
        return "backoff";
    case ServiceState::Failed:
        return "failed";
    }
    return "unknown";
}

RuntimeState ToRuntimeState(ServiceState state)
{
    switch (state)
    {
    case ServiceState::Stopped:
    case ServiceState::Stopping:
        return RuntimeState::Stopped;
    case ServiceState::Waiting:
    case ServiceState::Starting:
    case ServiceState::Backoff:
        return RuntimeState::Starting;
    case ServiceState::Running:
        return RuntimeState::Running;
    case ServiceState::Unhealthy:
    case ServiceState::Failed:
        return RuntimeState::Unhealthy;
    }
    return RuntimeState::Unknown;
}

bool IsAlive(ServiceState state)
{
    return state == ServiceState::Starting || state == ServiceState::Running ||
           state == ServiceState::Unhealthy || state == ServiceState::Stopping;
}

std::optional<ServiceState> ServiceStateFromByte(std::uint8_t value)
{
    if (value > static_cast<std::uint8_t>(ServiceState::Failed))
    {
        return std::nullopt;
    }

    return static_cast<ServiceState>(value);
}

} // namespace process_manager
