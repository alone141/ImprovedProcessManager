#include "SystemdNotifier.hpp"

#include <string_view>

namespace process_manager
{

bool SystemdNotifier::Notify(std::string_view state)
{
    static_cast<void>(state);
    return false;
}

} // namespace process_manager
