#include "CommandMessage.hpp"
#include "WireReader.hpp"
#include "WireWriter.hpp"

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

constexpr std::size_t command_code_offset = 0;
constexpr std::size_t command_name_offset = 1;
constexpr std::size_t command_args_offset = 33;
constexpr std::size_t reply_code_offset = 0;
constexpr std::size_t reply_result_offset = 1;
constexpr std::size_t reply_name_offset = 2;
constexpr std::size_t reply_message_offset = 34;

CommandResult CommandResultFromByte(std::uint8_t value)
{
    if (value > static_cast<std::uint8_t>(CommandResult::ReloadFailed))
    {
        return CommandResult::Malformed;
    }

    return static_cast<CommandResult>(value);
}

} // namespace

std::vector<std::uint8_t> EncodeCommand(const CommandMessage& message)
{
    WireWriter writer{command_message_size};
    writer.PutU8(command_code_offset, message.command);
    writer.PutText(command_name_offset, command_name_size, message.serviceName);
    writer.PutText(command_args_offset, command_args_size, message.args);
    return writer.TakeBytes();
}

DecodeCode DecodeCommand(std::span<const std::uint8_t> payload, CommandMessage& out)
{
    if (payload.size() != command_message_size)
    {
        return DecodeCode::BadLength;
    }

    const WireReader reader{payload, 0, command_message_size};
    out.command = reader.U8(command_code_offset);
    out.serviceName = reader.Text(command_name_offset, command_name_size);
    out.args = reader.Text(command_args_offset, command_args_size);
    return DecodeCode::Ok;
}

std::vector<std::uint8_t> EncodeReply(const CommandReply& reply)
{
    WireWriter writer{command_reply_size};
    writer.PutU8(reply_code_offset, reply.command);
    writer.PutU8(reply_result_offset, static_cast<std::uint8_t>(reply.result));
    writer.PutText(reply_name_offset, command_name_size, reply.serviceName);
    writer.PutText(reply_message_offset, reply_message_size, reply.message);
    return writer.TakeBytes();
}

DecodeCode DecodeReply(std::span<const std::uint8_t> payload, CommandReply& out)
{
    if (payload.size() != command_reply_size)
    {
        return DecodeCode::BadLength;
    }

    const WireReader reader{payload, 0, command_reply_size};
    out.command = reader.U8(reply_code_offset);
    out.result = CommandResultFromByte(reader.U8(reply_result_offset));
    out.serviceName = reader.Text(reply_name_offset, command_name_size);
    out.message = reader.Text(reply_message_offset, reply_message_size);
    return DecodeCode::Ok;
}

std::optional<CommandCode> CommandCodeFromByte(std::uint8_t value)
{
    switch (value)
    {
    case static_cast<std::uint8_t>(CommandCode::Start):
        return CommandCode::Start;
    case static_cast<std::uint8_t>(CommandCode::Stop):
        return CommandCode::Stop;
    case static_cast<std::uint8_t>(CommandCode::Restart):
        return CommandCode::Restart;
    case static_cast<std::uint8_t>(CommandCode::Heartbeat):
        return CommandCode::Heartbeat;
    case static_cast<std::uint8_t>(CommandCode::Reload):
        return CommandCode::Reload;
    default:
        return std::nullopt;
    }
}

std::string_view CommandName(CommandCode code)
{
    switch (code)
    {
    case CommandCode::Start:
        return "start";
    case CommandCode::Stop:
        return "stop";
    case CommandCode::Restart:
        return "restart";
    case CommandCode::Heartbeat:
        return "heartbeat";
    case CommandCode::Reload:
        return "reload";
    }
    return "unknown";
}

std::string_view CommandResultName(CommandResult result)
{
    switch (result)
    {
    case CommandResult::Ok:
        return "ok";
    case CommandResult::UnknownService:
        return "unknown service";
    case CommandResult::UnknownCommand:
        return "unknown command";
    case CommandResult::Malformed:
        return "malformed request";
    case CommandResult::AlreadyInState:
        return "nothing to do";
    case CommandResult::InvalidState:
        return "not possible now";
    case CommandResult::LaunchFailed:
        return "launch failed";
    case CommandResult::ShuttingDown:
        return "manager is shutting down";
    case CommandResult::ReloadFailed:
        return "reload failed";
    }
    return "unknown result";
}

bool IsSuccess(CommandResult result)
{
    return result == CommandResult::Ok || result == CommandResult::AlreadyInState;
}

} // namespace process_manager
