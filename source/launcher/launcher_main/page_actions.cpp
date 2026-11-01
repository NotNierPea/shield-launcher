#include "std_include.hpp"
#include "page_actions.hpp"
#include "../launcher_funcs/auto_update.hpp"
#include "../launcher_funcs/download.hpp"
#include "../launcher_funcs/mods.hpp"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
    using json_writer = rapidjson::Writer<rapidjson::StringBuffer>;

    std::string string_member(const rapidjson::Value& object, const char* key) {
        if (!object.IsObject() || !object.HasMember(key) || !object[key].IsString())
            return "";
        return object[key].GetString();
    }

    bool read_entries(const rapidjson::Value& message, std::vector<launcher::config_entry>& entries) {
        if (!message.HasMember("entries") || !message["entries"].IsArray())
            return false;

        for (const auto& item : message["entries"].GetArray()) {
            if (!item.IsObject() || !item.HasMember("path") || !item["path"].IsArray())
                return false;

            launcher::config_entry entry;
            for (const auto& part : item["path"].GetArray()) {
                if (!part.IsString())
                    return false;
                entry.path.push_back(part.GetString());
            }
            entry.type = string_member(item, "type");
            entry.value = string_member(item, "value");
            entries.push_back(std::move(entry));
        }

        return true;
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

    // what the page sent over the saved values, anything missing or of the wrong type keeps its saved value
    launcher::dedicated_settings read_dedicated_values(const rapidjson::Value& page) {
        launcher::dedicated_settings values = launcher::read_dedicated_settings();

        values.name = read_string(page, "name", values.name);
        values.lobby = read_string(page, "lobby", values.lobby);
        values.playlist = read_int(page, "playlist", values.playlist);
        values.rotation = read_string(page, "rotation", values.rotation);
        values.min_players = read_int(page, "min_players", values.min_players);
        values.lobby_seconds = read_int(page, "lobby_seconds", values.lobby_seconds);
        values.draw = read_string(page, "draw", values.draw);
        values.max_fps = read_int(page, "max_fps", values.max_fps);
        values.minimize = read_bool(page, "minimize", values.minimize);
        values.streaming = read_bool(page, "streaming", values.streaming);
        values.background = read_bool(page, "background", values.background);
        return values;
    }

#ifndef _WIN32
    launcher::linux_settings read_linux_values(const rapidjson::Value& page) {
        launcher::linux_settings values = launcher::load_linux_settings();

        if (page.HasMember("runtimePath") && page["runtimePath"].IsString())
            values.runtime_path = page["runtimePath"].GetString();
        if (page.HasMember("prefixPath") && page["prefixPath"].IsString())
            values.prefix_path = page["prefixPath"].GetString();
        if (page.HasMember("backend") && page["backend"].IsString())
            values.backend = page["backend"].GetString();

        return values;
    }
#endif

    std::string current_player_name() {
        std::string name = launcher::player_name();
        return name.empty() ? "Unknown Soldier" : name;
    }
}

namespace page {
    controller::controller(launcher::settings settings, host callbacks, std::function<void(const std::string&)> send)
        : settings_(settings), host_(std::move(callbacks)), send_(std::move(send)) {
    }

    template <typename fill_type>
    void controller::post(const char* type, const fill_type& fill) {
        rapidjson::StringBuffer buffer;
        json_writer writer(buffer);
        writer.StartObject();
        writer.Key("type");
        writer.String(type);
        fill(writer);
        writer.EndObject();

        std::lock_guard<std::mutex> lock(send_mutex_);
        send_(std::string(buffer.GetString(), buffer.GetSize()));
    }

    void controller::post_error(const std::string& message) {
        post("error", [&](json_writer& writer) {
            writer.Key("message");
            writer.String(message.c_str());
        });
    }

    void controller::post_window_state(bool maximized) {
        post("window", [&](json_writer& writer) {
            writer.Key("maximized");
            writer.Bool(maximized);
        });
    }

    void controller::post_progress(int value, const std::string& text) {
        post("progress", [&](json_writer& writer) {
            writer.Key("value");
            writer.Int(value);
            writer.Key("text");
            writer.String(text.c_str());
        });
    }

    void controller::post_name() {
        const std::string name = current_player_name();
        post("name", [&](json_writer& writer) {
            writer.Key("name");
            writer.String(name.c_str());
        });
    }

    void controller::post_installed_languages() {
        // null when .build.info can't be read, the page then marks nothing as missing
        auto installed = launcher::installed_game_languages();
        post("languages", [&](json_writer& writer) {
            writer.Key("installedLanguages");
            if (!installed) {
                writer.Null();
                return;
            }

            writer.StartArray();
            for (const auto& code : *installed)
                writer.String(code.c_str());
            writer.EndArray();
        });
    }

    void controller::post_init() {
        const std::string name = current_player_name();
        const std::string version = updater::get_server_version();
        const bool game_found = !launcher::game_dir().empty();
        const std::string language = launcher::game_language();
        const std::string platform = launcher::platform();
        auto installed = launcher::installed_game_languages();

        post("init", [&](json_writer& writer) {
            writer.Key("name");
            writer.String(name.c_str());
            writer.Key("version");
            writer.String(version.c_str());
            writer.Key("gameFound");
            writer.Bool(game_found);
            writer.Key("volume");
            writer.Int(settings_.volume);
            writer.Key("closeOnPlay");
            writer.Bool(settings_.close_on_play);
            writer.Key("language");
            writer.String(language.c_str());
            writer.Key("platform");
            writer.String(platform.c_str());

            writer.Key("installedLanguages");
            if (!installed) {
                writer.Null();
            }
            else {
                writer.StartArray();
                for (const auto& code : *installed)
                    writer.String(code.c_str());
                writer.EndArray();
            }
        });
    }

    void controller::post_client_config() {
        std::vector<launcher::config_entry> entries;
        std::string error;
        if (!launcher::read_client_config(entries, error)) {
            post_error(error);
            return;
        }

        post("settings", [&](json_writer& writer) {
            writer.Key("entries");
            writer.StartArray();
            for (const auto& entry : entries) {
                writer.StartObject();
                writer.Key("path");
                writer.StartArray();
                for (const auto& part : entry.path)
                    writer.String(part.c_str());
                writer.EndArray();
                writer.Key("type");
                writer.String(entry.type.c_str());
                writer.Key("value");
                writer.String(entry.value.c_str());
                writer.EndObject();
            }
            writer.EndArray();
        });
    }

    void controller::post_dedicated_settings() {
        const launcher::dedicated_settings values = launcher::read_dedicated_settings();

        post("dedicated", [&](json_writer& writer) {
            writer.Key("values");
            writer.StartObject();
            writer.Key("name");
            writer.String(values.name.c_str());
            writer.Key("lobby");
            writer.String(values.lobby.c_str());
            writer.Key("playlist");
            writer.Int(values.playlist);
            writer.Key("rotation");
            writer.String(values.rotation.c_str());
            writer.Key("min_players");
            writer.Int(values.min_players);
            writer.Key("lobby_seconds");
            writer.Int(values.lobby_seconds);
            writer.Key("draw");
            writer.String(values.draw.c_str());
            writer.Key("max_fps");
            writer.Int(values.max_fps);
            writer.Key("minimize");
            writer.Bool(values.minimize);
            writer.Key("streaming");
            writer.Bool(values.streaming);
            writer.Key("background");
            writer.Bool(values.background);
            writer.EndObject();
        });
    }

    void controller::post_mods() {
        const std::string game = launcher::game_dir().string();
        const std::vector<mods::InstalledMod> installed = game.empty() ? std::vector<mods::InstalledMod>{} : mods::installed(game);
        const std::vector<std::string> unmanaged = game.empty() ? std::vector<std::string>{} : mods::unmanagedFiles(game);
        const std::vector<mods::CatalogEntry> catalog = mods::catalog();

        post("mods", [&](json_writer& writer) {
            writer.Key("modsDir");
            writer.String(game.empty() ? "" : mods::modsDirectory(game).c_str());

            writer.Key("installed");
            writer.StartArray();
            for (const auto& mod : installed) {
                writer.StartObject();
                writer.Key("name");
                writer.String(mod.name.c_str());
                writer.Key("folder");
                writer.String(mod.folder.c_str());
                writer.Key("enabled");
                writer.Bool(mod.enabled);
                writer.Key("files");
                writer.Int(mod.fileCount);
                writer.EndObject();
            }
            writer.EndArray();

            writer.Key("catalog");
            writer.StartArray();
            for (const auto& entry : catalog) {
                writer.StartObject();
                writer.Key("title");
                writer.String(entry.title.c_str());
                writer.Key("modes");
                writer.String(entry.modes.c_str());
                writer.Key("description");
                writer.String(entry.description.c_str());
                writer.Key("url");
                writer.String(entry.url.c_str());
                writer.EndObject();
            }
            writer.EndArray();

            writer.Key("unmanaged");
            writer.StartArray();
            for (const auto& name : unmanaged)
                writer.String(name.c_str());
            writer.EndArray();
        });
    }

#ifndef _WIN32
    void controller::post_linux() {
        const launcher::linux_settings values = launcher::load_linux_settings();
        const auto runtimes = launcher::linux_runtimes();
        const bool ready = launcher::linux_prefix_ready(values.prefix_path);

        post("linux", [&](json_writer& writer) {
            writer.Key("runtimePath");
            writer.String(values.runtime_path.c_str());
            writer.Key("prefixPath");
            writer.String(values.prefix_path.c_str());
            writer.Key("backend");
            writer.String(values.backend.c_str());
            writer.Key("prefixReady");
            writer.Bool(ready);

            writer.Key("runtimes");
            writer.StartArray();
            for (const auto& runtime : runtimes) {
                writer.StartObject();
                writer.Key("name");
                writer.String(runtime.name.c_str());
                writer.Key("path");
                writer.String(runtime.path.c_str());
                writer.Key("kind");
                writer.String(runtime.kind.c_str());
                writer.EndObject();
            }
            writer.EndArray();
        });
    }
#endif

    void controller::check_for_updates() {
        host_.run_async([this] {
            if (!updater::check_for_updates())
                return;

            updater::mark_prompted();

            const std::string latest = updater::get_latest_version();
            const std::string current = updater::get_server_version();
            post("update", [&](json_writer& writer) {
                writer.Key("latest");
                writer.String(latest.c_str());
                writer.Key("current");
                writer.String(current.c_str());
            });
        });
    }

    void controller::start_update() {
        if (updating_)
            return;

        updating_ = true;
        host_.run_async([this] {
            const bool started = updater::download_and_update();
            updating_ = false;
            if (started)
                host_.close_for_update();
            else
                post_error("The update could not be downloaded. Try again later.");
        });
    }

    void controller::start_play(launcher::play_mode mode) {
        if (playing_)
            return;

        playing_ = true;
        host_.run_async([this, mode] {
            launcher::play(mode,
                [this](int value, const char* text) {
                    post_progress(value, text);
                },
                [this](bool ok, std::string error) {
                    playing_ = false;
                    const bool closing = ok && settings_.close_on_play;

                    post("playDone", [&](json_writer& writer) {
                        writer.Key("ok");
                        writer.Bool(ok);
                        writer.Key("error");
                        writer.String(error.c_str());
                        writer.Key("closing");
                        writer.Bool(closing);
                    });

                    if (closing)
                        host_.close_window();
                });
        });
    }

    void controller::install_mod(const std::string& url, const std::string& title) {
        if (mods_busy_) {
            post_error("A mod is already being installed.");
            return;
        }

        if (url.empty()) {
            post_error("That mod has no download link.");
            return;
        }

        mods_busy_ = true;
        host_.run_async([this, url, title] {
            const fs::path temp = fs::temp_directory_path() / "shield-launcher-mods";
            std::error_code ec;
            fs::create_directories(temp, ec);

            const std::string archive = (temp / "download.zip").string();
            const std::string name = title.empty() ? "the mod" : title;

            // a mod picked from disk arrives as file://, no download then
            const bool local = url.rfind("file://", 0) == 0;
            const std::string source = local ? url.substr(7) : archive;

            if (local) {
                if (!fs::exists(source)) {
                    mods_busy_ = false;
                    post_error("That file is gone: " + source);
                    return;
                }
                post_progress(10, "Installing " + name);
            }
            else {
                post_progress(10, "Downloading " + name);
                const std::string error = webdl::download(url, archive);
                if (!error.empty()) {
                    mods_busy_ = false;
                    post_error(error);
                    return;
                }
            }

            post_progress(60, "Installing " + name);

            const std::string game = launcher::game_dir().string();
            std::string install_error;
            const std::string target = mods::installArchive(game, source, (temp / "extract").string(), &install_error);
            fs::remove(archive, ec);
            fs::remove_all(temp / "extract", ec);
            mods_busy_ = false;

            if (target.empty()) {
                post_error(install_error.empty() ? "The mod could not be installed." : install_error);
                return;
            }

            post_progress(100, name + " installed");
            post_mods();
        });
    }

    void controller::handle_mods(const std::string& action, const rapidjson::Value& message) {
        const std::string game = launcher::game_dir().string();

        if (action == "openMods" || action == "refreshMods") {
            post_mods();
        }
        else if (action == "installMod") {
            install_mod(string_member(message, "url"), string_member(message, "title"));
        }
        else if (action == "installModArchive") {
            const std::string archive = host_.pick_path("Select a mod archive", false);
            if (archive.empty())
                return;
            install_mod("file://" + archive, fs::path(archive).stem().string());
        }
        else if (action == "toggleMod" || action == "removeMod") {
            const std::string folder = string_member(message, "folder");
            if (folder.empty() || game.empty())
                return;

            const std::string active = (fs::path(mods::modsDirectory(game)) / folder).string();
            const std::string disabled = (fs::path(mods::disabledDirectory(game)) / folder).string();

            mods::InstalledMod mod;
            mod.folder = folder;
            mod.path = fs::exists(active) ? active : disabled;
            mod.enabled = fs::exists(active);

            const bool ok = action == "removeMod"
                ? mods::removeMod(mod)
                : mods::setEnabled(game, mod, message.HasMember("enabled") && message["enabled"].IsBool() && message["enabled"].GetBool());

            if (!ok)
                post_error(action == "removeMod" ? "Could not delete " + folder + ", close the game and try again."
                                                 : "Could not move " + folder + ", close the game and try again.");

            post_mods();
        }
    }

#ifndef _WIN32
    void controller::handle_linux(const std::string& action, const rapidjson::Value& message) {
        if (action == "openLinux") {
            post_linux();
        }
        else if (action == "saveLinux") {
            std::string error;
            if (!launcher::save_linux_settings(read_linux_values(message), error)) {
                post_error(error);
                return;
            }

            post_linux();
            post("linuxSaved", [](json_writer&) {});
        }
        else if (action == "pickPrefix") {
            const std::string picked = host_.pick_path("Select or create a Wine prefix folder", true);
            if (!picked.empty())
                post("pickedFolder", [&](json_writer& writer) {
                    writer.Key("value");
                    writer.String(picked.c_str());
                });
        }
        else if (action == "createPrefix") {
            const launcher::linux_settings values = read_linux_values(message);
            if (values.prefix_path.empty()) {
                post_error("Pick a prefix folder first.");
                return;
            }

            post_progress(10, "Creating the Wine prefix");
            host_.run_async([this, values] {
                std::string error;
                const bool ok = launcher::create_linux_prefix(values, error);
                post_progress(100, ok ? "Prefix ready" : "Prefix creation failed");
                if (!ok) {
                    post_error(error);
                    return;
                }
                post_linux();
            });
        }
    }
#endif

    void controller::handle(const std::string& json) {
        rapidjson::Document message;
        message.Parse(json.c_str());
        if (message.HasParseError() || !message.IsObject())
            return;

        const std::string action = string_member(message, "action");
        if (action.empty())
            return;

        if (action == "ready") {
            post_init();
            check_for_updates();
            if (host_.play_startup_sound)
                host_.play_startup_sound();
        }
        else if (action == "drag") {
            if (host_.start_drag)
                host_.start_drag();
        }
        else if (action == "resize") {
            if (host_.start_resize)
                host_.start_resize(string_member(message, "edge"));
        }
        else if (action == "minimize") {
            if (host_.minimize_window)
                host_.minimize_window();
        }
        else if (action == "close") {
            if (host_.close_window)
                host_.close_window();
        }
        else if (action == "discord") {
            host_.open_url("https://discord.gg/AXECAzJJGU");
        }
        else if (action == "docs") {
            host_.open_url("https://shield-client.gitbook.io/shield-documentation");
        }
        else if (action == "play") {
            start_play(string_member(message, "mode") == "vanilla" ? launcher::play_mode::vanilla : launcher::play_mode::client);
        }
        else if (action == "openDedicated") {
            post_dedicated_settings();
        }
        else if (action == "launchDedicated" && !playing_) {
            std::string error;
            if (!message.HasMember("values") || !message["values"].IsObject())
                post("playDone", [&](json_writer& writer) {
                    writer.Key("ok");
                    writer.Bool(false);
                    writer.Key("error");
                    writer.String("The dedicated settings could not be read.");
                    writer.Key("closing");
                    writer.Bool(false);
                });
            else if (!launcher::write_dedicated_settings(read_dedicated_values(message["values"]), error))
                post_error(error);
            else
                start_play(launcher::play_mode::dedicated);
        }
        else if (action == "setName") {
            const std::string name = string_member(message, "name");
            if (name.empty())
                post_error("Name cannot be empty!");
            else if (!launcher::set_player_name(name))
                post_error("Failed to save name!");
            else
                post_name();
        }
        else if (action == "volume" && message.HasMember("value") && message["value"].IsInt()) {
            settings_.volume = std::clamp(message["value"].GetInt(), 0, 100);
            if (host_.set_sound_volume)
                host_.set_sound_volume(settings_.volume);
            if (message.HasMember("save") && message["save"].IsTrue())
                launcher::save_volume(settings_.volume);
        }
        else if (action == "closeOnPlay" && message.HasMember("value") && message["value"].IsBool()) {
            settings_.close_on_play = message["value"].GetBool();
            launcher::save_close_on_play(settings_.close_on_play);
        }
        else if (action == "language") {
            if (!launcher::set_game_language(string_member(message, "value")))
                post_error("Failed to save the game language!");
        }
        else if (action == "languages") {
            post_installed_languages();
        }
        else if (action == "openSettings") {
            post_client_config();
        }
        else if (action == "saveSettings") {
            std::vector<launcher::config_entry> entries;
            std::string error;
            if (!read_entries(message, entries))
                post_error("The settings could not be read.");
            else if (!launcher::write_client_config(entries, error))
                post_error(error);
            else {
                post("settingsSaved", [](json_writer&) {});
                post_name();
            }
        }
        else if (action == "resetSettings") {
            launcher::reset_client_config();
            post_client_config();
            post_name();
        }
        else if (action == "update") {
            start_update();
        }
        else if (action == "openMods" || action == "refreshMods" || action == "installMod"
            || action == "installModArchive" || action == "toggleMod" || action == "removeMod") {
            handle_mods(action, message);
        }
#ifndef _WIN32
        else if (action == "openLinux" || action == "saveLinux" || action == "pickPrefix" || action == "createPrefix") {
            handle_linux(action, message);
        }
#endif
    }
}
