#include "std_include.hpp"
#include "dll_loading.hpp"
#include "zip_archive.hpp"
#include "linux_run.hpp"
#include "../launcher_main/launcher_actions.hpp"

#ifdef _WIN32
#include <TlHelp32.h>
#endif

#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdint>
#include <zlib.h>
#include <thread>
#include <atomic>

namespace fs = std::filesystem;

namespace {
    // CRC-32 for checking dlls and exe
    bool computeFileCrc32(const fs::path& filePath, uint32_t& outCrc) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file) {
            return false;
        }

        uLong crc = crc32(0L, Z_NULL, 0);
        std::vector<char> buffer(1 << 16);
        while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
            crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer.data()), static_cast<uInt>(file.gcount()));
        }

        outCrc = static_cast<uint32_t>(crc);
        return true;
    }

    // the game folder, whether the launcher sits at its root or in project-bo4/launcher
    fs::path resolveGameDir(const fs::path& start) {
        if (start.empty()) {
            return {};
        }

        if (fs::exists(start / "BlackOps4.exe")) {
            return start;
        }

        const fs::path parent = start.parent_path().parent_path();
        if (fs::exists(parent / "BlackOps4.exe")) {
            return parent;
        }

        return {};
    }

    // the released launcher folder, wherever the launcher itself lives
    fs::path launcherResourcesDir(const fs::path& gamePath, const fs::path& launcherPath) {
        std::error_code ec;
        if (fs::exists(launcherPath / "exe.zip", ec)) {
            return launcherPath;
        }

        const fs::path nested = gamePath / "project-bo4" / "launcher";
        if (fs::exists(nested / "exe.zip", ec)) {
            return nested;
        }

        return launcherPath;
    }
}

namespace DllLoading {
    std::thread cleanupThread;
    std::atomic<bool> cleanupThreadRunning(false);
    std::string gameDirectoryForCleanup;
#ifdef _WIN32
    DWORD gameProcessId = 0;
#endif

    bool isGameRunning() {
#ifdef _WIN32
        if (gameProcessId != 0) {
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, gameProcessId);
            if (hProcess != NULL) {
                DWORD exitCode;
                if (GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE) {
                    CloseHandle(hProcess);
                    return true;
                }
                CloseHandle(hProcess);
                return false;
            }
        }

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;

        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(processEntry);

        if (!Process32FirstW(snapshot, &processEntry)) {
            CloseHandle(snapshot);
            return false;
        }

        bool found = false;
        do {
            if (_wcsicmp(processEntry.szExeFile, L"BlackOps4.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &processEntry));

        CloseHandle(snapshot);
        return found;
#else
        return launcher::linux_game_running();
#endif
    }

    Result copyClientDll(const std::string& launcherDir) {
        if (launcherDir.empty()) {
            return Result::InvalidGamePath;
        }

        const fs::path launcherPath = fs::path(launcherDir);
        const fs::path gamePath = resolveGameDir(launcherPath);
        if (gamePath.empty()) {
            std::cout << "Game exe not found next to the launcher nor two folders up" << std::endl;
            return Result::InvalidGamePath;
        }

        const fs::path dllPath = gamePath / "XInput9_1_0.dll";
        const fs::path sourceDll = launcherPath / "XInput9_1_0.dll";

        // the released zips carry the online dll, so a launcher folder without the
        // plain file still has one
        fs::path zipDll;
        if (!fs::exists(sourceDll)) {
            for (const char* zipName : { "mp.zip", "solo.zip" }) {
                const fs::path candidate = launcherResourcesDir(gamePath, launcherPath) / zipName;
                if (fs::exists(candidate)) {
                    zipDll = candidate;
                    break;
                }
            }

            // an install that already has its dll (the client package dropped it there) needs nothing
            if (zipDll.empty()) {
                std::error_code exists_ec;
                return fs::exists(dllPath, exists_ec) ? Result::Success : Result::FileNotFound;
            }
        }

        // delete the old support file, two zone files give ui errors
        std::error_code ec;
        const fs::path supportFF = gamePath / "zone" / "support.ff";
        if (fs::exists(supportFF, ec)) {
            fs::remove(supportFF, ec);
        }

        if (!zipDll.empty()) {
            if (!zip::extractEntry(zipDll.string(), "XInput9_1_0.dll", dllPath.string())) {
                return Result::ZipError;
            }

            std::cout << "Extracted XInput9_1_0.dll from " << zipDll.filename().string() << std::endl;
            return Result::Success;
        }

        // same file skips the copy, a running game keeps the dll locked
        uint32_t sourceDllCrc = 0;
        uint32_t existingDllCrc = 0;
        if (fs::exists(dllPath) &&
            computeFileCrc32(sourceDll, sourceDllCrc) &&
            computeFileCrc32(dllPath, existingDllCrc) &&
            sourceDllCrc == existingDllCrc) {
            std::cout << "XInput9_1_0.dll already up to date, skipping copy" << std::endl;
            return Result::Success;
        }

        fs::copy_file(sourceDll, dllPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cout << "Failed to copy XInput9_1_0.dll: " << ec.message() << std::endl;
            return Result::CopyError;
        }

        std::cout << "Copied XInput9_1_0.dll to: " << dllPath.string() << std::endl;
        return Result::Success;
    }

    Result extractExe(const std::string& gameDir) {
        if (gameDir.empty()) {
            return Result::InvalidGamePath;
        }

        const fs::path gamePath = resolveGameDir(fs::path(gameDir));
        if (gamePath.empty()) {
            std::cout << "Game exe not found next to the launcher nor two folders up" << std::endl;
            return Result::InvalidGamePath;
        }

        const fs::path launcherPath = launcherResourcesDir(gamePath, fs::path(gameDir));
        const fs::path zipPath = launcherPath / "exe.zip";
        const fs::path gameExePath = gamePath / "BlackOps4.exe";

        // exe.zip only refreshes the game exe: an install that has one already is playable
        if (!fs::exists(zipPath)) {
            if (fs::exists(gameExePath)) {
                std::cout << "No " << zipPath.string() << ", keeping the exe already in place" << std::endl;
                return Result::Success;
            }

            std::cout << "Looking for zip at: " << zipPath.string() << " (missing)" << std::endl;
            return Result::FileNotFound;
        }

        // the zip's own crc tells whether the exe on disk is already that one
        zip::Entry entry;
        uint32_t existingExeCrc = 0;
        if (zip::findEntry(zipPath.string(), "BlackOps4.exe", entry) &&
            computeFileCrc32(gameExePath, existingExeCrc) &&
            entry.crc32 == existingExeCrc) {
            std::cout << "BlackOps4.exe already up to date, skipping extraction" << std::endl;
            return Result::Success;
        }

        // in-process inflate: the powershell Expand-Archive call is Windows-only, and silent when it is absent
        if (!zip::extractEntry(zipPath.string(), "BlackOps4.exe", gameExePath.string())) {
            std::cout << "Failed to extract BlackOps4.exe from " << zipPath.string() << std::endl;
            return Result::ZipError;
        }

        if (!fs::exists(gameExePath)) {
            return Result::FileNotFound;
        }

        std::cout << "Extracted BlackOps4.exe to " << gameExePath.string() << std::endl;
        return Result::Success;
    }

    void cleanupDllsAfterGame() {
        cleanupThreadRunning = true;
        std::cout << "Starting DLL cleanup monitor thread..." << std::endl;

        while (isGameRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }

        std::this_thread::sleep_for(std::chrono::seconds(3));

        if (gameDirectoryForCleanup.empty()) {
            cleanupThreadRunning = false;
            return;
        }

        const fs::path dllPath = fs::path(gameDirectoryForCleanup) / "XInput9_1_0.dll";

        std::error_code ec;
        if (fs::exists(dllPath, ec)) {
            for (int i = 0; i < 5; i++) {
                if (fs::remove(dllPath, ec)) {
                    std::cout << "Successfully deleted DLL" << std::endl;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        cleanupThreadRunning = false;
    }

    bool launchGame(const std::string& gameExePath, const std::string& gameFlags) {
        std::cout << "Launching game: " << gameExePath << gameFlags << std::endl;

        const fs::path gameDir = fs::path(gameExePath).parent_path();
        gameDirectoryForCleanup = gameDir.string();

        bool launched = false;
#ifdef _WIN32
        SHELLEXECUTEINFOA shExInfo = { 0 };
        shExInfo.cbSize = sizeof(SHELLEXECUTEINFOA);
        shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
        shExInfo.hwnd = NULL;
        shExInfo.lpVerb = "open";
        shExInfo.lpFile = gameExePath.c_str();
        shExInfo.lpParameters = gameFlags.c_str();
        shExInfo.lpDirectory = gameDirectoryForCleanup.c_str();
        shExInfo.nShow = SW_SHOW;

        if (!ShellExecuteExA(&shExInfo)) {
            std::cout << "ShellExecuteEx failed with error: " << GetLastError() << std::endl;
            return false;
        }

        if (shExInfo.hProcess) {
            CloseHandle(shExInfo.hProcess);
        }

        launched = true;
#else
        std::string error;
        launched = launcher::launch_linux_game(gameExePath, gameFlags, &error);
        if (!launched) {
            std::cout << "Launch failed: " << error << std::endl;
        }
#endif

        if (!launched) {
            return false;
        }

        if (cleanupThreadRunning && cleanupThread.joinable()) {
            cleanupThread.join();
        }

        cleanupThread = std::thread(cleanupDllsAfterGame);
        cleanupThread.detach();

        std::cout << "Game launched successfully!" << std::endl;
        return true;
    }
}
