#pragma once
#include <std_include.hpp>

namespace updater
{
    std::string get_server_version();
    bool check_for_updates();
    std::string get_latest_version();

    // remembers the offered update so it is only offered once
    void mark_prompted();

    // the update script replaces the files once the launcher has closed
    bool download_and_update();

    class UpdateManager
    {
    public:
    };
}
