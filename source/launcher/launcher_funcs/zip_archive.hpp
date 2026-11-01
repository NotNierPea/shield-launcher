#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Minimal, dependency-free zip reader. Only what the launcher needs: read the
// central directory and inflate a single stored/deflated entry to disk.
// Replaces the "powershell Expand-Archive" shell-out used before, so the same
// code path works on Windows and Linux.
namespace zip {
    struct Entry {
        std::string name;
        uint32_t crc32 = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint16_t method = 0;
        uint32_t localHeaderOffset = 0;
    };

    bool list(const std::string& zipPath, std::vector<Entry>& out);

    bool findEntry(const std::string& zipPath, const std::string& entryName, Entry& out);

    // Extracts a single entry to destPath, overwriting it.
    bool extractEntry(const std::string& zipPath, const Entry& entry, const std::string& destPath);

    bool extractEntry(const std::string& zipPath, const std::string& entryName, const std::string& destPath);

    // Extracts every regular entry below destDir, keeping the archive layout.
    // Entries pointing outside destDir ("..", absolute paths) are rejected.
    bool extractAll(const std::string& zipPath, const std::string& destDir);
}
