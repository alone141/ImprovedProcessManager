#include <gtest/gtest.h>

#include "ServiceConfig.hpp"

#include <optional>
#include <string>

TEST(ServiceConfigTest, AcceptsNamesThatFitTheCommandField)
{
    EXPECT_TRUE(process_manager::IsValidServiceName("sensor_fusion"));
    EXPECT_TRUE(process_manager::IsValidServiceName("a.b-c_1"));
    EXPECT_TRUE(process_manager::IsValidServiceName(std::string(31, 'x')));
}

TEST(ServiceConfigTest, RejectsNamesTheWireOrCgroupsCannotCarry)
{
    EXPECT_FALSE(process_manager::IsValidServiceName(""));
    EXPECT_FALSE(process_manager::IsValidServiceName(std::string(32, 'x')));
    EXPECT_FALSE(process_manager::IsValidServiceName("with space"));
    EXPECT_FALSE(process_manager::IsValidServiceName("slash/name"));
    EXPECT_FALSE(process_manager::IsValidServiceName("*"));
    EXPECT_FALSE(process_manager::IsValidServiceName(".."));
}

TEST(ServiceConfigTest, ParsesRestartModes)
{
    EXPECT_EQ(process_manager::ParseRestartMode("no"), process_manager::RestartMode::Never);
    EXPECT_EQ(process_manager::ParseRestartMode("on-failure"), process_manager::RestartMode::OnFailure);
    EXPECT_EQ(process_manager::ParseRestartMode("always"), process_manager::RestartMode::Always);
    EXPECT_EQ(process_manager::ParseRestartMode("sometimes"), std::nullopt);
    EXPECT_EQ(process_manager::RestartModeName(process_manager::RestartMode::OnFailure), "on-failure");
}

TEST(ServiceConfigTest, ParsesStopSignalsWithOrWithoutPrefix)
{
    EXPECT_EQ(process_manager::ParseStopSignal("TERM"), process_manager::StopSignal::Terminate);
    EXPECT_EQ(process_manager::ParseStopSignal("SIGINT"), process_manager::StopSignal::Interrupt);
    EXPECT_EQ(process_manager::ParseStopSignal("USR2"), process_manager::StopSignal::User2);
    EXPECT_EQ(process_manager::ParseStopSignal("SIGSEGV"), std::nullopt);
}

TEST(ServiceConfigTest, ComparesEveryField)
{
    process_manager::ServiceConfig a{};
    a.name = "x";
    process_manager::ServiceConfig b = a;
    EXPECT_TRUE(process_manager::SameServiceConfig(a, b));
    b.environment.push_back(process_manager::EnvironmentVariable{"A", "1"});
    EXPECT_FALSE(process_manager::SameServiceConfig(a, b));
    b = a;
    b.cpuMaxPercent = 50;
    EXPECT_FALSE(process_manager::SameServiceConfig(a, b));
}

TEST(ServiceConfigTest, FindsServicesByName)
{
    process_manager::Config config{};
    config.services.resize(2);
    config.services[0].name = "one";
    config.services[1].name = "two";
    const process_manager::ServiceConfig* found = process_manager::FindService(config, "two");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "two");
    EXPECT_EQ(process_manager::FindService(config, "three"), nullptr);
}
