#include <gtest/gtest.h>

#include "CommandLine.hpp"

#include <chrono>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{

process_manager::ParseCode Parse(std::initializer_list<std::string> arguments, process_manager::Options& options)
{
    std::string error{};
    return process_manager::ParseCommandLine(arguments, options, error);
}

} // namespace

TEST(CommandLineTest, RunsTheManagerByDefault)
{
    process_manager::Options options{};
    ASSERT_EQ(Parse({}, options), process_manager::ParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::Mode::Run);
    EXPECT_FALSE(options.configGiven);
    EXPECT_EQ(options.configPath, process_manager::DefaultConfigPath());
}

TEST(CommandLineTest, ParsesServiceCommands)
{
    process_manager::Options options{};
    ASSERT_EQ(Parse({"-r", "sensor_fusion"}, options), process_manager::ParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::Mode::Restart);
    EXPECT_EQ(options.serviceName, "sensor_fusion");

    ASSERT_EQ(Parse({"--stop=logger"}, options), process_manager::ParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::Mode::Stop);
    EXPECT_EQ(options.serviceName, "logger");

    ASSERT_EQ(Parse({"-s", "*"}, options), process_manager::ParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::Mode::Start);
    EXPECT_EQ(options.serviceName, "*");

    ASSERT_EQ(Parse({"-k", "x"}, options), process_manager::ParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::Mode::Stop);
}

TEST(CommandLineTest, ParsesRouterOptions)
{
    process_manager::Options options{};
    ASSERT_EQ(Parse({"--stop", "x", "--router", "tcp://rig:5558", "--manager", "bpm-rig01"}, options),
              process_manager::ParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::Mode::Stop);
    EXPECT_EQ(options.routerEndpoint, "tcp://rig:5558");
    EXPECT_EQ(options.managerIdentity, "bpm-rig01");

    ASSERT_EQ(Parse({"--reload", "--router=tcp://127.0.0.1:5558"}, options), process_manager::ParseCode::Ok);
    EXPECT_EQ(options.routerEndpoint, "tcp://127.0.0.1:5558");
    EXPECT_TRUE(options.managerIdentity.empty()); // from the configuration, else the default

    EXPECT_EQ(Parse({"--router"}, options), process_manager::ParseCode::MissingValue);
    EXPECT_EQ(Parse({"--router="}, options), process_manager::ParseCode::InvalidValue);
    EXPECT_EQ(Parse({"--manager", "has space"}, options), process_manager::ParseCode::InvalidValue);
    EXPECT_EQ(Parse({"--manager", ""}, options), process_manager::ParseCode::InvalidValue);
    EXPECT_NE(process_manager::UsageText("x").find("--router"), std::string::npos);
}

TEST(CommandLineTest, ParsesStatusOptions)
{
    process_manager::Options options{};
    ASSERT_EQ(Parse({"--status", "--wide", "--timeout", "500", "-c", "/tmp/x.conf"}, options),
              process_manager::ParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::Mode::Status);
    EXPECT_TRUE(options.wide);
    EXPECT_FALSE(options.watch);
    EXPECT_EQ(options.timeout, std::chrono::milliseconds{500});
    EXPECT_TRUE(options.configGiven);
    EXPECT_EQ(options.configPath, "/tmp/x.conf");

    ASSERT_EQ(Parse({"-w"}, options), process_manager::ParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::Mode::Status);
    EXPECT_TRUE(options.watch);
}

TEST(CommandLineTest, ParsesEndpointsAndLogLevel)
{
    process_manager::Options options{};
    ASSERT_EQ(Parse({"-l", "--command", "tcp://h:1", "--report=tcp://h:2", "--log-level", "debug"}, options),
              process_manager::ParseCode::Ok);
    EXPECT_EQ(options.commandEndpoint, "tcp://h:1");
    EXPECT_EQ(options.reportEndpoint, "tcp://h:2");
    EXPECT_EQ(options.logLevel, process_manager::LogLevel::Debug);
}

TEST(CommandLineTest, RejectsMistakes)
{
    process_manager::Options options{};
    EXPECT_EQ(Parse({"--bogus"}, options), process_manager::ParseCode::UnknownOption);
    EXPECT_EQ(Parse({"-r"}, options), process_manager::ParseCode::MissingValue);
    EXPECT_EQ(Parse({"-s", "a", "-k", "b"}, options), process_manager::ParseCode::ConflictingModes);
    EXPECT_EQ(Parse({"--timeout", "soon"}, options), process_manager::ParseCode::InvalidValue);
    EXPECT_EQ(Parse({"-s", "bad name"}, options), process_manager::ParseCode::InvalidValue);
    EXPECT_EQ(Parse({"--heartbeat", "*"}, options), process_manager::ParseCode::InvalidValue);
    EXPECT_EQ(Parse({"--log-level", "loud"}, options), process_manager::ParseCode::InvalidValue);
}

TEST(CommandLineTest, TurnsBindEndpointsIntoLocalOnes)
{
    EXPECT_EQ(process_manager::ConnectEndpoint("tcp://*:5557"), "tcp://127.0.0.1:5557");
    EXPECT_EQ(process_manager::ConnectEndpoint("tcp://0.0.0.0:6668"), "tcp://127.0.0.1:6668");
    EXPECT_EQ(process_manager::ConnectEndpoint("tcp://[::]:6668"), "tcp://[::1]:6668");
    EXPECT_EQ(process_manager::ConnectEndpoint("tcp://10.0.0.5:1"), "tcp://10.0.0.5:1");
    EXPECT_EQ(process_manager::ConnectEndpoint("ipc:///run/pm.sock"), "ipc:///run/pm.sock");
}

TEST(CommandLineTest, UsageMentionsEveryMode)
{
    const std::string usage = process_manager::UsageText("pm");
    for (const char* flag : {"--status", "--start", "--stop", "--restart", "--reload", "--heartbeat", "--check"})
    {
        EXPECT_NE(usage.find(flag), std::string::npos) << flag;
    }
}

TEST(CommandLineTest, RemembersWhetherATimeoutWasGiven)
{
    process_manager::Options options{};
    ASSERT_EQ(Parse({"--status"}, options), process_manager::ParseCode::Ok);
    EXPECT_FALSE(options.timeoutGiven);
    ASSERT_EQ(Parse({"--status", "--timeout", "250"}, options), process_manager::ParseCode::Ok);
    EXPECT_TRUE(options.timeoutGiven);
}
