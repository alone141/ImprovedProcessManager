#include "RouterOptions.hpp"
#include "Logger.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace process_manager
{

RouterParseCode ParseRouterCommandLine(std::span<const std::string> arguments, RouterOptions& out, std::string& error)
{
    RouterOptions options{};
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

        const bool wantsValue = flag == "-b" || flag == "--bind" || flag == "--log-level";
        if (wantsValue && !hasInlineValue)
        {
            if (i + 1 >= arguments.size())
            {
                error = flag + " needs a value";
                return RouterParseCode::MissingValue;
            }
            value = arguments[++i];
        }

        if (flag == "-h" || flag == "--help")
        {
            options.mode = RouterMode::Help;
        }
        else if (flag == "-V" || flag == "--version")
        {
            options.mode = RouterMode::Version;
        }
        else if (flag == "-b" || flag == "--bind")
        {
            if (value.empty())
            {
                error = "--bind needs an endpoint such as tcp://*:5558";
                return RouterParseCode::InvalidValue;
            }
            options.endpoint = value;
        }
        else if (flag == "--log-level")
        {
            options.logLevel = ParseLogLevel(value);
            if (!options.logLevel.has_value())
            {
                error = "--log-level must be error, warning, info or debug";
                return RouterParseCode::InvalidValue;
            }
        }
        else if (flag == "-v" || flag == "--verbose")
        {
            options.logLevel = LogLevel::Debug;
        }
        else
        {
            error = "unknown option " + arguments[i] + " (see --help)";
            return RouterParseCode::UnknownOption;
        }
    }

    out = options;
    return RouterParseCode::Ok;
}

std::string RouterUsageText(std::string_view program)
{
    const std::string name{program};
    return "Usage:\n"
           "  " + name + " [--bind ENDPOINT] [--log-level LEVEL]\n"
           "\n"
           "Forwards each message from a connected DEALER to the peer named in its first\n"
           "frame; the receiver gets the sender's identity first, then the payload. This\n"
           "is how berayprocessmanager serves commands when its configuration names a\n"
           "router_endpoint (see docs/protocol.md).\n"
           "\n"
           "Options:\n"
           "  -b, --bind ENDPOINT     where to listen (default: tcp://*:5558)\n"
           "      --log-level LEVEL   error, warning, info or debug; debug logs every message\n"
           "  -v, --verbose           same as --log-level debug\n"
           "  -h, --help              show this text\n"
           "  -V, --version           show the version\n"
           "\n"
           "Exit status: 0 stopped by SIGTERM or Ctrl+C, 1 cannot listen, 2 usage error.\n";
}

} // namespace process_manager
