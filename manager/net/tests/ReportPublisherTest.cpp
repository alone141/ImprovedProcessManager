#include <gtest/gtest.h>

#include "ReportPublisher.hpp"
#include "DetailedReport.hpp"
#include "HealthRecord.hpp"
#include "ZmqSocket.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace
{

using Records = std::vector<process_manager::HealthRecord>;
using Sockets = std::vector<process_manager::ZmqSocket*>;

} // namespace

TEST(ReportPublisherTest, HealthIsOneFrameAndTheReportHasATopic)
{
    process_manager::ZmqContext context{};
    process_manager::ReportPublisher publisher{context};
    std::string error{};
    ASSERT_EQ(publisher.Bind("tcp://127.0.0.1:*", "tcp://127.0.0.1:*", error), process_manager::ZmqCode::Ok) << error;

    process_manager::ZmqSocket health{context, process_manager::SocketType::Subscriber};
    health.SetOption(process_manager::SocketOption::Subscribe, "");
    health.Connect(publisher.HealthEndpoint());
    process_manager::ZmqSocket report{context, process_manager::SocketType::Subscriber};
    report.SetOption(process_manager::SocketOption::Subscribe, "report");
    report.Connect(publisher.ReportEndpoint());

    process_manager::HealthRecord record{};
    record.processName = "svc";
    process_manager::DetailedReport detailed{};
    detailed.hostName = "rig";
    process_manager::Message healthMessage{};
    process_manager::Message reportMessage{};
    // Only the sockets still waiting for their first message are polled. Once the
    // health frame is in, the wait belongs to the report subscriber, which may still
    // be joining (a PUB drops messages until the subscription has arrived); polling
    // the readable health socket as well would return at once and never wait.
    for (int attempt = 0; attempt < 200 && (healthMessage.empty() || reportMessage.empty()); ++attempt)
    {
        publisher.Publish(Records{record, record}, detailed);
        Sockets pending{};
        if (healthMessage.empty())
        {
            pending.push_back(&health);
        }
        if (reportMessage.empty())
        {
            pending.push_back(&report);
        }
        std::vector<bool> ready{};
        process_manager::PollReadable(pending, std::chrono::milliseconds{20}, ready);
        for (std::size_t i = 0; i < pending.size(); ++i)
        {
            if (ready[i])
            {
                pending[i]->Receive(pending[i] == &health ? healthMessage : reportMessage, true);
            }
        }
    }

    ASSERT_EQ(healthMessage.size(), 1u);
    EXPECT_EQ(healthMessage[0].size(), 2 * process_manager::health_record_size);
    ASSERT_EQ(reportMessage.size(), 2u);
    EXPECT_TRUE(process_manager::FrameIs(reportMessage[0], "report"));
    process_manager::DetailedReport decoded{};
    ASSERT_EQ(process_manager::DecodeReport(reportMessage[1], decoded), process_manager::DecodeCode::Ok);
    EXPECT_EQ(decoded.hostName, "rig");
}

TEST(ReportPublisherTest, ReportsBindFailures)
{
    process_manager::ZmqContext context{};
    process_manager::ReportPublisher publisher{context};
    std::string error{};
    EXPECT_EQ(publisher.Bind("nonsense", "tcp://127.0.0.1:*", error), process_manager::ZmqCode::Failed);
    EXPECT_NE(error.find("nonsense"), std::string::npos);
}
