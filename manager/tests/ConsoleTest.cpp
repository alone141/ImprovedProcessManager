#include <gtest/gtest.h>

#include "Console.hpp"

#include <span>
#include <string>
#include <vector>

TEST(ConsoleTest, ClearScreenIsAnAnsiSequence)
{
    EXPECT_EQ(process_manager::clear_screen.substr(0, 2), "\x1b[");
}

#ifndef _WIN32

TEST(ConsoleTest, ArgumentsSkipTheProgramName)
{
    char program[] = "berayprocessmanager";
    char first[] = "--status";
    char second[] = "--wide";
    char* argv[] = {program, first, second, nullptr};
    const std::vector<std::string> arguments = process_manager::Console::Arguments(std::span<char* const>{argv, 3});
    EXPECT_EQ(arguments, (std::vector<std::string>{"--status", "--wide"}));
}

#else

TEST(ConsoleTest, ArgumentsComeFromTheWideCommandLine)
{
    char program[] = "berayprocessmanager";
    char* argv[] = {program, nullptr};
    const std::vector<std::string> arguments = process_manager::Console::Arguments(std::span<char* const>{argv, 1});
    for (const std::string& argument : arguments)
    {
        EXPECT_EQ(argument.find("process_manager_tests"), std::string::npos);
    }
}

#endif
