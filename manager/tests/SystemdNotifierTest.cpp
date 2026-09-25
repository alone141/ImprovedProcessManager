#include <gtest/gtest.h>

#include "SystemdNotifier.hpp"

#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <cstring>
#include <filesystem>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

TEST(SystemdNotifierTest, DoesNothingOutsideSystemd)
{
#ifdef _WIN32
    _putenv_s("NOTIFY_SOCKET", "");
#else
    unsetenv("NOTIFY_SOCKET");
#endif
    EXPECT_FALSE(process_manager::SystemdNotifier::Notify("READY=1"));
}

#ifndef _WIN32

TEST(SystemdNotifierTest, SendsTheStateToTheNotifySocket)
{
    const std::string path = (std::filesystem::temp_directory_path() / "process_manager_notify.sock").string();
    std::filesystem::remove(path);
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    ASSERT_EQ(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);

    setenv("NOTIFY_SOCKET", path.c_str(), 1);
    EXPECT_TRUE(process_manager::SystemdNotifier::Notify("READY=1"));
    char buffer[64]{};
    const ssize_t received = ::recv(fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
    EXPECT_EQ(std::string(buffer, received > 0 ? static_cast<std::size_t>(received) : 0), "READY=1");
    unsetenv("NOTIFY_SOCKET");
    ::close(fd);
    std::filesystem::remove(path);
}

#endif
