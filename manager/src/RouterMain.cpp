#include "Console.hpp"
#include "Logger.hpp"
#include "MessageRouter.hpp"
#include "RouterOptions.hpp"
#include "SignalWatcher.hpp"
#include "Version.hpp"
#include "ZmqSocket.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace process_manager
{

namespace
{

constexpr int exit_ok = 0;
constexpr int exit_failed = 1;
constexpr int exit_usage = 2;
// Long enough to idle cheaply, short enough that Ctrl+C stops the router promptly.
constexpr std::chrono::milliseconds poll_wait{200};

void Print(std::FILE* stream, const std::string& text)
{
    std::fwrite(text.data(), 1, text.size(), stream);
    std::fflush(stream);
}

int RunRouter(const RouterOptions& options)
{
    if (options.logLevel.has_value())
    {
        GlobalLogger().SetLevel(*options.logLevel);
    }
    LogInfo(std::string{router_name} + " " + std::string{manager_version} + " starting (libzmq " + ZmqVersion() +
            ")");

    SignalWatcher::Install();
    ZmqContext context{};
    MessageRouter router{context};
    std::string error{};
    const std::string endpoint = options.endpoint.empty() ? std::string{default_router_endpoint} : options.endpoint;
    if (router.Bind(endpoint, error) != ZmqCode::Ok)
    {
        LogError(error);
        return exit_failed;
    }

    while (!SignalWatcher::TakeStopRequest())
    {
        router.Step(poll_wait);
    }
    LogInfo(std::string{router_name} + " stopped");
    return exit_ok;
}

int Dispatch(int argc, char** argv)
{
    Console::UseUtf8Output();
    const std::string program = argc > 0 ? std::filesystem::path{argv[0]}.filename().string() : std::string{router_name};
    const std::span<char* const> commandLine{argv, static_cast<std::size_t>(argc)};
    const std::vector<std::string> arguments = Console::Arguments(commandLine);
    RouterOptions options{};
    std::string error{};
    if (ParseRouterCommandLine(arguments, options, error) != RouterParseCode::Ok)
    {
        Print(stderr, program + ": " + error + "\n");
        return exit_usage;
    }

    switch (options.mode)
    {
    case RouterMode::Help:
        Print(stdout, RouterUsageText(program));
        return exit_ok;
    case RouterMode::Version:
        Print(stdout, std::string{router_name} + " " + std::string{manager_version} + " (libzmq " + ZmqVersion() + ")\n");
        return exit_ok;
    case RouterMode::Run:
        return RunRouter(options);
    }
    return exit_usage;
}

} // namespace

} // namespace process_manager

int main(int argc, char** argv)
{
    return process_manager::Dispatch(argc, argv);
}
