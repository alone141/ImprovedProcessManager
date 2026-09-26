#pragma once

#include "HealthRecord.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace process_manager
{

enum class ServiceState : std::uint8_t
{
    Stopped = 0,
    Waiting = 1,
    Starting = 2,
    Running = 3,
    Unhealthy = 4,
    Stopping = 5,
    Backoff = 6,
    Failed = 7,
};

/**
 * @brief Lower-case name of a state, as the status table prints it.
 * @param state State to name.
 * @return The name, for example "running".
 */
std::string_view ServiceStateName(ServiceState state);

/**
 * @brief Map a state onto the five states of the simplified health report.
 * @param state Detailed state.
 * @return The closest runtime state. Waiting and Backoff read as Starting, Stopping as
 *         Stopped and Failed as Unhealthy, so existing GUIs enable the right buttons.
 */
RuntimeState ToRuntimeState(ServiceState state);

/**
 * @brief Tell whether a service in this state has a live process.
 * @param state State to check.
 * @return true for Starting, Running, Unhealthy and Stopping.
 */
bool IsAlive(ServiceState state);

/**
 * @brief Map a wire byte to a state.
 * @param value Byte from the wire.
 * @return The state, or std::nullopt for a value outside the enum.
 */
std::optional<ServiceState> ServiceStateFromByte(std::uint8_t value);

} // namespace process_manager
