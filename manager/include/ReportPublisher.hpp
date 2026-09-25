#pragma once

#include "DetailedReport.hpp"
#include "HealthRecord.hpp"
#include "ZmqSocket.hpp"

#include <span>
#include <string>

namespace process_manager
{

// Two PUB sockets. The health socket sends exactly what the GUI has always read
// (one frame holding the 128-byte records), so existing subscribers keep working;
// the detailed report goes out on its own socket with a "report" topic frame.
class ReportPublisher
{
public:
    /**
     * @brief Create the two publisher sockets.
     * @param context Context to create them in. It must outlive the publisher.
     */
    explicit ReportPublisher(ZmqContext& context);

    /**
     * @brief Bind both sockets.
     * @param healthEndpoint Endpoint for the simplified health report, for example tcp://0.0.0.0:6667.
     * @param reportEndpoint Endpoint for the detailed report, for example tcp://0.0.0.0:6668.
     * @param error Receives the reason on failure.
     * @return ZmqCode::Ok when both are bound.
     */
    ZmqCode Bind(const std::string& healthEndpoint, const std::string& reportEndpoint, std::string& error);

    /**
     * @brief Publish one snapshot on both sockets. Slow subscribers lose messages; the
     *        manager never waits for them.
     * @param health Simplified records.
     * @param report Detailed report.
     */
    void Publish(std::span<const HealthRecord> health, const DetailedReport& report);

    /**
     * @brief Where the health socket is bound.
     * @return The endpoint with its actual port.
     */
    std::string HealthEndpoint() const;

    /**
     * @brief Where the report socket is bound.
     * @return The endpoint with its actual port.
     */
    std::string ReportEndpoint() const;

private:
    ZmqSocket health;
    ZmqSocket report;
};

} // namespace process_manager
