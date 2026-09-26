#pragma once

#include "WireReader.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace process_manager
{

// DEALER -> ROUTER: frame "BPM", then the 65-byte command.
// ROUTER -> DEALER: frame "BPM", then the 128-byte reply.
constexpr std::size_t command_message_size = 65;
constexpr std::size_t command_reply_size = 128;
constexpr std::size_t command_name_size = 32;
constexpr std::size_t command_args_size = 32;
constexpr std::size_t reply_message_size = 94;
constexpr std::string_view command_tag = "BPM";
constexpr std::string_view all_services = "*";

enum class CommandCode : std::uint8_t
{
    Start = 78,
    Stop = 79,
    Restart = 81,
    Heartbeat = 90,
    Reload = 91,
};

enum class CommandResult : std::uint8_t
{
    Ok = 0,
    UnknownService = 1,
    UnknownCommand = 2,
    Malformed = 3,
    AlreadyInState = 4,
    InvalidState = 5,
    LaunchFailed = 6,
    ShuttingDown = 7,
    ReloadFailed = 8,
};

struct CommandMessage
{
    std::uint8_t command{0};
    std::string serviceName;
    std::string args;
};

struct CommandReply
{
    std::uint8_t command{0};
    CommandResult result{CommandResult::Ok};
    std::string serviceName;
    std::string message;
};

/**
 * @brief Encode a command as the 65-byte wire struct.
 * @param message Command to encode. Text fields are cut to 31 bytes.
 * @return The payload frame.
 */
std::vector<std::uint8_t> EncodeCommand(const CommandMessage& message);

/**
 * @brief Decode the 65-byte command struct.
 * @param payload Payload frame.
 * @param out Receives the command on success.
 * @return DecodeCode::Ok, or DecodeCode::BadLength for any other size.
 */
DecodeCode DecodeCommand(std::span<const std::uint8_t> payload, CommandMessage& out);

/**
 * @brief Encode a reply as the 128-byte wire struct.
 * @param reply Reply to encode. The message is cut to 93 bytes.
 * @return The payload frame.
 */
std::vector<std::uint8_t> EncodeReply(const CommandReply& reply);

/**
 * @brief Decode the 128-byte reply struct.
 * @param payload Payload frame.
 * @param out Receives the reply on success.
 * @return DecodeCode::Ok, or DecodeCode::BadLength for any other size.
 */
DecodeCode DecodeReply(std::span<const std::uint8_t> payload, CommandReply& out);

/**
 * @brief Map a wire byte to a known command.
 * @param value Byte from the wire.
 * @return The command, or std::nullopt for an unknown value.
 */
std::optional<CommandCode> CommandCodeFromByte(std::uint8_t value);

/**
 * @brief Lower-case name of a command.
 * @param code Command to name.
 * @return The name, for example "restart".
 */
std::string_view CommandName(CommandCode code);

/**
 * @brief Short description of a result, as the CLI prints it.
 * @param result Result to describe.
 * @return The description, for example "unknown service".
 */
std::string_view CommandResultName(CommandResult result);

/**
 * @brief Tell whether a result means the request was carried out or needed nothing.
 * @param result Result to check.
 * @return true for CommandResult::Ok and CommandResult::AlreadyInState.
 */
bool IsSuccess(CommandResult result);

} // namespace process_manager
