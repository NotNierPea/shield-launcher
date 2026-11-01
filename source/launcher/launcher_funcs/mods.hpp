#pragma once
#include <string>
#include <vector>

// Mod handling for the Shield client.
//
// The game only loads user mods that are packaged as a folder inside
// project-bo4/mods containing a metadata.json (see the released mods). Files
// dropped directly in mods/ are ignored, which is why a bare .gscc never shows
// up in the in-game mod manager.
namespace mods {
    struct CatalogEntry {
        std::string title;        // "[ZM] - Round Timer"
        std::string modes;        // "[ZM]", "[ALL]", "[MP/ZM]"
        std::string description;
        std::string url;
    };

    struct InstalledMod {
        std::string name;         // metadata.json "name", falls back to the folder
        std::string folder;       // folder name inside mods/
        std::string path;         // absolute path
        bool enabled = true;
        int fileCount = 0;
    };

    std::string modsDirectory(const std::string& gameDir);
    std::string disabledDirectory(const std::string& gameDir);

    // The same curated list the in-game mod manager offers.
    std::vector<CatalogEntry> catalog();

    std::vector<InstalledMod> installed(const std::string& gameDir);

    // Files sitting directly in mods/: the client never loads those.
    std::vector<std::string> unmanagedFiles(const std::string& gameDir);

    // Unpacks an archive into mods/<folder of the archive>. workDir must be a
    // writable scratch directory. Returns the installed folder, empty on error.
    std::string installArchive(const std::string& gameDir,
                               const std::string& archivePath,
                               const std::string& workDir,
                               std::string* error = nullptr);

    bool removeMod(const InstalledMod& mod);
    bool setEnabled(const std::string& gameDir, const InstalledMod& mod, bool enabled);
}
