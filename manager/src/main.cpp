#include "CommandLine.hpp"
#include "CommandMessage.hpp"
#include "ConfigParser.hpp"
#include "Console.hpp"
#include "Daemon.hpp"
#include "DetailedReport.hpp"
#include "Logger.hpp"
#include "ManagerClient.hpp"
#include "ProcFs.hpp"
#include "ServiceConfig.hpp"
#include "SignalWatcher.hpp"
#include "StatusTable.hpp"
#include "Version.hpp"
#include "ZmqSocket.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace process_manager
{

namespace
{

constexpr int exit_ok = 0;
constexpr int exit_refused = 1;
constexpr int exit_usage = 2;
constexpr int exit_no_answer = 3;
constexpr std::chrono::milliseconds report_wait_margin{2000};

void Print(std::FILE* stream, const std::string& text)
{
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fflush(stream);
}

std::string DescribeConfigError(const std::string& path, const ConfigError& error)
{
    return path + (error.line > 0 ? ":" + std::to_string(error.line) : std::string{}) + ": " + error.message;
}

// Client modes read the endpoints from the configuration when it exists, so a
// manager on custom ports is found without repeating them on the command line.
int ResolveEndpoints(const Options& options, std::string& command, std::string& report,
                     std::chrono::milliseconds& interval)
{
    command = "tcp://127.0.0.1:5557";
    report = "tcp://127.0.0.1:6668";
    interval = std::chrono::milliseconds{1000};
    std::error_code status{};
    if (options.configGiven || std::filesystem::exists(Utf8Path(options.configPath), status))
    {
        Config config{};
        ConfigError error{};
        const ConfigParser parser{};
        if (parser.ParseFile(options.configPath, config, error) == ConfigCode::Ok)
        {
            command = ConnectEndpoint(config.manager.commandEndpoint);
            report = ConnectEndpoint(config.manager.reportEndpoint);
            interval = config.manager.publishInterval;
        }
        else if (options.configGiven)
        {
            Print(stderr, DescribeConfigError(options.configPath, error) + "\n");
            return exit_usage;
        }
    }
    if (!options.commandEndpoint.empty())
    {
        command = options.commandEndpoint;
    }
    if (!options.reportEndpoint.empty())
    {
        report = options.reportEndpoint;
    }
    return exit_ok;
}

int RunCheck(const Options& options)
{
    Config config{};
    ConfigError error{};
    const ConfigParser parser{};
    if (parser.ParseFile(options.configPath, config, error) != ConfigCode::Ok)
    {
        Print(stderr, DescribeConfigError(options.configPath, error) + "\n");
        return exit_usage;
    }

    std::string text = options.configPath + ": " + std::to_string(config.services.size()) + " services\n";
    text += "  health " + config.manager.healthEndpoint + ", report " + config.manager.reportEndpoint +
            ", commands " + config.manager.commandEndpoint + "\n";
    for (const ServiceConfig& service : config.services)
    {
        text += "  " + service.name + ": " + service.binary + " (restart " + std::string{RestartModeName(service.restart)} +
                (service.autostart ? ", autostart" : "") +
                (service.dependsOn.empty() ? std::string{} : ", after " + service.dependsOn.front() +
                                                                 (service.dependsOn.size() > 1 ? " and others" : "")) +
                ")\n";
    }
    Print(stdout, text);
    return exit_ok;
}

int RunManager(const Options& options)
{
    Config config{};
    ConfigError error{};
    const ConfigParser parser{};
    if (parser.ParseFile(options.configPath, config, error) != ConfigCode::Ok)
    {
        LogError(DescribeConfigError(options.configPath, error));
        return exit_usage;
    }
    GlobalLogger().SetLevel(options.logLevel.value_or(config.manager.logLevel));

    SignalWatcher::Install();
    Daemon daemon{config};
    if (options.logLevel.has_value())
    {
        daemon.OverrideLogLevel(*options.logLevel);
    }
    std::string problem{};
    if (daemon.Start(problem) != DaemonCode::Ok)
    {
        LogError(problem);
        return exit_refused;
    }
    return daemon.Run();
}

int RunStatus(const Options& options)
{
    std::string command{};
    std::string report{};
    std::chrono::milliseconds interval{};
    const int resolved = ResolveEndpoints(options, command, report, interval);
    if (resolved != exit_ok)
    {
        return resolved;
    }
    // A fresh subscription sees the next report, which can be a whole interval away.
    const std::chrono::milliseconds timeout =
        options.timeoutGiven ? options.timeout : std::max(options.timeout, interval + report_wait_margin);

    ManagerClient client{command, report};
    const StatusTable table{TableOptions{options.wide}};
    DetailedReport snapshot{};
    std::string error{};
    if (!options.watch)
    {
        const ClientCode code = client.WaitForReport(timeout, snapshot, error);
        if (code != ClientCode::Ok)
        {
            Print(stderr, error + "\n");
            return code == ClientCode::Timeout ? exit_no_answer : exit_refused;
        }
        Print(stdout, table.Render(snapshot));
        return exit_ok;
    }

    const bool escapes = Console::EnableEscapes();
    SignalWatcher::Install();
    while (!SignalWatcher::TakeStopRequest())
    {
        const ClientCode code = client.WaitForReport(timeout, snapshot, error);
        if (code == ClientCode::Timeout)
        {
            Print(stderr, error + "\n");
            continue;
        }
        if (code != ClientCode::Ok)
        {
            Print(stderr, error + "\n");
            return exit_refused;
        }
        Print(stdout, (escapes ? std::string{clear_screen} : std::string{"\n"}) + table.Render(snapshot));
    }
    return exit_ok;
}

int RunCommand(const Options& options)
{
    std::string command{};
    std::string report{};
    std::chrono::milliseconds interval{};
    const int resolved = ResolveEndpoints(options, command, report, interval);
    if (resolved != exit_ok)
    {
        return resolved;
    }

    CommandCode code = CommandCode::Reload;
    switch (options.mode)
    {
    case Mode::Start:
        code = CommandCode::Start;
        break;
    case Mode::Stop:
        code = CommandCode::Stop;
        break;
    case Mode::Restart:
        code = CommandCode::Restart;
        break;
    case Mode::Heartbeat:
        code = CommandCode::Heartbeat;
        break;
    default:
        code = CommandCode::Reload;
        break;
    }

    ManagerClient client{command, report};
    CommandReply reply{};
    std::string error{};
    const ClientCode sent = client.SendCommand(code, options.serviceName, options.timeout, reply, error);
    if (sent != ClientCode::Ok)
    {
        Print(stderr, error + "\n");
        return sent == ClientCode::Timeout ? exit_no_answer : exit_refused;
    }

    const std::string subject =
        std::string{CommandName(code)} + (options.serviceName.empty() ? std::string{} : " " + options.serviceName);
    const std::string detail = reply.message.empty() ? std::string{} : " (" + reply.message + ")";
    const bool success = IsSuccess(reply.result);
    Print(success ? stdout : stderr, subject + ": " + std::string{CommandResultName(reply.result)} + detail + "\n");
    return success ? exit_ok : exit_refused;
}

int Dispatch(int argc, char** argv)
{
    Console::UseUtf8Output();
    const std::string program = argc > 0 ? std::filesystem::path{argv[0]}.filename().string() : "berayprocessmanager";
    const std::span<char* const> commandLine{argv, static_cast<std::size_t>(argc)};
    const std::vector<std::string> arguments = Console::Arguments(commandLine);
    Options options{};
    std::string error{};
    if (ParseCommandLine(arguments, options, error) != ParseCode::Ok)
    {
        Print(stderr, program + ": " + error + "\n");
        return exit_usage;
    }
    if (options.logLevel.has_value())
    {
        GlobalLogger().SetLevel(*options.logLevel);
    }

    switch (options.mode)
    {
    case Mode::Help:
        Print(stdout, UsageText(program));
        return exit_ok;
    case Mode::Version:
        Print(stdout, std::string{manager_name} + " " + std::string{manager_version} + " (libzmq " + ZmqVersion() + ")\n");
        return exit_ok;
    case Mode::Check:
        return RunCheck(options);
    case Mode::Run:
        return RunManager(options);
    case Mode::Status:
        return RunStatus(options);
    case Mode::Start:
    case Mode::Stop:
    case Mode::Restart:
    case Mode::Reload:
    case Mode::Heartbeat:
        return RunCommand(options);
    }
    return exit_usage;
}

} // namespace

} // namespace process_manager

int main(int argc, char** argv)
{
    return process_manager::Dispatch(argc, argv);
}
