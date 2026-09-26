#include "Console.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace process_manager
{

void Console::UseUtf8Output()
{
}

bool Console::EnableEscapes()
{
    return ::isatty(STDOUT_FILENO) == 1;
}

std::vector<std::string> Console::Arguments(std::span<char* const> argv)
{
    std::vector<std::string> arguments{};
    for (std::size_t i = 1; i < argv.size(); ++i)
    {
        arguments.emplace_back(argv[i]);
    }
    return arguments;
}

} // namespace process_manager
