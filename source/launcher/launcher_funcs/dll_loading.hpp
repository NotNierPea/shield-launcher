#pragma once
#include <string>
#include <filesystem>

namespace DllLoading {
    enum class Result {
        Success,
        FileNotFound,
        InvalidGamePath,
        ZipError,
        CopyError
    };

    bool isGameRunning();

    Result copyClientDll(const std::string& launcherDir);

    Result extractExe(const std::string& gameDir);

    // flags start with a space, " -dedicated"
    bool launchGame(const std::string& gameExePath, const std::string& gameFlags = " -zombies");
}