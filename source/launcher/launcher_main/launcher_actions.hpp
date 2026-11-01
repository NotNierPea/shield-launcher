#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <optional>

namespace launcher {
    std::filesystem::path launcher_dir();

    // the game dir has BlackOps4.exe, it is the launcher dir or two up from project-bo4/launcher
    std::filesystem::path game_dir();

    // launcher-config.json next to the launcher
    struct settings {
        int volume = 5;
        bool close_on_play = false;
    };

    settings load_settings();
    void save_volume(int volume);
    void save_close_on_play(bool close_on_play);

    // the game's language as battle.net sets it: LOCALE and LOCALE_AUDIO of its BO4 launch options, enUS when unset
    std::string game_language();
    bool set_game_language(const std::string& locale);

    // the languages battle.net downloaded: the "<locale> text?" tags of the active build in .build.info, unknown without it
    std::optional<std::vector<std::string>> installed_game_languages();

    // project-bo4.json, the client's own settings
    std::string client_config_path();
    std::string player_name();
    bool set_player_name(const std::string& name);

    // a leaf of project-bo4.json, numbers travel as text so int64 values stay exact
    struct config_entry {
        std::vector<std::string> path;
        std::string type;
        std::string value;
    };

    bool read_client_config(std::vector<config_entry>& entries, std::string& error);
    bool write_client_config(const std::vector<config_entry>& entries, std::string& error);
    void reset_client_config();

    // the "dedicated" section of project-bo4.json, defaults as the client's dedicated.cpp has them
    struct dedicated_settings {
        std::string name = "Dedicated";
        std::string lobby = "public";
        int playlist = 0;
        std::string rotation = "mp_urban tdm, mp_seaside tdm, mp_frenetic tdm";
        int min_players = 2;
        int lobby_seconds = 20;
        std::string draw = "ui";
        int max_fps = 60;
        bool minimize = false;
        bool streaming = false;
        bool background = true;
    };

    dedicated_settings read_dedicated_settings();
    bool write_dedicated_settings(const dedicated_settings& values, std::string& error);

    void remove_acts_plugin_once();

    enum class play_mode { client, vanilla, dedicated };

    // runs on a worker thread, progress and the result come back through the callbacks
    void play(play_mode mode, std::function<void(int, const char*)> progress, std::function<void(bool, std::string)> done);
}
