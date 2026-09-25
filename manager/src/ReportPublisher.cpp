#include "ReportPublisher.hpp"
#include "DetailedReport.hpp"
#include "HealthRecord.hpp"
#include "ZmqSocket.hpp"

#include <span>
#include <string>
#include <vector>

namespace process_manager
{

namespace
{

// Long enough for the final report to leave when the manager shuts down.
constexpr int publisher_linger_ms = 500;

ZmqCode Prepare(ZmqSocket& socket, const std::string& endpoint, std::string& error)
{
    socket.SetOption(SocketOption::Linger, publisher_linger_ms);
    if (endpoint.find('[') != std::string::npos)
    {
        socket.SetOption(SocketOption::Ipv6, 1);
    }
    if (socket.Bind(endpoint) != ZmqCode::Ok)
    {
        error = "cannot bind " + endpoint + ": " + socket.LastError();
        return ZmqCode::Failed;
    }
    return ZmqCode::Ok;
}

} // namespace

ReportPublisher::ReportPublisher(ZmqContext& context)
    : health{context, SocketType::Publisher}, report{context, SocketType::Publisher}
{
}

ZmqCode ReportPublisher::Bind(const std::string& healthEndpoint, const std::string& reportEndpoint,
                              std::string& error)
{
    if (Prepare(health, healthEndpoint, error) != ZmqCode::Ok)
    {
        return ZmqCode::Failed;
    }
    return Prepare(report, reportEndpoint, error);
}

void ReportPublisher::Publish(std::span<const HealthRecord> records, const DetailedReport& detailed)
{
    health.Send(Message{EncodeHealthRecords(records)}, true);
    report.Send(Message{MakeFrame(report_topic), EncodeReport(detailed)}, true);
}

std::string ReportPublisher::HealthEndpoint() const
{
    return health.BoundEndpoint();
}

std::string ReportPublisher::ReportEndpoint() const
{
    return report.BoundEndpoint();
}

} // namespace process_manager
