#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace process_manager
{

constexpr std::string_view clear_screen = "\x1b[H\x1b[2J";

class Console
{
public:
    /**
     * @brief Print UTF-8 correctly: on Windows the console's output code page becomes
     *        UTF-8 until the program exits, then the original one is restored.
     */
    static void UseUtf8Output();

    /**
     * @brief Prepare standard output for the watch view: ANSI escape sequences, which
     *        Windows consoles only process on request; the console mode is restored at exit.
     * @return true when standard output is a terminal that understands escape sequences.
     */
    static bool EnableEscapes();

    /**
     * @brief The program's arguments as UTF-8.
     * @param argv Arguments from main, the program name first; on Windows they are read
     *        again as UTF-16 instead.
     * @return The arguments without the program name.
     */
    static std::vector<std::string> Arguments(std::span<char* const> argv);
};

} // namespace process_manager
