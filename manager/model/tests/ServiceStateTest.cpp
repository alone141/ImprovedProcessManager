#include <gtest/gtest.h>

#include "ServiceState.hpp"
#include "HealthRecord.hpp"

#include <optional>

TEST(ServiceStateTest, FoldsIntoTheFiveGuiStates)
{
    using process_manager::RuntimeState;
    using process_manager::ServiceState;
    EXPECT_EQ(process_manager::ToRuntimeState(ServiceState::Stopped), RuntimeState::Stopped);
    EXPECT_EQ(process_manager::ToRuntimeState(ServiceState::Waiting), RuntimeState::Starting);
    EXPECT_EQ(process_manager::ToRuntimeState(ServiceState::Starting), RuntimeState::Starting);
    EXPECT_EQ(process_manager::ToRuntimeState(ServiceState::Running), RuntimeState::Running);
    EXPECT_EQ(process_manager::ToRuntimeState(ServiceState::Unhealthy), RuntimeState::Unhealthy);
    EXPECT_EQ(process_manager::ToRuntimeState(ServiceState::Stopping), RuntimeState::Stopped);
    EXPECT_EQ(process_manager::ToRuntimeState(ServiceState::Backoff), RuntimeState::Starting);
    EXPECT_EQ(process_manager::ToRuntimeState(ServiceState::Failed), RuntimeState::Unhealthy);
}

TEST(ServiceStateTest, AliveMeansAProcessExists)
{
    EXPECT_TRUE(process_manager::IsAlive(process_manager::ServiceState::Stopping));
    EXPECT_TRUE(process_manager::IsAlive(process_manager::ServiceState::Unhealthy));
    EXPECT_FALSE(process_manager::IsAlive(process_manager::ServiceState::Backoff));
    EXPECT_FALSE(process_manager::IsAlive(process_manager::ServiceState::Failed));
}

TEST(ServiceStateTest, NamesStates)
{
    EXPECT_EQ(process_manager::ServiceStateName(process_manager::ServiceState::Backoff), "backoff");
}

TEST(ServiceStateTest, RejectsBytesOutsideTheEnum)
{
    EXPECT_EQ(process_manager::ServiceStateFromByte(7), process_manager::ServiceState::Failed);
    EXPECT_EQ(process_manager::ServiceStateFromByte(8), std::nullopt);
}
