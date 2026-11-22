#pragma once
#include <std_include.hpp>

// game files (client, exe, support mod, maps) from manifest.json on the content release, only changed parts are downloaded
namespace content
{
    struct part {
        uint64_t size = 0;
        std::string sha256;
    };

    // a file of a package, as the folder should have it
    struct package_file {
        std::string path;
        uint64_t size = 0;
        uint32_t crc32 = 0;
    };

    struct file_entry {
        std::string path; // relative to the game folder, forward slashes
        std::string component;
        uint64_t size = 0;
        std::string sha256;
        std::vector<part> parts;
        std::string extract; // a package: the zip is unzipped into this folder, replacing it
        std::vector<package_file> entries; // what the folder holds once unzipped, checked instead of the zip
    };

    struct component {
        std::string id;
        std::string name;
        std::string description;
        bool required = true;
    };

    struct manifest {
        int schema = 0;
        std::string version;
        std::string base_url; // a part is base_url + sha256 + ".bin"
        uint64_t part_size = 0;
        std::string launcher_version;
        std::optional<file_entry> launcher_file;
        std::vector<component> components;
        std::vector<file_entry> files;
        std::vector<std::string> remove;
    };

    enum class file_state { installed, missing, outdated };

    struct file_status {
        size_t index = 0; // into manifest.files
        file_state state = file_state::installed;
        uint64_t download = 0; // bytes still to fetch
    };

    struct component_status {
        component info;
        std::vector<file_status> files;
        uint64_t size = 0;
        uint64_t download = 0;
        file_state state = file_state::installed;
    };

    struct status {
        bool ok = false;
        std::string error;
        manifest data;
        std::vector<component_status> components;
        uint64_t download = 0;
        bool ready = false; // every required file is installed and current
        bool launcher_update = false;
    };

    // phase is "checking", "downloading", "installing" or "launcher"
    struct progress_info {
        std::string phase;
        std::string file;
        uint64_t done = 0;
        uint64_t total = 0;
        double speed = 0; // bytes per second
    };

    using progress_fn = std::function<void(const progress_info&)>;

    std::string launcher_version();

    // 1.2.10 > 1.2.9, missing parts count as 0
    int compare_versions(const std::string& a, const std::string& b);

    // the manifest's URL, launcher/content-url.txt overrides it (a URL or a local folder for testing, "none" turns checks off)
    std::string manifest_location();

    // full_verify rehashes every file instead of trusting the cache
    status check(bool full_verify, const progress_fn& progress, const std::atomic<bool>& cancel);

    // downloads and installs every missing or outdated file of the status
    bool download_all(const status& current, const progress_fn& progress, const std::atomic<bool>& cancel, std::string& error);

    // swaps in the manifest's launcher, the caller starts it and exits
    bool update_launcher(const status& current, const progress_fn& progress, const std::atomic<bool>& cancel, std::string& error);

    // removes the exe a self-update left behind
    void cleanup_old_launcher();

    // starts the swapped-in launcher
    bool restart_launcher();
}
