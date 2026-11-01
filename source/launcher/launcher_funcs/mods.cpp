#include "std_include.hpp"
#include "mods.hpp"
#include "zip_archive.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <rapidjson/document.h>

namespace fs = std::filesystem;

namespace {
    std::string readMetadataName(const fs::path& metadataPath) {
        std::ifstream file(metadataPath);
        if (!file) return {};

        std::stringstream buffer;
        buffer << file.rdbuf();

        rapidjson::Document doc;
        doc.Parse(buffer.str().c_str());
        if (doc.HasParseError() || !doc.IsObject()) return {};

        const auto it = doc.FindMember("name");
        if (it == doc.MemberEnd() || !it->value.IsString()) return {};

        return it->value.GetString();
    }

    int countFiles(const fs::path& dir) {
        std::error_code ec;
        int count = 0;

        for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec)) ++count;
        }

        return count;
    }

    std::vector<mods::InstalledMod> scan(const fs::path& root, bool enabled) {
        std::vector<mods::InstalledMod> result;

        std::error_code ec;
        if (!fs::is_directory(root, ec)) return result;

        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (!entry.is_directory()) continue;

            const fs::path metadata = entry.path() / "metadata.json";
            if (!fs::exists(metadata)) continue;

            mods::InstalledMod mod;
            mod.folder = entry.path().filename().string();
            mod.path = entry.path().string();
            mod.enabled = enabled;
            mod.name = readMetadataName(metadata);
            if (mod.name.empty()) mod.name = mod.folder;
            mod.fileCount = countFiles(entry.path());

            result.push_back(mod);
        }

        return result;
    }

    // Finds the folder that carries the metadata.json, either at the root of
    // the archive or one level down (the usual layout).
    fs::path findModRoot(const fs::path& extracted) {
        std::error_code ec;
        if (fs::exists(extracted / "metadata.json")) {
            return extracted;
        }

        for (const auto& entry : fs::directory_iterator(extracted, ec)) {
            if (!entry.is_directory()) continue;
            if (fs::exists(entry.path() / "metadata.json")) {
                return entry.path();
            }
        }

        return {};
    }
}

namespace mods {
    std::string modsDirectory(const std::string& gameDir) {
        return (fs::path(gameDir) / "project-bo4" / "mods").string();
    }

    std::string disabledDirectory(const std::string& gameDir) {
        return (fs::path(gameDir) / "project-bo4" / "mods_disabled").string();
    }

    std::vector<CatalogEntry> catalog() {
        return {
            {"[ALL] - Atian Mod Menu", "[ALL]",
             "A really cool looking mod menu by a french man (ATE47). Note: might randomly crash in long sessions.",
             "https://github.com/ate47/t8-atian-menu/releases/download/latest_build/BlackOps4_shield_atianmenu.zip"},

            {"[ZM] - Enhancement Mod", "[ZM]",
             "Improves the zombies experience with a lot of mods and a config in a menu. By peawhatever.",
             "https://github.com/NotNierPea/shield-MM-mods/releases/download/release/EnhancementModT8.zip"},

            {"[ALL] - Black Ops 4 Menu", "[ALL]",
             "A menu made for Blackout, Multiplayer and Zombies, easy to read code for devs. By Kam.",
             "https://github.com/NotNierPea/shield-MM-mods/releases/download/release/BlackOps4Menu.zip"},

            {"[ZM] - Rush Mod", "[ZM]",
             "Chaos Mod like for zombies, a lot of ducks too. By peawhatever.",
             "https://github.com/NotNierPea/shield-MM-mods/releases/download/release/RushModT8.zip"},

            {"[ZM] - Round Timer", "[ZM]",
             "Adds a round timer to zombies. By RealD4rk9.",
             "https://github.com/NotNierPea/shield-MM-mods/releases/download/release/TimersMod.zip"},

            {"[ZM] - Survival Maps", "[ZM]",
             "Survival mod, first room only like. By GerardS0406.",
             "https://github.com/NotNierPea/shield-MM-mods/releases/download/release/survival.zip"},

            {"[ZM] - Abomination Menu", "[ZM]",
             "Mod menu made for zombies. By SirCryptic.",
             "https://github.com/NotNierPea/shield-MM-mods/releases/download/release/abdom.zip"},

            {"[MP/ZM] - Synergy Mod Menu", "[MP/ZM]",
             "Mod menu made for zombies and multiplayer. By SyndiShanX.",
             "https://github.com/SyndiShanX/Synergy-BO4-GSC-Menu/releases/download/Latest/Synergy.zip"},

            {"[ZM] - Gun Game", "[ZM]",
             "Gun Game mod for zombies. By SyndiShanX.",
             "https://github.com/SyndiShanX/Synergy-BO4-GSC-Menu/releases/download/Latest/Gun_Game.zip"},
        };
    }

    std::vector<InstalledMod> installed(const std::string& gameDir) {
        std::vector<InstalledMod> result = scan(modsDirectory(gameDir), true);

        for (auto& mod : scan(disabledDirectory(gameDir), false)) {
            result.push_back(mod);
        }

        return result;
    }

    std::vector<std::string> unmanagedFiles(const std::string& gameDir) {
        std::vector<std::string> result;

        std::error_code ec;
        const fs::path root(modsDirectory(gameDir));
        if (!fs::is_directory(root, ec)) return result;

        for (const auto& entry : fs::directory_iterator(root, ec)) {
            if (entry.is_regular_file()) {
                result.push_back(entry.path().filename().string());
            }
        }

        return result;
    }

    std::string installArchive(const std::string& gameDir,
                               const std::string& archivePath,
                               const std::string& workDir,
                               std::string* error) {
        if (error) error->clear();

        std::error_code ec;
        fs::remove_all(workDir, ec);
        fs::create_directories(workDir, ec);

        if (!zip::extractAll(archivePath, workDir)) {
            if (error) *error = "Could not unpack " + fs::path(archivePath).filename().string();
            return {};
        }

        const fs::path modRoot = findModRoot(workDir);
        if (modRoot.empty()) {
            if (error) *error = "This archive has no metadata.json, it is not a packaged mod.";
            fs::remove_all(workDir, ec);
            return {};
        }

        std::string folderName = modRoot.filename().string();
        if (modRoot == fs::path(workDir) || folderName.empty()) {
            // flat archive: name the folder after the archive itself
            folderName = fs::path(archivePath).stem().string();
        }

        const fs::path target = fs::path(modsDirectory(gameDir)) / folderName;

        fs::create_directories(fs::path(modsDirectory(gameDir)), ec);
        fs::remove_all(target, ec);

        fs::rename(modRoot, target, ec);
        if (ec) {
            // different filesystems: copy then drop the scratch copy
            ec.clear();
            fs::copy(modRoot, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                if (error) *error = "Could not copy the mod into project-bo4/mods.";
                fs::remove_all(workDir, ec);
                return {};
            }
        }

        fs::remove_all(workDir, ec);

        return target.string();
    }

    bool removeMod(const InstalledMod& mod) {
        std::error_code ec;
        fs::remove_all(mod.path, ec);
        return !ec;
    }

    bool setEnabled(const std::string& gameDir, const InstalledMod& mod, bool enabled) {
        const fs::path from = enabled ? fs::path(disabledDirectory(gameDir)) / mod.folder
                                      : fs::path(modsDirectory(gameDir)) / mod.folder;
        const fs::path to = enabled ? fs::path(modsDirectory(gameDir)) / mod.folder
                                    : fs::path(disabledDirectory(gameDir)) / mod.folder;

        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        fs::rename(from, to, ec);

        return !ec;
    }
}
