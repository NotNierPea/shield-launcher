#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <optional>

namespace launcher {
    // the binary itself, the updater replaces it where it runs from
    std::filesystem::path executable_path();

    std::filesystem::path launcher_dir();

    // the game dir has BlackOps4.exe, it is the launcher dir or two up from project-bo4/launcher
    std::filesystem::path game_dir();

    // "windows" or "linux", what the page shows and hides
    std::string platform();

    // launcher-config.json next to the launcher
    struct settings {
        int volume = 5;
        bool close_on_play = false;
    };

    settings load_settings();
    void save_volume(int volume);
    void save_close_on_play(bool close_on_play);

    // the game's language as battle.net sets it: LOCALE and LOCALE_AUDIO of its BO4 launch options, enUS when unset.
    // Windows only: under Linux there is no battle.net registry to read, so it is empty and the page hides the picker
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

    // Linux only, the "linux" section of launcher-config.json: the Proton/Wine runtime and prefix the game runs in
    struct linux_runtime {
        std::string name;   // display name, e.g. "GE-Proton11-9"
        std::string path;   // directory holding the runtime
        std::string kind;   // "proton" or "wine"
    };

    struct linux_settings {
        std::string runtime_path;  // the selected linux_runtime::path
        std::string prefix_path;   // WINEPREFIX
        std::string backend;       // "umu" or "wine"
    };

    // every runtime found in Steam, compatibilitytools.d and Heroic
    std::vector<linux_runtime> linux_runtimes();
    std::string detect_linux_prefix();
    linux_settings load_linux_settings();
    bool save_linux_settings(const linux_settings& values, std::string& error);
    bool linux_prefix_ready(const std::string& dir);
    bool create_linux_prefix(const linux_settings& values, std::string& error);
    bool linux_game_running();

    enum class play_mode { client, vanilla, dedicated };

    // runs on a worker thread, progress and the result come back through the callbacks
    void play(play_mode mode, std::function<void(int, const char*)> progress, std::function<void(bool, std::string)> done);
}
