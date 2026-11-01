#pragma once
#ifndef _WIN32
#include <string>
#include <utility>
#include <vector>

namespace launcher {
    // forks twice so the child is never our zombie and outlives the launcher
    bool spawn_detached(const std::string& program, const std::vector<std::string>& args,
                        const std::vector<std::pair<std::string, std::string>>& env,
                        const std::string& work_dir, std::string* error = nullptr);

    // Starts the Windows executable through umu-run (Proton) or a plain wine binary,
    // using the "linux" section of launcher-config.json. Detached: the game outlives us.
    bool launch_linux_game(const std::string& exe_path, const std::string& arguments, std::string* error = nullptr);
}
#endif
