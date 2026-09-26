#include "CommandLine.hpp"
#include "Logger.hpp"
#include "ServiceConfig.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Windows only: the default configuration sits next to the executable.
#ifdef _WIN32
#include <windows.h>
#endif

namespace process_manager
{

namespace
{

constexpr long long max_timeout_ms = 600'000;

ParseCode SetMode(Options& options, Mode mode, bool& modeSet, const std::string& flag, std::string& error)
{
    if (modeSet && options.mode != mode)
    {
        error = flag + " cannot be combined with another mode";
        return ParseCode::ConflictingModes;
    }

    options.mode = mode;
    modeSet = true;
    return ParseCode::Ok;
}

bool TakeValue(std::span<const std::string> arguments, std::size_t& index, bool hasInlineValue, std::string& value)
{
    if (hasInlineValue)
    {
        return true;
    }
    if (index + 1 >= arguments.size())
    {
        return false;
    }

    value = arguments[++index];
    return true;
}

ParseCode SetServiceMode(Options& options, Mode mode, bool& modeSet, const std::string& flag, const std::string& value,
                         std::string& error)
{
    if (value != "*" && !IsValidServiceName(value))
    {
        error = "'" + value + "' is not a service name (1 to 31 of A-Z a-z 0-9 _ . -, or '*' for all)";
        return ParseCode::InvalidValue;
    }

    options.serviceName = value;
    return SetMode(options, mode, modeSet, flag, error);
}

} // namespace

ParseCode ParseCommandLine(std::span<const std::string> arguments, Options& out, std::string& error)
{
    Options options{};
    options.configPath = DefaultConfigPath();
    bool modeSet = false;
    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        std::string flag = arguments[i];
        std::string value{};
        bool hasInlineValue = false;
        const std::size_t equals = flag.find('=');
        if (flag.rfind("--", 0) == 0 && equals != std::string::npos)
        {
            value = flag.substr(equals + 1);
            flag = flag.substr(0, equals);
            hasInlineValue = true;
        }

        ParseCode code = ParseCode::Ok;
        const bool wantsValue = flag == "-c" || flag == "--config" || flag == "-s" || flag == "--start" ||
                                flag == "-k" || flag == "--stop" || flag == "-r" || flag == "--restart" ||
                                flag == "--heartbeat" || flag == "--command" || flag == "--report" ||
                                flag == "--router" || flag == "--manager" || flag == "--timeout" ||
                                flag == "--log-level";
        if (wantsValue && !TakeValue(arguments, i, hasInlineValue, value))
        {
            error = flag + " needs a value";
            return ParseCode::MissingValue;
        }

        if (flag == "-h" || flag == "--help")
        {
            code = SetMode(options, Mode::Help, modeSet, flag, error);
        }
        else if (flag == "-V" || flag == "--version")
        {
            code = SetMode(options, Mode::Version, modeSet, flag, error);
        }
        else if (flag == "-c" || flag == "--config")
        {
            options.configPath = value;
            options.configGiven = true;
        }
        else if (flag == "-l" || flag == "--status")
        {
            code = SetMode(options, Mode::Status, modeSet, flag, error);
        }
        else if (flag == "-w" || flag == "--watch")
        {
            options.watch = true;
            code = SetMode(options, Mode::Status, modeSet, flag, error);
        }
        else if (flag == "--wide")
        {
            options.wide = true;
        }
        else if (flag == "-s" || flag == "--start")
        {
            code = SetServiceMode(options, Mode::Start, modeSet, flag, value, error);
        }
        else if (flag == "-k" || flag == "--stop")
        {
            code = SetServiceMode(options, Mode::Stop, modeSet, flag, value, error);
        }
        else if (flag == "-r" || flag == "--restart")
        {
            code = SetServiceMode(options, Mode::Restart, modeSet, flag, value, error);
        }
        else if (flag == "--heartbeat")
        {
            code = value == "*" ? ParseCode::InvalidValue
                                : SetServiceMode(options, Mode::Heartbeat, modeSet, flag, value, error);
            if (value == "*")
            {
                error = "--heartbeat names one service";
            }
        }
        else if (flag == "--reload")
        {
            code = SetMode(options, Mode::Reload, modeSet, flag, error);
        }
        else if (flag == "--check")
        {
            code = SetMode(options, Mode::Check, modeSet, flag, error);
        }
        else if (flag == "--command")
        {
            options.commandEndpoint = value;
        }
        else if (flag == "--report")
        {
            options.reportEndpoint = value;
        }
        else if (flag == "--router")
        {
            if (value.empty())
            {
                error = "--router needs an endpoint such as tcp://127.0.0.1:5558";
                return ParseCode::InvalidValue;
            }
            options.routerEndpoint = value;
        }
        else if (flag == "--manager")
        {
            if (!IsValidIdentity(value))
            {
                error = "--manager must be 1 to 255 printable characters without spaces";
                return ParseCode::InvalidValue;
            }
            options.managerIdentity = value;
        }
        else if (flag == "--timeout")
        {
            long long ms = 0;
            const char* last = value.data() + value.size();
            const std::from_chars_result result = std::from_chars(value.data(), last, ms);
            if (result.ec != std::errc{} || result.ptr != last || ms < 1 || ms > max_timeout_ms)
            {
                error = "--timeout must be a number of milliseconds from 1 to " + std::to_string(max_timeout_ms);
                return ParseCode::InvalidValue;
            }
            options.timeout = std::chrono::milliseconds{ms};
            options.timeoutGiven = true;
        }
        else if (flag == "--log-level")
        {
            options.logLevel = ParseLogLevel(value);
            if (!options.logLevel.has_value())
            {
                error = "--log-level must be error, warning, info or debug";
                return ParseCode::InvalidValue;
            }
        }
        else if (flag == "-v" || flag == "--verbose")
        {
            options.logLevel = LogLevel::Debug;
        }
        else
        {
            error = "unknown option " + arguments[i] + " (see --help)";
            return ParseCode::UnknownOption;
        }

        if (code != ParseCode::Ok)
        {
            return code;
        }
    }

    out = options;
    return ParseCode::Ok;
}

std::string UsageText(std::string_view program)
{
    const std::string name{program};
    return "Usage:\n"
           "  " + name + " [--config FILE]            run the manager (the default mode)\n"
           "  " + name + " --status [--watch] [--wide] show every service of a running manager\n"
           "  " + name + " --start NAME              start a service ('*' for all)\n"
           "  " + name + " --stop NAME               stop a service ('*' for all)\n"
           "  " + name + " --restart NAME            restart a service ('*' for all)\n"
           "  " + name + " --reload                  make the running manager re-read its configuration\n"
           "  " + name + " --heartbeat NAME          report that a service is alive (for scripts)\n"
           "  " + name + " --check [--config FILE]   validate a configuration file and list its services\n"
           "\n"
           "Options:\n"
           "  -c, --config FILE       configuration file (default: " + DefaultConfigPath() + ")\n"
           "  -l, --status            show the status table\n"
           "  -w, --watch             with --status: redraw on every report until Ctrl+C\n"
           "      --wide              with --status: add restart mode, threads, open files, I/O,\n"
           "                          heartbeat age and binary\n"
           "  -s, --start NAME        start NAME and the services it depends on\n"
           "  -k, --stop NAME         stop NAME\n"
           "  -r, --restart NAME      restart NAME\n"
           "      --command ENDPOINT  the manager's command endpoint\n"
           "                          (default: from the configuration, else tcp://127.0.0.1:5557)\n"
           "      --report ENDPOINT   the manager's detailed report endpoint\n"
           "                          (default: from the configuration, else tcp://127.0.0.1:6668)\n"
           "      --router ENDPOINT   send the command through a router (beraynetworkmanager)\n"
           "                          instead of the command endpoint\n"
           "      --manager NAME      with --router: the manager's identity on the router\n"
           "                          (default: from the configuration, else berayprocessmanager)\n"
           "      --timeout MS        how long client modes wait for the manager (default: 3000)\n"
           "      --log-level LEVEL   error, warning, info or debug (run mode)\n"
           "  -v, --verbose           same as --log-level debug\n"
           "  -h, --help              show this text\n"
           "  -V, --version           show the version\n"
           "\n"
           "Exit status: 0 done, 1 refused or failed, 2 usage or configuration error,\n"
           "3 no answer from the manager.\n";
}

std::string DefaultConfigPath()
{
#ifdef _WIN32
    std::wstring path{};
    path.resize(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length);
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring directory = slash == std::wstring::npos ? std::wstring{L"."} : path.substr(0, slash);
    const std::wstring file = directory + L"\\services.conf";
    const int size = WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string narrow{};
    narrow.resize(static_cast<std::size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, file.data(), static_cast<int>(file.size()), narrow.data(), size, nullptr, nullptr);
    return narrow;
#else
    return "/etc/berayprocessmanager/services.conf";
#endif
}

std::string ConnectEndpoint(std::string_view bindEndpoint)
{
    constexpr std::string_view tcp = "tcp://";
    if (bindEndpoint.substr(0, tcp.size()) != tcp)
    {
        return std::string{bindEndpoint};
    }

    const std::size_t colon = bindEndpoint.rfind(':');
    if (colon == std::string_view::npos || colon < tcp.size())
    {
        return std::string{bindEndpoint};
    }
    const std::string_view host = bindEndpoint.substr(tcp.size(), colon - tcp.size());
    const std::string port{bindEndpoint.substr(colon)};
    if (host == "*" || host == "0.0.0.0")
    {
        return "tcp://127.0.0.1" + port;
    }
    if (host == "[::]" || host == "::")
    {
        return "tcp://[::1]" + port;
    }
    return std::string{bindEndpoint};
}

} // namespace process_manager
