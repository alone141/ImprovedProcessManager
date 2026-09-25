#include <gtest/gtest.h>

#include "ConfigParser.hpp"
#include "ServiceConfig.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace
{

process_manager::ConfigCode Parse(const std::string& text, process_manager::Config& config,
                                  process_manager::ConfigError& error)
{
    const process_manager::ConfigParser parser{};
    return parser.ParseText(text, config, error);
}

// An absolute path on the platform running the test.
std::string LogPath()
{
    return (std::filesystem::temp_directory_path() / "client.log").string();
}

const char* const full_example = R"(# Example
[manager]
health_endpoint = tcp://*:7001
report_endpoint = tcp://*:7002   # inline comment
command_endpoint = tcp://127.0.0.1:7003
publish_interval_ms = 500
log_level = debug
cgroups = off
gpu = off

[defaults]
restart = always
stop_timeout_ms = 2000
env = SHARED=yes

[service broker]
binary = /opt/app/bin/broker
args = --port 9000 --name "message broker" 'single quoted' C:\data\file.txt
working_dir = /opt/app
env = BROKER_MODE=fast
env = QUOTED="a b"
autostart = false
restart_delay_ms = 250
restart_delay_max_ms = 4000
max_restarts = 0
restart_window_s = 30
start_grace_ms = 0
stop_signal = SIGINT
heartbeat_interval_ms = 1000
heartbeat_tolerance = 2
unhealthy_action = restart
output = null
memory_max = 512M
cpu_max = 150%
description = "The message broker"

[service client]
binary = client
depends_on = broker
output = @LOG@
)";

std::string FullExample()
{
    std::string text{full_example};
    text.replace(text.find("@LOG@"), 5, LogPath());
    return text;
}

} // namespace

TEST(ConfigParserTest, ParsesEveryKey)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    ASSERT_EQ(Parse(FullExample(), config, error), process_manager::ConfigCode::Ok) << error.message;

    EXPECT_EQ(config.manager.healthEndpoint, "tcp://*:7001");
    EXPECT_EQ(config.manager.reportEndpoint, "tcp://*:7002");
    EXPECT_EQ(config.manager.commandEndpoint, "tcp://127.0.0.1:7003");
    EXPECT_EQ(config.manager.publishInterval, std::chrono::milliseconds{500});
    EXPECT_EQ(config.manager.logLevel, process_manager::LogLevel::Debug);
    EXPECT_EQ(config.manager.cgroups, process_manager::CgroupMode::Off);
    EXPECT_EQ(config.manager.gpu, process_manager::GpuMode::Off);
    ASSERT_EQ(config.services.size(), 2u);

    const process_manager::ServiceConfig& broker = config.services[0];
    EXPECT_EQ(broker.name, "broker");
    EXPECT_EQ(broker.description, "The message broker");
    EXPECT_EQ(broker.binary, "/opt/app/bin/broker");
    const std::vector<std::string> arguments{"--port", "9000", "--name", "message broker", "single quoted",
                                             "C:\\data\\file.txt"};
    EXPECT_EQ(broker.arguments, arguments);
    EXPECT_EQ(broker.workingDirectory, "/opt/app");
    ASSERT_EQ(broker.environment.size(), 3u);
    EXPECT_EQ(broker.environment[0].name, "SHARED");
    EXPECT_EQ(broker.environment[1].value, "fast");
    EXPECT_EQ(broker.environment[2].value, "a b");
    EXPECT_FALSE(broker.autostart);
    EXPECT_EQ(broker.restart, process_manager::RestartMode::Always);
    EXPECT_EQ(broker.restartDelay, std::chrono::milliseconds{250});
    EXPECT_EQ(broker.restartDelayMax, std::chrono::milliseconds{4000});
    EXPECT_EQ(broker.maxRestarts, 0);
    EXPECT_EQ(broker.restartWindow, std::chrono::seconds{30});
    EXPECT_EQ(broker.startGrace, std::chrono::milliseconds{0});
    EXPECT_EQ(broker.stopSignal, process_manager::StopSignal::Interrupt);
    EXPECT_EQ(broker.stopTimeout, std::chrono::milliseconds{2000});
    EXPECT_EQ(broker.heartbeatInterval, std::chrono::milliseconds{1000});
    EXPECT_EQ(broker.heartbeatTolerance, 2);
    EXPECT_EQ(broker.unhealthyAction, process_manager::UnhealthyAction::Restart);
    EXPECT_EQ(broker.output, process_manager::OutputMode::Null);
    EXPECT_EQ(broker.memoryMax, 512ull * 1024 * 1024);
    EXPECT_EQ(broker.cpuMaxPercent, 150u);

    const process_manager::ServiceConfig& client = config.services[1];
    EXPECT_EQ(client.binary, "client");
    EXPECT_TRUE(client.autostart);
    EXPECT_EQ(client.restart, process_manager::RestartMode::Always);
    EXPECT_EQ(client.dependsOn, std::vector<std::string>{"broker"});
    EXPECT_EQ(client.output, process_manager::OutputMode::File);
    EXPECT_EQ(client.outputPath, LogPath());
    ASSERT_EQ(client.environment.size(), 1u);
}

TEST(ConfigParserTest, DefaultsApplyWithoutAManagerSection)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    ASSERT_EQ(Parse("[service a]\nbinary = /bin/true\n", config, error), process_manager::ConfigCode::Ok);
    EXPECT_EQ(config.manager.healthEndpoint, "tcp://*:6667");
    EXPECT_EQ(config.manager.commandEndpoint, "tcp://*:5557");
    EXPECT_EQ(config.services[0].restart, process_manager::RestartMode::OnFailure);
    EXPECT_EQ(config.services[0].maxRestarts, 5);
}

TEST(ConfigParserTest, ReportsTheLineOfAnUnknownKey)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[service a]\nbinary = /bin/true\nrestat = always\n", config, error),
              process_manager::ConfigCode::UnknownKey);
    EXPECT_EQ(error.line, 3);
    EXPECT_NE(error.message.find("restat"), std::string::npos);
}

TEST(ConfigParserTest, RejectsDuplicateKeysButCollectsEnv)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[service a]\nbinary = x\nbinary = y\n", config, error), process_manager::ConfigCode::DuplicateKey);
    EXPECT_EQ(error.line, 3);
    EXPECT_EQ(Parse("[service a]\nbinary = x\nenv = A=1\nenv = B=2\n", config, error), process_manager::ConfigCode::Ok);
}

TEST(ConfigParserTest, RejectsServicesWithoutBinary)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[service lonely]\nautostart = true\n", config, error), process_manager::ConfigCode::MissingValue);
    EXPECT_EQ(error.line, 1);
}

TEST(ConfigParserTest, RejectsBadNamesAndDuplicates)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[service bad name]\nbinary = x\n", config, error), process_manager::ConfigCode::InvalidName);
    EXPECT_EQ(Parse("[service a]\nbinary = x\n[service a]\nbinary = y\n", config, error),
              process_manager::ConfigCode::DuplicateService);
    EXPECT_EQ(error.line, 3);
}

TEST(ConfigParserTest, RejectsUnknownSections)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[services]\n", config, error), process_manager::ConfigCode::UnknownSection);
    EXPECT_EQ(Parse("[manager\n", config, error), process_manager::ConfigCode::Syntax);
    EXPECT_EQ(Parse("binary = x\n", config, error), process_manager::ConfigCode::Syntax);
}

TEST(ConfigParserTest, DefaultsMustComeFirst)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[service a]\nbinary = x\n[defaults]\nrestart = always\n", config, error),
              process_manager::ConfigCode::Syntax);
    EXPECT_EQ(Parse("[defaults]\nbinary = x\n", config, error), process_manager::ConfigCode::UnknownKey);
}

TEST(ConfigParserTest, RejectsInvalidValues)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[service a]\nbinary = x\nrestart = maybe\n", config, error),
              process_manager::ConfigCode::InvalidValue);
    EXPECT_EQ(Parse("[service a]\nbinary = x\nstop_timeout_ms = -1\n", config, error),
              process_manager::ConfigCode::InvalidValue);
    EXPECT_EQ(Parse("[service a]\nbinary = x\noutput = relative.log\n", config, error),
              process_manager::ConfigCode::InvalidValue);
    EXPECT_EQ(Parse("[service a]\nbinary = x\nargs = \"open\n", config, error),
              process_manager::ConfigCode::InvalidValue);
    EXPECT_EQ(Parse("[manager]\npublish_interval_ms = 10\n", config, error), process_manager::ConfigCode::InvalidValue);
    EXPECT_EQ(Parse("[service a]\nbinary = x\nrestart_delay_ms = 5000\nrestart_delay_max_ms = 1000\n", config, error),
              process_manager::ConfigCode::InvalidValue);
}

TEST(ConfigParserTest, RejectsUnknownDependencies)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[service a]\nbinary = x\ndepends_on = ghost\n", config, error),
              process_manager::ConfigCode::UnknownDependency);
    EXPECT_EQ(error.line, 3);
}

TEST(ConfigParserTest, RejectsDependencyCycles)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    const std::string text = "[service a]\nbinary = x\ndepends_on = b\n"
                             "[service b]\nbinary = x\ndepends_on = c\n"
                             "[service c]\nbinary = x\ndepends_on = a\n";
    EXPECT_EQ(Parse(text, config, error), process_manager::ConfigCode::DependencyCycle);
    EXPECT_NE(error.message.find("a -> b -> c -> a"), std::string::npos) << error.message;
    EXPECT_EQ(Parse("[service a]\nbinary = x\ndepends_on = a\n", config, error),
              process_manager::ConfigCode::DependencyCycle);
}

TEST(ConfigParserTest, RejectsSharedEndpoints)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[manager]\nhealth_endpoint = tcp://*:1\nreport_endpoint = tcp://*:1\n", config, error),
              process_manager::ConfigCode::InvalidValue);
}

TEST(ConfigParserTest, IgnoresCarriageReturnsAndByteOrderMark)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    ASSERT_EQ(Parse("\xEF\xBB\xBF[service a]\r\nbinary = /bin/true\r\n", config, error),
              process_manager::ConfigCode::Ok);
    EXPECT_EQ(config.services[0].binary, "/bin/true");
}

TEST(ConfigParserTest, ReadsAFileAndKeepsItsPath)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "process_manager_config_test.conf";
    {
        std::ofstream file{path};
        file << "[service a]\nbinary = /bin/true\n";
    }
    process_manager::Config config{};
    process_manager::ConfigError error{};
    const process_manager::ConfigParser parser{};
    ASSERT_EQ(parser.ParseFile(path.string(), config, error), process_manager::ConfigCode::Ok);
    EXPECT_EQ(config.path, path.string());
    std::filesystem::remove(path);
    EXPECT_EQ(parser.ParseFile(path.string(), config, error), process_manager::ConfigCode::Unreadable);
}

TEST(ConfigParserTest, SplitsArgumentsLikeAShell)
{
    std::vector<std::string> out{};
    const std::string text = R"(a "b c" 'd "e"' f\g "h\"i" "")";
    ASSERT_EQ(process_manager::SplitArguments(text, out), process_manager::SplitCode::Ok);
    const std::vector<std::string> expected{"a", "b c", "d \"e\"", "f\\g", "h\"i", ""};
    EXPECT_EQ(out, expected);
    EXPECT_EQ(process_manager::SplitArguments("'open", out), process_manager::SplitCode::UnterminatedQuote);
    ASSERT_EQ(process_manager::SplitArguments("   ", out), process_manager::SplitCode::Ok);
    EXPECT_TRUE(out.empty());
}

TEST(ConfigParserTest, ParsesByteSizes)
{
    EXPECT_EQ(process_manager::ParseByteSize("1048576"), 1048576u);
    EXPECT_EQ(process_manager::ParseByteSize("64K"), 65536u);
    EXPECT_EQ(process_manager::ParseByteSize("512 MiB"), 512ull << 20);
    EXPECT_EQ(process_manager::ParseByteSize("2g"), 2ull << 30);
    EXPECT_EQ(process_manager::ParseByteSize("1TB"), 1ull << 40);
    EXPECT_EQ(process_manager::ParseByteSize("12 parsecs"), std::nullopt);
    EXPECT_EQ(process_manager::ParseByteSize("M"), std::nullopt);
    EXPECT_EQ(process_manager::ParseByteSize("99999999999999T"), std::nullopt);
}

TEST(ConfigParserTest, RejectsARelativeWorkingDirectory)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    EXPECT_EQ(Parse("[service a]\nbinary = /bin/true\nworking_dir = srv/a\n", config, error),
              process_manager::ConfigCode::InvalidValue);
    EXPECT_EQ(error.line, 3);
    EXPECT_NE(error.message.find("absolute"), std::string::npos) << error.message;
}

TEST(ConfigParserTest, CommentsMayFollowSectionHeaders)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    ASSERT_EQ(Parse("[service vision]   # camera\nbinary = /bin/true\n", config, error),
              process_manager::ConfigCode::Ok)
        << error.message;
    EXPECT_EQ(config.services[0].name, "vision");
}

TEST(ConfigParserTest, ApostrophesDoNotHideComments)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    ASSERT_EQ(Parse("[service a]\nbinary = /bin/true\ndescription = Bob's camera  # front\n", config, error),
              process_manager::ConfigCode::Ok);
    EXPECT_EQ(config.services[0].description, "Bob's camera");
}

TEST(ConfigParserTest, QuotesKeepHashesThatBelongToTheValue)
{
    process_manager::Config config{};
    process_manager::ConfigError error{};
    const std::string text = "[service a]\nbinary = /bin/true\ndescription = \"channel # 5\"  # note\n"
                             "args = --title \"a # b\" --x  # comment\nenv = TAG='x # y'  # c\n";
    ASSERT_EQ(Parse(text, config, error), process_manager::ConfigCode::Ok) << error.message;
    EXPECT_EQ(config.services[0].description, "channel # 5");
    EXPECT_EQ(config.services[0].arguments, (std::vector<std::string>{"--title", "a # b", "--x"}));
    ASSERT_EQ(config.services[0].environment.size(), 1u);
    EXPECT_EQ(config.services[0].environment[0].value, "x # y");
}
