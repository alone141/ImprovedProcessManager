#include <gtest/gtest.h>

#include "SignalWatcher.hpp"

#include <csignal>

TEST(SignalWatcherTest, NothingIsPendingAtFirst)
{
    process_manager::SignalWatcher::Install();
    EXPECT_FALSE(process_manager::SignalWatcher::TakeReloadRequest());
}

#ifndef _WIN32

TEST(SignalWatcherTest, TurnsSignalsIntoRequests)
{
    process_manager::SignalWatcher::Install();
    std::raise(SIGHUP);
    EXPECT_TRUE(process_manager::SignalWatcher::TakeReloadRequest());
    EXPECT_FALSE(process_manager::SignalWatcher::TakeReloadRequest());
    std::raise(SIGTERM);
    EXPECT_TRUE(process_manager::SignalWatcher::TakeStopRequest());
    EXPECT_FALSE(process_manager::SignalWatcher::TakeStopRequest());
}

TEST(SignalWatcherTest, UndoesSignalStateLeftByTheParent)
{
    // Ignored, SIGCHLD makes the kernel collect services before the manager reads their status.
    std::signal(SIGCHLD, SIG_IGN);
    sigset_t blocked{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, nullptr);

    process_manager::SignalWatcher::Install();

    struct sigaction child{};
    sigaction(SIGCHLD, nullptr, &child);
    EXPECT_TRUE(child.sa_handler == SIG_DFL);
    sigset_t mask{};
    sigprocmask(SIG_BLOCK, nullptr, &mask);
    EXPECT_EQ(sigismember(&mask, SIGTERM), 0);
}

TEST(SignalWatcherTest, IgnoresBrokenPipes)
{
    process_manager::SignalWatcher::Install();
    std::raise(SIGPIPE);
    EXPECT_FALSE(process_manager::SignalWatcher::TakeStopRequest());
}

#endif
