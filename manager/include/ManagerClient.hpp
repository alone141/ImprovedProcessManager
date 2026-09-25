#pragma once

#include "CommandMessage.hpp"
#include "DetailedReport.hpp"
#include "ZmqSocket.hpp"

#include <chrono>
#include <memory>
#include <string>

namespace process_manager
{

enum class ClientCode
{
    Ok,
    Timeout,
    Failed,
    Malformed,
};

// The CLI side of the protocol. Commands use a DEALER without a fixed identity,
// so the CLI never collides with a GUI connected as "PMC".
class ManagerClient
{
public:
    /**
     * @brief Prepare a client; nothing connects until a request is made.
     * @param commandEndpoint The manager's ROUTER, for example tcp://127.0.0.1:5557.
     * @param reportEndpoint The manager's detailed report publisher, for example tcp://127.0.0.1:6668.
     */
    ManagerClient(std::string commandEndpoint, std::string reportEndpoint);

    /**
     * @brief Send one command and wait for its reply.
     * @param command Command to send.
     * @param service Service name, or "*" for every service.
     * @param timeout Longest wait for the reply.
     * @param out Receives the reply on success.
     * @param error Receives the reason on failure.
     * @return ClientCode::Ok with a reply; ClientCode::Timeout when the manager did not answer.
     */
    ClientCode SendCommand(CommandCode command, const std::string& service, std::chrono::milliseconds timeout,
                           CommandReply& out, std::string& error);

    /**
     * @brief Wait for the next detailed report. The subscription stays open between calls.
     * @param timeout Longest wait.
     * @param out Receives the report on success.
     * @param error Receives the reason on failure.
     * @return ClientCode::Ok with a report; ClientCode::Timeout when none arrived.
     */
    ClientCode WaitForReport(std::chrono::milliseconds timeout, DetailedReport& out, std::string& error);

private:
    ZmqContext context;
    std::string commandEndpoint;
    std::string reportEndpoint;
    std::unique_ptr<ZmqSocket> subscriber;
};

} // namespace process_manager
