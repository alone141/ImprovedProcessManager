#include <gtest/gtest.h>

#include "CommandMessage.hpp"
#include "WireReader.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

TEST(CommandMessageTest, EncodesTheSixtyFiveByteGuiLayout)
{
    process_manager::CommandMessage message{};
    message.command = static_cast<std::uint8_t>(process_manager::CommandCode::Restart);
    message.serviceName = "path_planner";
    message.args = "fast";
    const std::vector<std::uint8_t> bytes = process_manager::EncodeCommand(message);
    ASSERT_EQ(bytes.size(), 65u);
    const process_manager::WireReader reader{bytes, 0, bytes.size()};
    EXPECT_EQ(reader.U8(0), 81);
    EXPECT_EQ(reader.Text(1, 32), "path_planner");
    EXPECT_EQ(reader.Text(33, 32), "fast");
}

TEST(CommandMessageTest, KeepsTheGuiCommandValues)
{
    EXPECT_EQ(static_cast<int>(process_manager::CommandCode::Start), 78);
    EXPECT_EQ(static_cast<int>(process_manager::CommandCode::Stop), 79);
    EXPECT_EQ(static_cast<int>(process_manager::CommandCode::Restart), 81);
}

TEST(CommandMessageTest, RoundTripsACommand)
{
    process_manager::CommandMessage message{};
    message.command = static_cast<std::uint8_t>(process_manager::CommandCode::Stop);
    message.serviceName = "logger_daemon";
    process_manager::CommandMessage decoded{};
    ASSERT_EQ(process_manager::DecodeCommand(process_manager::EncodeCommand(message), decoded),
              process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.command, 79);
    EXPECT_EQ(decoded.serviceName, "logger_daemon");
    EXPECT_EQ(decoded.args, "");
}

TEST(CommandMessageTest, NamesAreCutToThirtyOneBytes)
{
    process_manager::CommandMessage message{};
    message.serviceName = std::string(40, 'x');
    process_manager::CommandMessage decoded{};
    ASSERT_EQ(process_manager::DecodeCommand(process_manager::EncodeCommand(message), decoded),
              process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.serviceName.size(), 31u);
}

TEST(CommandMessageTest, WrongSizeIsRejected)
{
    process_manager::CommandMessage decoded{};
    const std::vector<std::uint8_t> bytes(64, 0);
    EXPECT_EQ(process_manager::DecodeCommand(bytes, decoded), process_manager::DecodeCode::BadLength);
}

TEST(CommandMessageTest, RoundTripsAReply)
{
    process_manager::CommandReply reply{};
    reply.command = 78;
    reply.result = process_manager::CommandResult::AlreadyInState;
    reply.serviceName = "vision";
    reply.message = "already running";
    const std::vector<std::uint8_t> bytes = process_manager::EncodeReply(reply);
    ASSERT_EQ(bytes.size(), 128u);

    process_manager::CommandReply decoded{};
    ASSERT_EQ(process_manager::DecodeReply(bytes, decoded), process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.command, 78);
    EXPECT_EQ(decoded.result, process_manager::CommandResult::AlreadyInState);
    EXPECT_EQ(decoded.serviceName, "vision");
    EXPECT_EQ(decoded.message, "already running");
}

TEST(CommandMessageTest, UnknownCommandBytesAreNotCommands)
{
    EXPECT_EQ(process_manager::CommandCodeFromByte(80), std::nullopt);
    EXPECT_EQ(process_manager::CommandCodeFromByte(90), process_manager::CommandCode::Heartbeat);
}

TEST(CommandMessageTest, AlreadyInStateCountsAsSuccess)
{
    EXPECT_TRUE(process_manager::IsSuccess(process_manager::CommandResult::Ok));
    EXPECT_TRUE(process_manager::IsSuccess(process_manager::CommandResult::AlreadyInState));
    EXPECT_FALSE(process_manager::IsSuccess(process_manager::CommandResult::UnknownService));
}

TEST(CommandMessageTest, NamesCommands)
{
    EXPECT_EQ(process_manager::CommandName(process_manager::CommandCode::Reload), "reload");
    EXPECT_EQ(process_manager::CommandResultName(process_manager::CommandResult::UnknownService), "unknown service");
}
