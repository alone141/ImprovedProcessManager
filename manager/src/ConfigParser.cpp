#include "ConfigParser.hpp"
#include "Logger.hpp"
#include "ProcFs.hpp"
#include "ServiceConfig.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace process_manager
{

namespace
{

constexpr std::int64_t max_duration_ms = 86'400'000;
constexpr std::int64_t max_window_s = 86'400;
constexpr std::int64_t max_count = 1'000'000;
constexpr std::int64_t min_publish_ms = 100;
constexpr std::int64_t max_publish_ms = 60'000;
constexpr std::uint32_t max_cpu_percent = 100 * 4096;

enum class Section
{
    None,
    Manager,
    Defaults,
    Service,
};

struct ParseState
{
    Config config;
    ServiceConfig defaults;
    Section section{Section::None};
    std::size_t current{0};
    std::set<std::string> seenKeys;
    bool managerSeen{false};
    bool defaultsSeen{false};
    std::vector<int> serviceLines;
    std::vector<int> dependencyLines;
};

bool IsBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && IsBlank(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsBlank(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view CutAtComment(std::string_view value, std::size_t from)
{
    for (std::size_t i = from; i < value.size(); ++i)
    {
        if ((value[i] == '#' || value[i] == ';') && (i == 0 || IsBlank(value[i - 1])))
        {
            return value.substr(0, i);
        }
    }
    return value;
}

// Quotes protect a " #" the way the value is read later: anywhere for shell-split
// values (args, env), only as a whole-value quote for everything else, so an
// apostrophe in "Bob's camera" does not hide the comment after it.
std::string_view StripInlineComment(std::string_view value, bool shellQuotes)
{
    if (!shellQuotes)
    {
        std::size_t first = 0;
        while (first < value.size() && IsBlank(value[first]))
        {
            ++first;
        }
        if (first == value.size() || (value[first] != '"' && value[first] != '\''))
        {
            return CutAtComment(value, 0);
        }
        const char opening = value[first];
        std::size_t i = first + 1;
        while (i < value.size() && value[i] != opening)
        {
            const bool escape = opening == '"' && value[i] == '\\' && i + 1 < value.size() &&
                                (value[i + 1] == '"' || value[i + 1] == '\\');
            i += escape ? 2 : 1;
        }
        return i < value.size() ? CutAtComment(value, i + 1) : value;
    }

    char quote = '\0';
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        const char c = value[i];
        if (quote != '\0')
        {
            if (c == quote)
            {
                quote = '\0';
            }
            else if (quote == '"' && c == '\\' && i + 1 < value.size())
            {
                ++i;
            }
            continue;
        }
        if (c == '"' || c == '\'')
        {
            quote = c;
            continue;
        }
        if ((c == '#' || c == ';') && (i == 0 || IsBlank(value[i - 1])))
        {
            return value.substr(0, i);
        }
    }
    return value;
}

std::string Unquote(std::string_view value)
{
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')
    {
        return std::string{value.substr(1, value.size() - 2)};
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    {
        const std::string_view inner = value.substr(1, value.size() - 2);
        std::string text{};
        for (std::size_t i = 0; i < inner.size(); ++i)
        {
            if (inner[i] == '\\' && i + 1 < inner.size() && (inner[i + 1] == '"' || inner[i + 1] == '\\'))
            {
                text.push_back(inner[i + 1]);
                ++i;
            }
            else
            {
                text.push_back(inner[i]);
            }
        }
        return text;
    }
    return std::string{value};
}

std::optional<std::int64_t> ParseInteger(std::string_view text)
{
    if (text.empty())
    {
        return std::nullopt;
    }

    std::int64_t value{0};
    const char* last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(text.data(), last, value);
    if (result.ec != std::errc{} || result.ptr != last)
    {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> ParseBool(std::string_view text)
{
    if (text == "true" || text == "yes" || text == "on" || text == "1")
    {
        return true;
    }
    if (text == "false" || text == "no" || text == "off" || text == "0")
    {
        return false;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ParsePercent(std::string_view text)
{
    if (!text.empty() && text.back() == '%')
    {
        text.remove_suffix(1);
    }

    const std::optional<std::int64_t> number = ParseInteger(Trim(text));
    if (!number.has_value() || *number < 0 || *number > static_cast<std::int64_t>(max_cpu_percent))
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*number);
}

std::vector<std::string> SplitList(std::string_view text)
{
    std::vector<std::string> items{};
    std::string current{};
    for (const char c : text)
    {
        if (c == ',' || IsBlank(c))
        {
            if (!current.empty())
            {
                items.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
    {
        items.push_back(current);
    }
    return items;
}

std::string LowerCase(std::string_view text)
{
    std::string lower{text};
    for (char& c : lower)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return lower;
}

ConfigCode Fail(ConfigError& error, ConfigCode code, int line, std::string message)
{
    error.code = code;
    error.line = line;
    error.message = std::move(message);
    return code;
}

ConfigCode ReadNumber(std::string_view key, std::string_view value, int line, std::int64_t minimum,
                      std::int64_t maximum, std::int64_t& out, ConfigError& error)
{
    const std::optional<std::int64_t> number = ParseInteger(value);
    if (!number.has_value() || *number < minimum || *number > maximum)
    {
        return Fail(error, ConfigCode::InvalidValue, line,
                    std::string{key} + " must be a whole number from " + std::to_string(minimum) + " to " +
                        std::to_string(maximum));
    }

    out = *number;
    return ConfigCode::Ok;
}

ConfigCode ApplyManagerKey(ManagerSettings& manager, std::string_view key, std::string_view value, int line,
                           ConfigError& error)
{
    if (key == "health_endpoint" || key == "report_endpoint" || key == "command_endpoint")
    {
        const std::string endpoint = Unquote(value);
        if (endpoint.empty())
        {
            return Fail(error, ConfigCode::MissingValue, line, std::string{key} + " needs an endpoint");
        }
        if (key == "health_endpoint")
        {
            manager.healthEndpoint = endpoint;
        }
        else if (key == "report_endpoint")
        {
            manager.reportEndpoint = endpoint;
        }
        else
        {
            manager.commandEndpoint = endpoint;
        }
        return ConfigCode::Ok;
    }
    if (key == "publish_interval_ms")
    {
        std::int64_t ms{0};
        const ConfigCode code = ReadNumber(key, value, line, min_publish_ms, max_publish_ms, ms, error);
        manager.publishInterval = std::chrono::milliseconds{ms};
        return code;
    }
    if (key == "log_level")
    {
        const std::optional<LogLevel> level = ParseLogLevel(value);
        if (!level.has_value())
        {
            return Fail(error, ConfigCode::InvalidValue, line, "log_level must be error, warning, info or debug");
        }
        manager.logLevel = *level;
        return ConfigCode::Ok;
    }
    if (key == "cgroups")
    {
        if (value == "auto")
        {
            manager.cgroups = CgroupMode::Auto;
        }
        else if (value == "off")
        {
            manager.cgroups = CgroupMode::Off;
        }
        else if (value == "required")
        {
            manager.cgroups = CgroupMode::Required;
        }
        else
        {
            return Fail(error, ConfigCode::InvalidValue, line, "cgroups must be auto, off or required");
        }
        return ConfigCode::Ok;
    }
    if (key == "gpu")
    {
        if (value == "auto")
        {
            manager.gpu = GpuMode::Auto;
        }
        else if (value == "off")
        {
            manager.gpu = GpuMode::Off;
        }
        else
        {
            return Fail(error, ConfigCode::InvalidValue, line, "gpu must be auto or off");
        }
        return ConfigCode::Ok;
    }
    return Fail(error, ConfigCode::UnknownKey, line, "unknown key '" + std::string{key} + "' in [manager]");
}

ConfigCode ApplyOutput(ServiceConfig& service, std::string_view value, int line, ConfigError& error)
{
    if (value == "auto")
    {
        service.output = OutputMode::Auto;
    }
    else if (value == "journal")
    {
        service.output = OutputMode::Journal;
    }
    else if (value == "inherit")
    {
        service.output = OutputMode::Inherit;
    }
    else if (value == "null")
    {
        service.output = OutputMode::Null;
    }
    else
    {
        const std::string path = Unquote(value);
        if (path.empty() || !Utf8Path(path).is_absolute())
        {
            return Fail(error, ConfigCode::InvalidValue, line,
                        "output must be auto, journal, inherit, null or an absolute file path");
        }
        service.output = OutputMode::File;
        service.outputPath = path;
    }
    return ConfigCode::Ok;
}

ConfigCode ApplyDuration(std::string_view key, std::string_view value, int line, std::chrono::milliseconds& out,
                         ConfigError& error)
{
    std::int64_t ms{0};
    const ConfigCode code = ReadNumber(key, value, line, 0, max_duration_ms, ms, error);
    out = std::chrono::milliseconds{ms};
    return code;
}

ConfigCode ApplyServiceKey(ServiceConfig& service, bool defaults, std::string_view key, std::string_view value,
                           int line, int& dependencyLine, ConfigError& error)
{
    const std::string where = defaults ? std::string{"[defaults]"} : "[service " + service.name + "]";
    if (defaults && (key == "binary" || key == "args" || key == "description" || key == "depends_on"))
    {
        return Fail(error, ConfigCode::UnknownKey, line, std::string{key} + " belongs in a [service] section");
    }

    if (key == "description")
    {
        service.description = Unquote(value);
        return ConfigCode::Ok;
    }
    if (key == "binary")
    {
        service.binary = Unquote(value);
        if (service.binary.empty())
        {
            return Fail(error, ConfigCode::MissingValue, line, "binary needs a path");
        }
        return ConfigCode::Ok;
    }
    if (key == "args")
    {
        std::vector<std::string> arguments{};
        if (SplitArguments(value, arguments) != SplitCode::Ok)
        {
            return Fail(error, ConfigCode::InvalidValue, line, "args has an unterminated quote");
        }
        service.arguments = std::move(arguments);
        return ConfigCode::Ok;
    }
    if (key == "working_dir")
    {
        // Relative, it would depend on the directory the manager happened to start in.
        const std::string directory = Unquote(value);
        if (!directory.empty() && !Utf8Path(directory).has_root_directory())
        {
            return Fail(error, ConfigCode::InvalidValue, line, "working_dir must be an absolute path");
        }
        service.workingDirectory = directory;
        return ConfigCode::Ok;
    }
    if (key == "env")
    {
        const std::size_t equals = value.find('=');
        const std::string_view name = Trim(value.substr(0, equals));
        if (equals == std::string_view::npos || name.empty())
        {
            return Fail(error, ConfigCode::InvalidValue, line, "env must look like NAME=value");
        }
        service.environment.push_back(EnvironmentVariable{std::string{name}, Unquote(Trim(value.substr(equals + 1)))});
        return ConfigCode::Ok;
    }
    if (key == "autostart")
    {
        const std::optional<bool> flag = ParseBool(value);
        if (!flag.has_value())
        {
            return Fail(error, ConfigCode::InvalidValue, line, "autostart must be true or false");
        }
        service.autostart = *flag;
        return ConfigCode::Ok;
    }
    if (key == "restart")
    {
        const std::optional<RestartMode> mode = ParseRestartMode(value);
        if (!mode.has_value())
        {
            return Fail(error, ConfigCode::InvalidValue, line, "restart must be never, on-failure or always");
        }
        service.restart = *mode;
        return ConfigCode::Ok;
    }
    if (key == "restart_delay_ms")
    {
        return ApplyDuration(key, value, line, service.restartDelay, error);
    }
    if (key == "restart_delay_max_ms")
    {
        return ApplyDuration(key, value, line, service.restartDelayMax, error);
    }
    if (key == "start_grace_ms")
    {
        return ApplyDuration(key, value, line, service.startGrace, error);
    }
    if (key == "stop_timeout_ms")
    {
        return ApplyDuration(key, value, line, service.stopTimeout, error);
    }
    if (key == "heartbeat_interval_ms")
    {
        return ApplyDuration(key, value, line, service.heartbeatInterval, error);
    }
    if (key == "max_restarts")
    {
        std::int64_t count{0};
        const ConfigCode code = ReadNumber(key, value, line, 0, max_count, count, error);
        service.maxRestarts = static_cast<int>(count);
        return code;
    }
    if (key == "restart_window_s")
    {
        std::int64_t seconds{0};
        const ConfigCode code = ReadNumber(key, value, line, 1, max_window_s, seconds, error);
        service.restartWindow = std::chrono::seconds{seconds};
        return code;
    }
    if (key == "heartbeat_tolerance")
    {
        std::int64_t count{0};
        const ConfigCode code = ReadNumber(key, value, line, 1, max_count, count, error);
        service.heartbeatTolerance = static_cast<int>(count);
        return code;
    }
    if (key == "stop_signal")
    {
        const std::optional<StopSignal> signal = ParseStopSignal(value);
        if (!signal.has_value())
        {
            return Fail(error, ConfigCode::InvalidValue, line,
                        "stop_signal must be TERM, INT, HUP, QUIT, KILL, USR1 or USR2");
        }
        service.stopSignal = *signal;
        return ConfigCode::Ok;
    }
    if (key == "unhealthy_action")
    {
        if (value == "none")
        {
            service.unhealthyAction = UnhealthyAction::None;
        }
        else if (value == "restart")
        {
            service.unhealthyAction = UnhealthyAction::Restart;
        }
        else
        {
            return Fail(error, ConfigCode::InvalidValue, line, "unhealthy_action must be none or restart");
        }
        return ConfigCode::Ok;
    }
    if (key == "depends_on")
    {
        service.dependsOn = SplitList(value);
        dependencyLine = line;
        return ConfigCode::Ok;
    }
    if (key == "output")
    {
        return ApplyOutput(service, value, line, error);
    }
    if (key == "memory_max")
    {
        if (value == "none")
        {
            service.memoryMax = 0;
            return ConfigCode::Ok;
        }
        const std::optional<std::uint64_t> bytes = ParseByteSize(value);
        if (!bytes.has_value())
        {
            return Fail(error, ConfigCode::InvalidValue, line, "memory_max must be a size such as 512M, or none");
        }
        service.memoryMax = *bytes;
        return ConfigCode::Ok;
    }
    if (key == "cpu_max")
    {
        if (value == "none")
        {
            service.cpuMaxPercent = 0;
            return ConfigCode::Ok;
        }
        const std::optional<std::uint32_t> percent = ParsePercent(value);
        if (!percent.has_value())
        {
            return Fail(error, ConfigCode::InvalidValue, line,
                        "cpu_max must be a percentage of one core such as 150%, or none");
        }
        service.cpuMaxPercent = *percent;
        return ConfigCode::Ok;
    }
    return Fail(error, ConfigCode::UnknownKey, line, "unknown key '" + std::string{key} + "' in " + where);
}

ConfigCode OpenSection(ParseState& state, std::string_view inner, int line, ConfigError& error)
{
    state.seenKeys.clear();
    if (inner == "manager")
    {
        if (state.managerSeen)
        {
            return Fail(error, ConfigCode::DuplicateKey, line, "[manager] appears twice");
        }
        state.managerSeen = true;
        state.section = Section::Manager;
        return ConfigCode::Ok;
    }
    if (inner == "defaults")
    {
        if (!state.config.services.empty())
        {
            return Fail(error, ConfigCode::Syntax, line, "[defaults] must come before the first [service]");
        }
        if (state.defaultsSeen)
        {
            return Fail(error, ConfigCode::DuplicateKey, line, "[defaults] appears twice");
        }
        state.defaultsSeen = true;
        state.section = Section::Defaults;
        return ConfigCode::Ok;
    }

    constexpr std::string_view service_word = "service";
    if (inner.size() > service_word.size() && inner.substr(0, service_word.size()) == service_word &&
        IsBlank(inner[service_word.size()]))
    {
        const std::string name{Trim(inner.substr(service_word.size()))};
        if (!IsValidServiceName(name))
        {
            return Fail(error, ConfigCode::InvalidName, line,
                        "service name '" + name + "' must be 1 to 31 of A-Z a-z 0-9 _ . -");
        }
        if (FindService(state.config, name) != nullptr)
        {
            return Fail(error, ConfigCode::DuplicateService, line, "service '" + name + "' is defined twice");
        }
        ServiceConfig service = state.defaults;
        service.name = name;
        state.config.services.push_back(std::move(service));
        state.serviceLines.push_back(line);
        state.dependencyLines.push_back(0);
        state.current = state.config.services.size() - 1;
        state.section = Section::Service;
        return ConfigCode::Ok;
    }
    return Fail(error, ConfigCode::UnknownSection, line,
                "unknown section [" + std::string{inner} + "]; expected [manager], [defaults] or [service NAME]");
}

ConfigCode ParseLine(ParseState& state, std::string_view raw, int line, ConfigError& error)
{
    const std::string_view text = Trim(raw);
    if (text.empty() || text.front() == '#' || text.front() == ';')
    {
        return ConfigCode::Ok;
    }

    if (text.front() == '[')
    {
        const std::string_view header = Trim(StripInlineComment(text, false));
        if (header.back() != ']')
        {
            return Fail(error, ConfigCode::Syntax, line, "section header must end with ]");
        }
        return OpenSection(state, Trim(header.substr(1, header.size() - 2)), line, error);
    }

    const std::size_t equals = text.find('=');
    if (equals == std::string_view::npos)
    {
        return Fail(error, ConfigCode::Syntax, line, "expected key = value");
    }
    const std::string key{Trim(text.substr(0, equals))};
    const bool shellQuotes = key == "args" || key == "env";
    const std::string_view value = Trim(StripInlineComment(text.substr(equals + 1), shellQuotes));
    if (key.empty())
    {
        return Fail(error, ConfigCode::Syntax, line, "missing key before =");
    }
    if (state.section == Section::None)
    {
        return Fail(error, ConfigCode::Syntax, line, "key '" + key + "' is outside any section");
    }
    if (key != "env" && !state.seenKeys.insert(key).second)
    {
        return Fail(error, ConfigCode::DuplicateKey, line, "key '" + key + "' appears twice in this section");
    }

    if (state.section == Section::Manager)
    {
        return ApplyManagerKey(state.config.manager, key, value, line, error);
    }
    if (state.section == Section::Defaults)
    {
        int ignored{0};
        return ApplyServiceKey(state.defaults, true, key, value, line, ignored, error);
    }
    return ApplyServiceKey(state.config.services[state.current], false, key, value, line,
                           state.dependencyLines[state.current], error);
}

// Depth-first search; mark 1 is "on the current path", 2 is "finished".
bool VisitForCycle(std::span<const ServiceConfig> services, const std::map<std::string, std::size_t>& index,
                   std::size_t node, std::vector<int>& mark, std::vector<std::string>& path, std::string& cycle)
{
    mark[node] = 1;
    path.push_back(services[node].name);
    for (const std::string& dependency : services[node].dependsOn)
    {
        const std::size_t next = index.at(dependency);
        if (mark[next] == 1)
        {
            cycle.clear();
            bool inCycle = false;
            for (const std::string& name : path)
            {
                inCycle = inCycle || name == dependency;
                if (inCycle)
                {
                    cycle += name + " -> ";
                }
            }
            cycle += dependency;
            return true;
        }
        if (mark[next] == 0 && VisitForCycle(services, index, next, mark, path, cycle))
        {
            return true;
        }
    }
    path.pop_back();
    mark[node] = 2;
    return false;
}

ConfigCode FindCycle(std::span<const ServiceConfig> services, const std::map<std::string, std::size_t>& index,
                     std::string& cycle)
{
    std::vector<int> mark{};
    mark.resize(services.size(), 0);
    std::vector<std::string> path{};
    for (std::size_t i = 0; i < services.size(); ++i)
    {
        if (mark[i] == 0 && VisitForCycle(services, index, i, mark, path, cycle))
        {
            return ConfigCode::DependencyCycle;
        }
    }
    return ConfigCode::Ok;
}

ConfigCode Validate(const ParseState& state, ConfigError& error)
{
    const std::vector<ServiceConfig>& services = state.config.services;
    std::map<std::string, std::size_t> index{};
    for (std::size_t i = 0; i < services.size(); ++i)
    {
        index[services[i].name] = i;
    }

    for (std::size_t i = 0; i < services.size(); ++i)
    {
        const ServiceConfig& service = services[i];
        if (service.binary.empty())
        {
            return Fail(error, ConfigCode::MissingValue, state.serviceLines[i],
                        "service '" + service.name + "' has no binary");
        }
        if (service.restartDelayMax < service.restartDelay)
        {
            return Fail(error, ConfigCode::InvalidValue, state.serviceLines[i],
                        "service '" + service.name + "': restart_delay_max_ms is below restart_delay_ms");
        }
        for (const std::string& dependency : service.dependsOn)
        {
            const int line = state.dependencyLines[i] != 0 ? state.dependencyLines[i] : state.serviceLines[i];
            if (dependency == service.name)
            {
                return Fail(error, ConfigCode::DependencyCycle, line,
                            "service '" + service.name + "' depends on itself");
            }
            if (index.count(dependency) == 0)
            {
                return Fail(error, ConfigCode::UnknownDependency, line,
                            "service '" + service.name + "' depends on unknown service '" + dependency + "'");
            }
        }
    }

    std::string cycle{};
    if (FindCycle(services, index, cycle) != ConfigCode::Ok)
    {
        return Fail(error, ConfigCode::DependencyCycle, 0, "dependency cycle: " + cycle);
    }

    const ManagerSettings& manager = state.config.manager;
    if (manager.healthEndpoint == manager.reportEndpoint || manager.healthEndpoint == manager.commandEndpoint ||
        manager.reportEndpoint == manager.commandEndpoint)
    {
        return Fail(error, ConfigCode::InvalidValue, 0,
                    "health_endpoint, report_endpoint and command_endpoint must all differ");
    }
    return ConfigCode::Ok;
}

} // namespace

ConfigCode ConfigParser::ParseText(std::string_view text, Config& out, ConfigError& error) const
{
    constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";
    if (text.substr(0, utf8_bom.size()) == utf8_bom)
    {
        text.remove_prefix(utf8_bom.size());
    }

    ParseState state{};
    int line = 0;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t end = text.find('\n', start);
        const std::string_view raw =
            text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        ++line;
        const ConfigCode code = ParseLine(state, raw, line, error);
        if (code != ConfigCode::Ok)
        {
            return code;
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        start = end + 1;
    }

    const ConfigCode code = Validate(state, error);
    if (code != ConfigCode::Ok)
    {
        return code;
    }

    out = std::move(state.config);
    error = ConfigError{};
    return ConfigCode::Ok;
}

ConfigCode ConfigParser::ParseFile(const std::string& path, Config& out, ConfigError& error) const
{
    const std::optional<std::string> contents = ReadTextFile(path);
    if (!contents.has_value())
    {
        return Fail(error, ConfigCode::Unreadable, 0, "cannot read " + path);
    }

    const ConfigCode code = ParseText(*contents, out, error);
    if (code == ConfigCode::Ok)
    {
        out.path = path;
    }
    return code;
}

SplitCode SplitArguments(std::string_view text, std::vector<std::string>& out)
{
    std::vector<std::string> arguments{};
    std::string current{};
    bool inToken = false;
    char quote = '\0';
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char c = text[i];
        if (quote == '\'')
        {
            if (c == '\'')
            {
                quote = '\0';
            }
            else
            {
                current.push_back(c);
            }
            continue;
        }
        if (quote == '"')
        {
            if (c == '\\' && i + 1 < text.size() && (text[i + 1] == '"' || text[i + 1] == '\\'))
            {
                current.push_back(text[i + 1]);
                ++i;
            }
            else if (c == '"')
            {
                quote = '\0';
            }
            else
            {
                current.push_back(c);
            }
            continue;
        }
        if (IsBlank(c))
        {
            if (inToken)
            {
                arguments.push_back(current);
                current.clear();
                inToken = false;
            }
            continue;
        }
        if (c == '\'' || c == '"')
        {
            quote = c;
        }
        else
        {
            current.push_back(c);
        }
        inToken = true;
    }

    if (quote != '\0')
    {
        return SplitCode::UnterminatedQuote;
    }
    if (inToken)
    {
        arguments.push_back(current);
    }
    out = std::move(arguments);
    return SplitCode::Ok;
}

std::optional<std::uint64_t> ParseByteSize(std::string_view text)
{
    text = Trim(text);
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9')
    {
        ++digits;
    }
    if (digits == 0)
    {
        return std::nullopt;
    }

    const std::optional<std::int64_t> number = ParseInteger(text.substr(0, digits));
    const std::string suffix = LowerCase(Trim(text.substr(digits)));
    const std::map<std::string, int> powers{{"", 0},  {"b", 0},   {"k", 1},  {"kb", 1},  {"kib", 1},
                                            {"m", 2}, {"mb", 2},  {"mib", 2}, {"g", 3},  {"gb", 3},
                                            {"gib", 3}, {"t", 4}, {"tb", 4}, {"tib", 4}};
    const std::map<std::string, int>::const_iterator power = powers.find(suffix);
    if (!number.has_value() || power == powers.end())
    {
        return std::nullopt;
    }

    std::uint64_t bytes = static_cast<std::uint64_t>(*number);
    for (int i = 0; i < power->second; ++i)
    {
        if (bytes > std::numeric_limits<std::uint64_t>::max() / 1024)
        {
            return std::nullopt;
        }
        bytes *= 1024;
    }
    return bytes;
}

} // namespace process_manager
