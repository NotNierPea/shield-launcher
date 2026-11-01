#include "std_include.hpp"
#include "launcher_actions.hpp"
#include "utils.hpp"
#include "../launcher_funcs/dll_loading.hpp"
#include "../launcher_funcs/json_utils.hpp"
#include "../launcher_funcs/linux_run.hpp"
#include <algorithm>
#include <charconv>

namespace fs = std::filesystem;

namespace launcher {
    namespace {
        fs::path launcher_config_path() {
            return launcher_dir() / "launcher-config.json";
        }

        bool read_json(const fs::path& path, rapidjson::Document& doc) {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;

            std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            doc.Parse(json.c_str());
            return !doc.HasParseError() && doc.IsObject();
        }

        bool write_json(const fs::path& path, const rapidjson::Document& doc) {
            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;

            file.write(buffer.GetString(), buffer.GetSize());
            return file.good();
        }

        // sets section.key in launcher-config.json, keeping everything else in it
        template <typename T>
        void save_launcher_value(const char* section, const char* key, T value) {
            rapidjson::Document doc;
            if (!read_json(launcher_config_path(), doc))
                doc.SetObject();

            auto& allocator = doc.GetAllocator();
            if (!doc.HasMember(section) || !doc[section].IsObject()) {
                doc.RemoveMember(section);
                doc.AddMember(rapidjson::Value(section, allocator), rapidjson::Value(rapidjson::kObjectType), allocator);
            }

            auto& object = doc[section];
            if (object.HasMember(key))
                object[key] = value;
            else
                object.AddMember(rapidjson::Value(key, allocator), rapidjson::Value(value), allocator);

            write_json(launcher_config_path(), doc);
        }

        void flatten(const rapidjson::Value& object, std::vector<std::string>& path, std::vector<config_entry>& entries) {
            for (auto it = object.MemberBegin(); it != object.MemberEnd(); ++it) {
                path.push_back(it->name.GetString());
                const auto& value = it->value;

                if (value.IsObject())
                    flatten(value, path, entries);
                else if (value.IsBool())
                    entries.push_back({ path, "bool", value.GetBool() ? "true" : "false" });
                else if (value.IsInt64())
                    entries.push_back({ path, "int", std::to_string(value.GetInt64()) });
                else if (value.IsUint64())
                    entries.push_back({ path, "int", std::to_string(value.GetUint64()) });
                else if (value.IsDouble())
                    entries.push_back({ path, "double", std::to_string(value.GetDouble()) });
                else if (value.IsString())
                    entries.push_back({ path, "string", value.GetString() });

                path.pop_back();
            }
        }

        rapidjson::Value* find_entry(rapidjson::Document& doc, const std::vector<std::string>& path) {
            rapidjson::Value* current = &doc;
            for (const auto& part : path) {
                if (!current->IsObject() || !current->HasMember(part.c_str()))
                    return nullptr;
                current = &(*current)[part.c_str()];
            }
            return current;
        }

        bool set_entry(rapidjson::Value& target, const config_entry& entry, rapidjson::Document::AllocatorType& allocator) {
            const std::string& text = entry.value;

            if (entry.type == "bool") {
                target.SetBool(text == "true");
            }
            else if (entry.type == "int") {
                const char* end = text.data() + text.size();
                if (!text.empty() && text[0] == '-') {
                    int64_t number = 0;
                    auto result = std::from_chars(text.data(), end, number);
                    if (result.ec != std::errc() || result.ptr != end)
                        return false;
                    target.SetInt64(number);
                }
                else {
                    uint64_t number = 0;
                    auto result = std::from_chars(text.data(), end, number);
                    if (result.ec != std::errc() || result.ptr != end)
                        return false;

                    if (number <= static_cast<uint64_t>(INT64_MAX))
                        target.SetInt64(static_cast<int64_t>(number));
                    else
                        target.SetUint64(number);
                }
            }
            else if (entry.type == "double") {
                try {
                    size_t used = 0;
                    double number = std::stod(text, &used);
                    if (used != text.size())
                        return false;
                    target.SetDouble(number);
                }
                catch (const std::exception&) {
                    return false;
                }
            }
            else if (entry.type == "string") {
                target.SetString(text.c_str(), static_cast<rapidjson::SizeType>(text.size()), allocator);
            }

            return true;
        }

        std::string join_path(const std::vector<std::string>& path) {
            std::string joined;
            for (const auto& part : path)
                joined += (joined.empty() ? "" : ".") + part;
            return joined;
        }

        std::string read_string(const rapidjson::Value& object, const char* key, const std::string& fallback) {
            return object.HasMember(key) && object[key].IsString() ? object[key].GetString() : fallback;
        }

        int read_int(const rapidjson::Value& object, const char* key, int fallback) {
            return object.HasMember(key) && object[key].IsInt() ? object[key].GetInt() : fallback;
        }

        bool read_bool(const rapidjson::Value& object, const char* key, bool fallback) {
            return object.HasMember(key) && object[key].IsBool() ? object[key].GetBool() : fallback;
        }

        void write_member(rapidjson::Value& object, const char* key, rapidjson::Value value, rapidjson::Document::AllocatorType& allocator) {
            if (object.HasMember(key))
                object[key] = value;
            else
                object.AddMember(rapidjson::Value(key, allocator), value, allocator);
        }

        std::string extraction_error(DllLoading::Result result, const std::string& missing_file) {
            switch (result) {
            case DllLoading::Result::FileNotFound:
                return missing_file;
            case DllLoading::Result::InvalidGamePath:
                return "BlackOps4.exe not found! Make sure the launcher shortcut is in the game directory.";
            case DllLoading::Result::CopyError:
                return "Failed to copy XInput9_1_0.dll to the game directory! Close any running game and try again.";
            case DllLoading::Result::ZipError:
                return "Failed to extract files!";
            default:
                return "Unknown error occurred while preparing files.";
            }
        }
    }

    fs::path executable_path() {
#ifdef _WIN32
        wchar_t buffer[MAX_PATH];
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return fs::path(buffer);
#else
        // /proc/self/exe survives symlinks and renames
        std::error_code ec;
        const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
        return exe.empty() ? fs::current_path() / "Shield_Launcher" : exe;
#endif
    }

    fs::path launcher_dir() {
        const fs::path exe_dir = executable_path().parent_path();

        // the launcher sits either next to its files or at the game root, with
        // project-bo4/launcher holding images, LPC, Sounds and the zips
        auto has_resources = [](const fs::path& dir) {
            std::error_code ec;
            for (const char* marker : { "exe.zip", "solo.zip", "mp.zip", "images" }) {
                if (fs::exists(dir / marker, ec))
                    return true;
            }
            return false;
        };

        if (has_resources(exe_dir))
            return exe_dir;

        const fs::path nested = exe_dir / "project-bo4" / "launcher";
        if (has_resources(nested))
            return nested;

        return exe_dir;
    }

    std::string platform() {
#ifdef _WIN32
        return "windows";
#else
        return "linux";
#endif
    }

    fs::path game_dir() {
        fs::path dir = launcher_dir();
        if (fs::exists(dir / "BlackOps4.exe"))
            return dir;

        fs::path up = dir.parent_path().parent_path();
        return fs::exists(up / "BlackOps4.exe") ? up : fs::path();
    }

    settings load_settings() {
        settings result;

        rapidjson::Document doc;
        if (!read_json(launcher_config_path(), doc))
            return result;

        if (doc.HasMember("audio") && doc["audio"].IsObject() && doc["audio"].HasMember("volume") && doc["audio"]["volume"].IsInt())
            result.volume = std::clamp(doc["audio"]["volume"].GetInt(), 0, 100);

        if (doc.HasMember("launcher") && doc["launcher"].IsObject() && doc["launcher"].HasMember("closeLauncherOnPlay") && doc["launcher"]["closeLauncherOnPlay"].IsBool())
            result.close_on_play = doc["launcher"]["closeLauncherOnPlay"].GetBool();

        return result;
    }

    void save_volume(int volume) {
        save_launcher_value("audio", "volume", std::clamp(volume, 0, 100));
    }

    void save_close_on_play(bool close_on_play) {
        save_launcher_value("launcher", "closeLauncherOnPlay", close_on_play);
    }

    // battle.net's launch options of BO4, the game reads its language from here
#ifdef _WIN32
    constexpr const wchar_t* launch_options_key = L"Software\\Blizzard Entertainment\\Battle.net\\Launch Options\\VIPR";

    // the languages battle.net offers for BO4
    constexpr const char* game_languages[] = { "enUS", "frFR", "deDE", "itIT", "esES", "jaJP", "ruRU", "koKR", "ptBR", "esMX", "zhTW", "plPL" };
#endif

#ifdef _WIN32
    std::string game_language() {
        wchar_t value[16] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, launch_options_key, L"LOCALE", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
            return "enUS";

        std::string locale(value, value + wcslen(value));
        bool known = std::any_of(std::begin(game_languages), std::end(game_languages), [&](const char* code) { return locale == code; });
        return known ? locale : "enUS";
    }

    bool set_game_language(const std::string& locale) {
        if (std::none_of(std::begin(game_languages), std::end(game_languages), [&](const char* code) { return locale == code; }))
            return false;

        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, launch_options_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;

        // text and speech both, as battle.net writes them when its language is changed
        std::wstring value(locale.begin(), locale.end());
        const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        bool ok = RegSetValueExW(key, L"LOCALE", 0,
                       REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS
            && RegSetValueExW(key, L"LOCALE_AUDIO", 0,
                       REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes) == ERROR_SUCCESS;

        RegCloseKey(key);
        return ok;
    }
#endif

    std::optional<std::vector<std::string>> installed_game_languages() {
        const fs::path game = game_dir();
        std::ifstream file(game / ".build.info");
        if (game.empty() || !file)
            return std::nullopt;

        // a header of "Name!TYPE:size" columns split by |, then a row per build
        auto split = [](const std::string& line) {
            std::vector<std::string> parts;
            std::stringstream stream(line);
            for (std::string part; std::getline(stream, part, '|');)
                parts.push_back(part);
            return parts;
        };

        std::string line;
        if (!std::getline(file, line))
            return std::nullopt;

        const auto header = split(line);
        auto column = [&](const char* name) {
            for (size_t i = 0; i < header.size(); ++i) {
                if (header[i].rfind(name, 0) == 0 && header[i].size() > strlen(name) && header[i][strlen(name)] == '!')
                    return static_cast<int>(i);
            }
            return -1;
        };

        const int active = column("Active");
        const int tags = column("Tags");
        if (tags < 0)
            return std::nullopt;

        // the tags read like "EU? acct-SAU? geoip-US? enUS speech?:... itIT text?"
        static const std::regex text_tag(R"(([a-z]{2}[A-Z]{2}) text\?)");
        while (std::getline(file, line)) {
            const auto row = split(line);
            if (static_cast<int>(row.size()) <= tags || (active >= 0 && static_cast<int>(row.size()) > active && row[active] != "1"))
                continue;

            std::vector<std::string> languages;
            for (std::sregex_iterator it(row[tags].begin(), row[tags].end(), text_tag), end; it != end; ++it)
                languages.push_back((*it)[1].str());
            return languages;
        }

        return std::nullopt;
    }

    std::string client_config_path() {
        fs::path dir = game_dir();
        if (dir.empty())
            dir = launcher_dir().parent_path().parent_path();
        return (dir / "project-bo4.json").string();
    }

    std::string player_name() {
        // getJsonItem would create the file, the client makes its own on first start
        if (!fs::exists(client_config_path()))
            return "";
        return JsonUtils::getJsonItem(client_config_path(), "identity", "name");
    }

    bool set_player_name(const std::string& name) {
        return JsonUtils::replaceJsonValue(client_config_path(), name, "identity", "name");
    }

    bool read_client_config(std::vector<config_entry>& entries, std::string& error) {
        fs::path path = client_config_path();
        if (!fs::exists(path))
            JsonUtils::createDefaultJson(path.string());

        rapidjson::Document doc;
        if (!read_json(path, doc)) {
            error = "Invalid JSON format in project-bo4.json.";
            return false;
        }

        std::vector<std::string> current;
        flatten(doc, current, entries);
        return true;
    }

    bool write_client_config(const std::vector<config_entry>& entries, std::string& error) {
        fs::path path = client_config_path();

        rapidjson::Document doc;
        if (!read_json(path, doc)) {
            error = "Invalid JSON format in project-bo4.json.";
            return false;
        }

        // only known leaves change, arrays and anything else in the file stay as they are
        for (const auto& entry : entries) {
            rapidjson::Value* target = find_entry(doc, entry.path);
            if (!target || target->IsObject() || target->IsArray())
                continue;

            if (!set_entry(*target, entry, doc.GetAllocator())) {
                error = join_path(entry.path) + (entry.type == "int" ? " must be a whole number." : " must be a number.");
                return false;
            }
        }

        if (!write_json(path, doc)) {
            error = "Failed to save project-bo4.json.";
            return false;
        }

        return true;
    }

    void reset_client_config() {
        JsonUtils::createDefaultJson(client_config_path());
    }

    dedicated_settings read_dedicated_settings() {
        dedicated_settings values;

        rapidjson::Document doc;
        if (!read_json(client_config_path(), doc) || !doc.HasMember("dedicated") || !doc["dedicated"].IsObject())
            return values;

        const auto& section = doc["dedicated"];
        values.name = read_string(section, "name", values.name);
        values.lobby = read_string(section, "lobby", values.lobby);
        values.playlist = read_int(section, "playlist", values.playlist);
        values.rotation = read_string(section, "rotation", values.rotation);
        values.min_players = read_int(section, "min_players", values.min_players);
        values.lobby_seconds = read_int(section, "lobby_seconds", values.lobby_seconds);
        values.draw = read_string(section, "draw", values.draw);
        values.max_fps = read_int(section, "max_fps", values.max_fps);
        values.minimize = read_bool(section, "minimize", values.minimize);
        values.streaming = read_bool(section, "streaming", values.streaming);
        values.background = read_bool(section, "background", values.background);
        return values;
    }

    bool write_dedicated_settings(const dedicated_settings& values, std::string& error) {
        // the same checks the client makes, so it doesn't stay off over a bad value
        if (values.lobby != "public" && values.lobby != "custom")
            error = "The lobby must be public or custom.";
        else if (values.lobby == "custom" && values.rotation.find_first_not_of(" ,") == std::string::npos)
            error = "A custom lobby needs at least one map in its rotation.";
        else if (values.draw != "none" && values.draw != "ui" && values.draw != "all")
            error = "Draw must be none, ui or all.";
        else if (values.playlist < 0 || values.min_players < 1 || values.lobby_seconds < 0 || values.max_fps < 0)
            error = "Playlist, countdown and max FPS can't be negative, and at least 1 player is needed.";
        if (!error.empty())
            return false;

        // other sections and dedicated keys the launcher doesn't show, like xuid, stay as they are
        fs::path path = client_config_path();
        rapidjson::Document doc;
        if (fs::exists(path) && !read_json(path, doc)) {
            error = "Invalid JSON format in project-bo4.json.";
            return false;
        }
        if (!doc.IsObject())
            doc.SetObject();

        auto& allocator = doc.GetAllocator();
        if (!doc.HasMember("dedicated") || !doc["dedicated"].IsObject()) {
            doc.RemoveMember("dedicated");
            doc.AddMember("dedicated", rapidjson::Value(rapidjson::kObjectType), allocator);
        }

        auto& section = doc["dedicated"];
        write_member(section, "name", rapidjson::Value(values.name.c_str(), allocator), allocator);
        write_member(section, "lobby", rapidjson::Value(values.lobby.c_str(), allocator), allocator);
        write_member(section, "playlist", rapidjson::Value(values.playlist), allocator);
        write_member(section, "rotation", rapidjson::Value(values.rotation.c_str(), allocator), allocator);
        write_member(section, "min_players", rapidjson::Value(values.min_players), allocator);
        write_member(section, "lobby_seconds", rapidjson::Value(values.lobby_seconds), allocator);
        write_member(section, "draw", rapidjson::Value(values.draw.c_str(), allocator), allocator);
        write_member(section, "max_fps", rapidjson::Value(values.max_fps), allocator);
        write_member(section, "minimize", rapidjson::Value(values.minimize), allocator);
        write_member(section, "streaming", rapidjson::Value(values.streaming), allocator);
        write_member(section, "background", rapidjson::Value(values.background), allocator);

        if (!write_json(path, doc)) {
            error = "Failed to save project-bo4.json.";
            return false;
        }

        return true;
    }

    void remove_acts_plugin_once() {
        try {
            // done once, the marker keeps it from ever running again
            fs::path marker = launcher_dir() / "acts_plugin_removed";
            if (fs::exists(marker))
                return;

            fs::path game = game_dir();
            if (game.empty())
                return;

            bool removed = true;
            fs::path plugins = game / "project-bo4" / "plugins";
            if (fs::exists(plugins)) {
                for (const auto& entry : fs::directory_iterator(plugins)) {
#ifdef _WIN32
                    if (_wcsicmp(entry.path().stem().c_str(), L"acts-shield-plugin") != 0)
                        continue;
#else
                    std::string stem = entry.path().stem().string();
                    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (stem != "acts-shield-plugin")
                        continue;
#endif

                    std::error_code ec;
                    fs::remove(entry.path(), ec);
                    if (ec)
                        removed = false;
                }
            }

            // a locked plugin (game running) is tried again on the next start
            if (removed)
                std::ofstream(marker).close();
        }
        catch (const std::exception&) {
        }
    }

    void play(play_mode mode, std::function<void(int, const char*)> progress, std::function<void(bool, std::string)> done) {
        bool vanilla = mode == play_mode::vanilla;
        try {
            progress(15, "Copying LPC files");

            // the launcher sits either at the game root or in project-bo4/launcher
            fs::path current = launcher_dir();
            fs::path game = game_dir();
            if (game.empty())
                game = current.parent_path().parent_path();

            fs::path lpc = current / "LPC";
            if (fs::exists(lpc))
                utils::copyDirectoryRecursive(lpc.string(), (game / "LPC").string());

            progress(25, vanilla ? "Starting vanilla" : "Copying the client");

            if (!vanilla) {
                auto dll_result = DllLoading::copyClientDll(current.string());
                if (dll_result != DllLoading::Result::Success) {
                    done(false, extraction_error(dll_result, "XInput9_1_0.dll not found! Make sure it is in the launcher directory."));
                    return;
                }

                progress(50, "Preparing BlackOps4.exe");

                auto exe_result = DllLoading::extractExe(current.string());
                if (exe_result != DllLoading::Result::Success) {
                    done(false, extraction_error(exe_result, "Required files not found! Make sure exe.zip exists in the launcher directory."));
                    return;
                }
            }

            progress(75, "Starting the game");

            // bnet's launcher for vanilla
            const char* exe_name = vanilla ? "Black Ops 4 Launcher.exe" : "BlackOps4.exe";

            fs::path exe_path;
            if (fs::exists(current / exe_name))
                exe_path = current / exe_name;
            else if (fs::exists(game / exe_name))
                exe_path = game / exe_name;
            else {
                done(false, std::string("Could not locate ") + exe_name + "!");
                return;
            }

            // delete our dll from the game dir if it exists, the launcher's copy stays
            if (vanilla) {
                std::error_code ec;
                fs::remove(exe_path.parent_path() / "XInput9_1_0.dll", ec);
            }

            // a dedicated server is the client started with -dedicated, it reads its setup from project-bo4.json
            bool launched = mode == play_mode::dedicated
                ? DllLoading::launchGame(exe_path.string(), " -dedicated")
                : DllLoading::launchGame(exe_path.string());

            if (!launched) {
                done(false, "Failed to start the game!");
                return;
            }

            progress(100, "Game started");
            done(true, "");
        }
        catch (const std::exception& e) {
            done(false, std::string("Failed to start the game: ") + e.what());
        }
    }
}
