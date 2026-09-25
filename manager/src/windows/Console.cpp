#include "Console.hpp"

// windows.h must come before the other Win32 headers, which rely on its types.
#include <windows.h>

#include <cstddef>
#include <cstdlib>
#include <shellapi.h>
#include <span>
#include <string>
#include <vector>

namespace process_manager
{

namespace
{

UINT originalCodePage = 0;
DWORD originalMode = 0;
bool modeChanged = false;

void RestoreCodePage()
{
    SetConsoleOutputCP(originalCodePage);
}

void RestoreMode()
{
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), originalMode);
}

} // namespace

void Console::UseUtf8Output()
{
    const UINT current = GetConsoleOutputCP();
    if (current == 0 || current == CP_UTF8 || originalCodePage != 0)
    {
        return;
    }
    originalCodePage = current;
    if (SetConsoleOutputCP(CP_UTF8))
    {
        std::atexit(RestoreCodePage);
    }
}

bool Console::EnableEscapes()
{
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output == INVALID_HANDLE_VALUE || !GetConsoleMode(output, &mode))
    {
        return false;
    }
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
    {
        return true;
    }
    if (!SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
    {
        return false;
    }
    if (!modeChanged)
    {
        originalMode = mode;
        modeChanged = true;
        std::atexit(RestoreMode);
    }
    return true;
}

std::vector<std::string> Console::Arguments(std::span<char* const> argv)
{
    std::vector<std::string> arguments{};
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr)
    {
        for (std::size_t i = 1; i < argv.size(); ++i)
        {
            arguments.emplace_back(argv[i]);
        }
        return arguments;
    }

    for (int i = 1; i < count; ++i)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string text{};
        text.resize(size > 0 ? static_cast<std::size_t>(size - 1) : 0);
        if (size > 1)
        {
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), size, nullptr, nullptr);
        }
        arguments.push_back(text);
    }
    LocalFree(wide);
    return arguments;
}

} // namespace process_manager
