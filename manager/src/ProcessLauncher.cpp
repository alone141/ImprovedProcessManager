#include "ProcessLauncher.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace process_manager
{

namespace
{

// POSIX signal numbers are small; a larger negative code is never a signal.
constexpr std::int32_t max_signal_number = 64;

const char* SignalName(int signal)
{
    switch (signal)
    {
    case 1:
        return "HUP";
    case 2:
        return "INT";
    case 3:
        return "QUIT";
    case 4:
        return "ILL";
    case 6:
        return "ABRT";
    case 7:
        return "BUS";
    case 8:
        return "FPE";
    case 9:
        return "KILL";
    case 11:
        return "SEGV";
    case 13:
        return "PIPE";
    case 14:
        return "ALRM";
    case 15:
        return "TERM";
    default:
        return nullptr;
    }
}

} // namespace

bool ExitedCleanly(const ExitStatus& status)
{
    return status.code == 0 && status.signal == 0;
}

std::int32_t WireExitCode(const ExitStatus& status)
{
    return status.signal != 0 ? -status.signal : status.code;
}

std::string DescribeExitStatus(const ExitStatus& status)
{
    return status.signal != 0 ? DescribeExit(-status.signal, true) : DescribeExit(status.code, false);
}

std::string DescribeExit(std::int32_t wireCode, bool signals)
{
    if (wireCode >= 0)
    {
        return "exit " + std::to_string(wireCode);
    }
    if (signals && wireCode >= -max_signal_number)
    {
        const int signal = -wireCode;
        const char* name = SignalName(signal);
        return "signal " + std::to_string(signal) + (name != nullptr ? std::string{" ("} + name + ")" : "");
    }

    char text[24]{};
    std::snprintf(text, sizeof(text), "exit 0x%08X", static_cast<unsigned int>(wireCode));
    return std::string{text};
}

} // namespace process_manager
