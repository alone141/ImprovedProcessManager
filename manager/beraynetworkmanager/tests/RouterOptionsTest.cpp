#include <gtest/gtest.h>

#include "RouterOptions.hpp"
#include "Logger.hpp"
#include "MessageRouter.hpp"

#include <initializer_list>
#include <string>
#include <vector>

namespace
{

process_manager::RouterParseCode Parse(std::initializer_list<std::string> arguments,
                                       process_manager::RouterOptions& options, std::string& error)
{
    const std::vector<std::string> list{arguments};
    return process_manager::ParseRouterCommandLine(list, options, error);
}

} // namespace

TEST(RouterOptionsTest, RunsOnTheDefaultEndpointByDefault)
{
    process_manager::RouterOptions options{};
    std::string error{};
    ASSERT_EQ(Parse({}, options, error), process_manager::RouterParseCode::Ok) << error;
    EXPECT_EQ(options.mode, process_manager::RouterMode::Run);
    EXPECT_TRUE(options.endpoint.empty()); // main falls back to default_router_endpoint
    EXPECT_FALSE(options.logLevel.has_value());
    EXPECT_EQ(process_manager::default_router_endpoint, "tcp://*:5558");
}

TEST(RouterOptionsTest, ParsesBindAndLogLevel)
{
    process_manager::RouterOptions options{};
    std::string error{};
    ASSERT_EQ(Parse({"--bind", "tcp://*:7000", "--log-level", "debug"}, options, error),
              process_manager::RouterParseCode::Ok)
        << error;
    EXPECT_EQ(options.endpoint, "tcp://*:7000");
    EXPECT_EQ(options.logLevel, process_manager::LogLevel::Debug);

    ASSERT_EQ(Parse({"--bind=tcp://127.0.0.1:7001", "-v"}, options, error), process_manager::RouterParseCode::Ok);
    EXPECT_EQ(options.endpoint, "tcp://127.0.0.1:7001");
    EXPECT_EQ(options.logLevel, process_manager::LogLevel::Debug);

    ASSERT_EQ(Parse({"-b", "ipc:///tmp/router"}, options, error), process_manager::RouterParseCode::Ok);
    EXPECT_EQ(options.endpoint, "ipc:///tmp/router");
    EXPECT_FALSE(options.logLevel.has_value());
}

TEST(RouterOptionsTest, HelpAndVersionAreModes)
{
    process_manager::RouterOptions options{};
    std::string error{};
    ASSERT_EQ(Parse({"--help"}, options, error), process_manager::RouterParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::RouterMode::Help);
    ASSERT_EQ(Parse({"-V"}, options, error), process_manager::RouterParseCode::Ok);
    EXPECT_EQ(options.mode, process_manager::RouterMode::Version);
}

TEST(RouterOptionsTest, RejectsUnknownOptionsAndBadValues)
{
    process_manager::RouterOptions options{};
    std::string error{};
    EXPECT_EQ(Parse({"--nope"}, options, error), process_manager::RouterParseCode::UnknownOption);
    EXPECT_NE(error.find("--nope"), std::string::npos) << error;
    EXPECT_EQ(Parse({"--bind"}, options, error), process_manager::RouterParseCode::MissingValue);
    EXPECT_EQ(Parse({"--bind="}, options, error), process_manager::RouterParseCode::InvalidValue);
    EXPECT_EQ(Parse({"--log-level", "loud"}, options, error), process_manager::RouterParseCode::InvalidValue);
}

TEST(RouterOptionsTest, UsageNamesTheDefaultEndpoint)
{
    const std::string text = process_manager::RouterUsageText("beraynetworkmanager");
    EXPECT_NE(text.find("tcp://*:5558"), std::string::npos);
    EXPECT_NE(text.find("--bind"), std::string::npos);
    EXPECT_EQ(text.back(), '\n');
}
