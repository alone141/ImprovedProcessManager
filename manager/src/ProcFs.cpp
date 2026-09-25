#include "ProcFs.hpp"

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace process_manager
{

namespace
{

bool IsBlank(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::vector<std::string_view> SplitWords(std::string_view text)
{
    std::vector<std::string_view> words{};
    std::size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && IsBlank(text[i]))
        {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && !IsBlank(text[i]))
        {
            ++i;
        }
        if (i > start)
        {
            words.push_back(text.substr(start, i - start));
        }
    }
    return words;
}

std::vector<std::string_view> SplitLines(std::string_view text)
{
    std::vector<std::string_view> lines{};
    std::size_t start = 0;
    while (start < text.size())
    {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
        {
            end = text.size();
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::optional<std::uint64_t> ToUnsigned(std::string_view text)
{
    std::uint64_t value{0};
    const char* last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(text.data(), last, value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != last)
    {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> ToSigned(std::string_view text)
{
    std::int64_t value{0};
    const char* last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(text.data(), last, value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != last)
    {
        return std::nullopt;
    }
    return value;
}

std::uint64_t NonNegative(std::string_view text)
{
    const std::optional<std::int64_t> value = ToSigned(text);
    if (!value.has_value() || *value < 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(*value);
}

std::optional<double> ToDouble(std::string_view text)
{
    const std::string copy{text};
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size())
    {
        return std::nullopt;
    }
    return value;
}

// "Key: value" lines, as in meminfo and PID/io; the value's first word is returned.
std::optional<std::uint64_t> FindKeyedValue(std::string_view text, std::string_view key)
{
    for (const std::string_view line : SplitLines(text))
    {
        const std::vector<std::string_view> words = SplitWords(line);
        if (words.size() >= 2 && words[0].size() == key.size() + 1 && words[0].substr(0, key.size()) == key &&
            words[0].back() == ':')
        {
            return ToUnsigned(words[1]);
        }
    }
    return std::nullopt;
}

// "key value" lines, as in cgroup cpu.stat and memory.events.
std::optional<std::uint64_t> FindFlatValue(std::string_view text, std::string_view key)
{
    for (const std::string_view line : SplitLines(text))
    {
        const std::vector<std::string_view> words = SplitWords(line);
        if (words.size() >= 2 && words[0] == key)
        {
            return ToUnsigned(words[1]);
        }
    }
    return std::nullopt;
}

std::string UnescapeMountField(std::string_view field)
{
    std::string text{};
    for (std::size_t i = 0; i < field.size(); ++i)
    {
        if (field[i] == '\\' && i + 3 < field.size())
        {
            const std::string_view digits = field.substr(i + 1, 3);
            bool octal = true;
            int value = 0;
            for (const char c : digits)
            {
                octal = octal && c >= '0' && c <= '7';
                value = value * 8 + (c - '0');
            }
            if (octal)
            {
                text.push_back(static_cast<char>(value));
                i += 3;
                continue;
            }
        }
        text.push_back(field[i]);
    }
    return text;
}

} // namespace

bool ParseProcessStat(std::string_view text, ProcessStat& out)
{
    const std::size_t open = text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
    {
        return false;
    }

    const std::vector<std::string_view> head = SplitWords(text.substr(0, open));
    const std::vector<std::string_view> words = SplitWords(text.substr(close + 1));
    if (head.size() != 1 || words.size() < 22 || words[0].empty())
    {
        return false;
    }

    const std::optional<std::int64_t> pid = ToSigned(head[0]);
    const std::optional<std::int64_t> parent = ToSigned(words[1]);
    const std::optional<std::int64_t> group = ToSigned(words[2]);
    const std::optional<std::int64_t> session = ToSigned(words[3]);
    const std::optional<std::uint64_t> user = ToUnsigned(words[11]);
    const std::optional<std::uint64_t> system = ToUnsigned(words[12]);
    const std::optional<std::int64_t> threads = ToSigned(words[17]);
    const std::optional<std::uint64_t> start = ToUnsigned(words[19]);
    if (!pid || !parent || !group || !session || !user || !system || !threads || !start)
    {
        return false;
    }

    ProcessStat stat{};
    stat.pid = static_cast<int>(*pid);
    stat.comm = std::string{text.substr(open + 1, close - open - 1)};
    stat.state = words[0][0];
    stat.parentPid = static_cast<int>(*parent);
    stat.processGroup = static_cast<int>(*group);
    stat.session = static_cast<int>(*session);
    stat.userTicks = *user;
    stat.systemTicks = *system;
    stat.childUserTicks = NonNegative(words[13]);
    stat.childSystemTicks = NonNegative(words[14]);
    stat.threads = static_cast<int>(*threads);
    stat.startTicks = *start;
    stat.rssPages = NonNegative(words[21]);
    out = stat;
    return true;
}

bool ParseSystemCpu(std::string_view text, CpuTimes& out)
{
    for (const std::string_view line : SplitLines(text))
    {
        const std::vector<std::string_view> words = SplitWords(line);
        if (words.size() < 5 || words[0] != "cpu")
        {
            continue;
        }

        std::uint64_t values[8]{};
        for (std::size_t i = 0; i < 8 && i + 1 < words.size(); ++i)
        {
            values[i] = ToUnsigned(words[i + 1]).value_or(0);
        }
        // user nice system idle iowait irq softirq steal
        const std::uint64_t idle = values[3] + values[4];
        const std::uint64_t busy = values[0] + values[1] + values[2] + values[5] + values[6] + values[7];
        out = CpuTimes{busy, busy + idle};
        return true;
    }
    return false;
}

bool ParseMemInfo(std::string_view text, MemoryInfo& out)
{
    const std::optional<std::uint64_t> total = FindKeyedValue(text, "MemTotal");
    if (!total.has_value())
    {
        return false;
    }

    std::optional<std::uint64_t> available = FindKeyedValue(text, "MemAvailable");
    if (!available.has_value())
    {
        available = FindKeyedValue(text, "MemFree").value_or(0) + FindKeyedValue(text, "Buffers").value_or(0) +
                    FindKeyedValue(text, "Cached").value_or(0);
    }
    out = MemoryInfo{*total * 1024, *available * 1024};
    return true;
}

bool ParseLoadAverage(std::string_view text, LoadAverage& out)
{
    const std::vector<std::string_view> words = SplitWords(text);
    if (words.size() < 3)
    {
        return false;
    }

    const std::optional<double> one = ToDouble(words[0]);
    const std::optional<double> five = ToDouble(words[1]);
    const std::optional<double> fifteen = ToDouble(words[2]);
    if (!one || !five || !fifteen)
    {
        return false;
    }
    out = LoadAverage{*one, *five, *fifteen};
    return true;
}

bool ParseProcessIo(std::string_view text, IoCounters& out)
{
    const std::optional<std::uint64_t> read = FindKeyedValue(text, "read_bytes");
    const std::optional<std::uint64_t> written = FindKeyedValue(text, "write_bytes");
    if (!read || !written)
    {
        return false;
    }
    out = IoCounters{*read, *written};
    return true;
}

bool ParseCgroupCpuStat(std::string_view text, std::uint64_t& usageUsec)
{
    const std::optional<std::uint64_t> usage = FindFlatValue(text, "usage_usec");
    if (!usage.has_value())
    {
        return false;
    }
    usageUsec = *usage;
    return true;
}

std::vector<DeviceIo> ParseCgroupIoStat(std::string_view text)
{
    std::vector<DeviceIo> devices{};
    for (const std::string_view line : SplitLines(text))
    {
        const std::vector<std::string_view> words = SplitWords(line);
        if (words.empty())
        {
            continue;
        }
        DeviceIo device{};
        device.device = std::string{words.front()};
        for (std::size_t i = 1; i < words.size(); ++i)
        {
            const std::size_t equals = words[i].find('=');
            if (equals == std::string_view::npos)
            {
                continue;
            }
            const std::string_view key = words[i].substr(0, equals);
            const std::uint64_t value = ToUnsigned(words[i].substr(equals + 1)).value_or(0);
            if (key == "rbytes")
            {
                device.readBytes = value;
            }
            else if (key == "wbytes")
            {
                device.writeBytes = value;
            }
        }
        devices.push_back(device);
    }
    return devices;
}

bool ParseMemoryEvents(std::string_view text, std::uint32_t& oomKills)
{
    const std::optional<std::uint64_t> kills = FindFlatValue(text, "oom_kill");
    if (!kills.has_value())
    {
        return false;
    }
    oomKills = static_cast<std::uint32_t>(*kills);
    return true;
}

std::optional<std::string> FindCgroup2Mount(std::string_view mountInfo)
{
    for (const std::string_view line : SplitLines(mountInfo))
    {
        const std::vector<std::string_view> words = SplitWords(line);
        for (std::size_t i = 6; i + 1 < words.size(); ++i)
        {
            if (words[i] == "-")
            {
                if (words[i + 1] == "cgroup2" && words.size() > 4)
                {
                    return UnescapeMountField(words[4]);
                }
                break;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> FindUnifiedCgroupPath(std::string_view selfCgroup)
{
    constexpr std::string_view unified_prefix = "0::";
    for (const std::string_view line : SplitLines(selfCgroup))
    {
        if (line.substr(0, unified_prefix.size()) == unified_prefix)
        {
            std::string_view path = line.substr(unified_prefix.size());
            while (!path.empty() && IsBlank(path.back()))
            {
                path.remove_suffix(1);
            }
            return std::string{path};
        }
    }
    return std::nullopt;
}

std::filesystem::path Utf8Path(const std::string& text)
{
    const std::u8string utf8{text.begin(), text.end()};
    return std::filesystem::path{utf8};
}

std::optional<std::string> ReadTextFile(const std::string& path)
{
    std::ifstream file{Utf8Path(path), std::ios::binary};
    if (!file)
    {
        return std::nullopt;
    }

    std::ostringstream contents{};
    contents << file.rdbuf();
    return contents.str();
}

ProcFs::ProcFs(std::string root)
    : root{std::move(root)}
{
}

std::vector<int> ProcFs::ListPids() const
{
    std::vector<int> pids{};
    std::error_code error{};
    std::filesystem::directory_iterator entries{root, error};
    if (error)
    {
        return pids;
    }

    for (const std::filesystem::directory_entry& entry : entries)
    {
        const std::string name = entry.path().filename().string();
        const std::optional<std::uint64_t> pid = ToUnsigned(name);
        if (pid.has_value() && *pid > 0 && *pid <= 0x7fffffff)
        {
            pids.push_back(static_cast<int>(*pid));
        }
    }
    return pids;
}

bool ProcFs::ReadProcessStat(int pid, ProcessStat& out) const
{
    const std::optional<std::string> text = ReadTextFile(root + "/" + std::to_string(pid) + "/stat");
    return text.has_value() && ParseProcessStat(*text, out);
}

bool ProcFs::ReadProcessIo(int pid, IoCounters& out) const
{
    const std::optional<std::string> text = ReadTextFile(root + "/" + std::to_string(pid) + "/io");
    return text.has_value() && ParseProcessIo(*text, out);
}

int ProcFs::CountOpenFiles(int pid) const
{
    std::error_code error{};
    std::filesystem::directory_iterator entries{root + "/" + std::to_string(pid) + "/fd", error};
    if (error)
    {
        return -1;
    }

    int count = 0;
    for (std::filesystem::directory_iterator it = entries; it != std::filesystem::directory_iterator{};
         it.increment(error))
    {
        if (error)
        {
            return -1;
        }
        ++count;
    }
    return count;
}

bool ProcFs::ProcessExists(int pid) const
{
    std::error_code error{};
    return std::filesystem::is_directory(root + "/" + std::to_string(pid), error);
}

bool ProcFs::ReadSystemCpu(CpuTimes& out) const
{
    const std::optional<std::string> text = ReadTextFile(root + "/stat");
    return text.has_value() && ParseSystemCpu(*text, out);
}

bool ProcFs::ReadMemory(MemoryInfo& out) const
{
    const std::optional<std::string> text = ReadTextFile(root + "/meminfo");
    return text.has_value() && ParseMemInfo(*text, out);
}

bool ProcFs::ReadLoadAverage(LoadAverage& out) const
{
    const std::optional<std::string> text = ReadTextFile(root + "/loadavg");
    return text.has_value() && ParseLoadAverage(*text, out);
}

bool ProcFs::ReadUptime(std::uint64_t& seconds) const
{
    const std::optional<std::string> text = ReadTextFile(root + "/uptime");
    if (!text.has_value())
    {
        return false;
    }

    const std::vector<std::string_view> words = SplitWords(*text);
    const std::optional<double> value = words.empty() ? std::nullopt : ToDouble(words[0]);
    if (!value.has_value() || *value < 0)
    {
        return false;
    }
    seconds = static_cast<std::uint64_t>(*value);
    return true;
}

const std::string& ProcFs::Root() const
{
    return root;
}

} // namespace process_manager
