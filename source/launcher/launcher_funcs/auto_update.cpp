#include "std_include.hpp"
#include "auto_update.hpp"
#include "download.hpp"
#include "linux_run.hpp"
#include "../launcher_main/launcher_actions.hpp"

#include <cstdio>
#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;

namespace updater {
    // the launcher's own version, the page shows it next to the title
    const std::string SERVER_VERSION = "1.1.2";

    const std::string GITHUB_API_URL = "https://api.github.com/repos/NotNierPea/shield-launcher/releases/latest";
    const std::string DOWNLOAD_URL_BASE = "https://github.com/NotNierPea/shield-launcher/releases/download/";

    namespace {
        // a launcher update is the archive of this platform: the windows one unpacks with
        // Expand-Archive, the linux one with tar. An asset that is neither is not for us
        const char* asset_extension() {
#ifdef _WIN32
            return ".zip";
#else
            return ".tar.gz";
#endif
        }

        bool ends_with(const std::string& text, const std::string& suffix) {
            return text.size() >= suffix.size()
                && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        fs::path state_path() {
            return launcher::launcher_dir() / "update-state.json";
        }

        bool read_state(rapidjson::Document& doc) {
            std::ifstream file(state_path(), std::ios::binary);
            if (!file)
                return false;

            const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            doc.Parse(content.c_str());
            return !doc.HasParseError() && doc.IsObject();
        }

        void write_state(rapidjson::Document& doc) {
            rapidjson::StringBuffer buffer;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);

            std::ofstream file(state_path(), std::ios::binary | std::ios::trunc);
            file.write(buffer.GetString(), static_cast<std::streamsize>(buffer.GetSize()));
        }

        std::string state_string(const char* key) {
            rapidjson::Document doc;
            if (!read_state(doc) || !doc.HasMember(key) || !doc[key].IsString())
                return {};
            return doc[key].GetString();
        }

        void set_state(const char* key, const std::string& value) {
            rapidjson::Document doc;
            if (!read_state(doc))
                doc.SetObject();

            auto& allocator = doc.GetAllocator();
            rapidjson::Value text(value.c_str(), allocator);

            if (doc.HasMember(key))
                doc[key] = text;
            else
                doc.AddMember(rapidjson::Value(key, allocator), text, allocator);

            write_state(doc);
        }

        // 1.2.0 is newer than 1.1.2, and a fork suffix never counts on its own
        bool is_newer(const std::string& latest, const std::string& current) {
            auto numbers = [](const std::string& version) {
                std::vector<int> parts;
                std::string text = version;
                if (!text.empty() && (text[0] == 'v' || text[0] == 'V'))
                    text = text.substr(1);

                std::stringstream stream(text);
                for (std::string part; std::getline(stream, part, '.');) {
                    try {
                        parts.push_back(std::stoi(part));
                    }
                    catch (const std::exception&) {
                        break;
                    }
                }
                return parts;
            };

            const std::vector<int> left = numbers(latest);
            const std::vector<int> right = numbers(current);
            if (left.empty() || right.empty())
                return false;

            for (size_t i = 0; i < std::min(left.size(), right.size()); ++i) {
                if (left[i] != right[i])
                    return left[i] > right[i];
            }

            return false;
        }

        std::string shell_quote(const std::string& text) {
            std::string out = "'";
            for (char c : text) {
                if (c == '\'')
                    out += "'\\''";
                else
                    out += c;
            }
            out += "'";
            return out;
        }
    }

    std::string get_server_version() {
        return SERVER_VERSION;
    }

    std::string get_latest_version() {
        const std::string latest = state_string("latest");
        return latest.empty() ? "release" : latest;
    }

    void mark_prompted() {
        set_state("prompted", state_string("latest"));
    }

    bool check_for_updates() {
        std::string error;
        const std::string response = webdl::fetch(GITHUB_API_URL, error);
        if (response.empty()) {
            std::cout << "Update check failed: " << error << std::endl;
            return false;
        }

        rapidjson::Document release;
        release.Parse(response.c_str());
        if (release.HasParseError() || !release.IsObject() || !release.HasMember("tag_name") || !release["tag_name"].IsString()) {
            std::cout << "Update check: could not read the GitHub answer" << std::endl;
            return false;
        }

        const std::string latest = release["tag_name"].GetString();

        // only the archive of this platform is an update, installing the other one would
        // leave a launcher that cannot start
        std::string asset_name;
        std::string asset_url;
        if (release.HasMember("assets") && release["assets"].IsArray()) {
            for (const auto& asset : release["assets"].GetArray()) {
                if (!asset.IsObject() || !asset.HasMember("name") || !asset["name"].IsString())
                    continue;

                const std::string name = asset["name"].GetString();
                if (!ends_with(name, asset_extension()))
                    continue;

                asset_name = name;
                if (asset.HasMember("browser_download_url") && asset["browser_download_url"].IsString())
                    asset_url = asset["browser_download_url"].GetString();
                break;
            }
        }

        std::cout << "Version " << SERVER_VERSION << ", latest " << latest;
        if (asset_name.empty())
            std::cout << " (no " << asset_extension() << " asset, this platform is not built there)";
        std::cout << std::endl;

        set_state("latest", latest);
        set_state("asset", asset_name);
        set_state("url", asset_url.empty() ? DOWNLOAD_URL_BASE + latest + "/" + asset_name : asset_url);

        if (asset_name.empty() || asset_url.empty())
            return false;

        if (state_string("prompted") == latest)
            return false;

        return is_newer(latest, SERVER_VERSION);
    }

    bool download_and_update() {
        const std::string latest = state_string("latest");
        const std::string asset = state_string("asset");
        const std::string url = state_string("url");
        if (latest.empty() || url.empty())
            return false;

        std::cout << "[update] state " << state_path().string() << " latest=" << state_string("latest")
                  << " asset=" << state_string("asset") << " url=" << state_string("url") << std::endl;

        const fs::path exe = launcher::executable_path();
        const fs::path exe_dir = exe.parent_path();
        const fs::path temp = fs::temp_directory_path() / "shield-launcher-update";
        std::error_code ec;
        fs::remove_all(temp, ec);
        fs::create_directories(temp, ec);

        const fs::path archive = temp / asset;
        const std::string error = webdl::download(url, archive.string());
        if (!error.empty()) {
            std::cout << error << std::endl;
            return false;
        }

#ifdef _WIN32
        // a batch that waits for us to close, unpacks, copies over the launcher folder
        // and starts it again
        const fs::path script = temp / "update.bat";
        std::ofstream batch(script);
        batch << "@echo off\r\n";
        batch << "echo Waiting for the launcher to close...\r\n";
        batch << "timeout /t 3 > nul\r\n";
        batch << "powershell -Command \"Expand-Archive -Path '" << archive.string() << "' -DestinationPath '"
              << temp.string() << "' -Force\"\r\n";
        batch << "set src=\"" << temp.string() << "\"\r\n";
        batch << "for /d %%i in (\"" << temp.string() << "\\*\") do set src=\"%%i\"\r\n";
        batch << "xcopy /s /y \"%src%\\*\" \"" << exe_dir.string() << "\\\" > nul\r\n";
        batch << "rmdir /s /q \"" << temp.string() << "\" > nul\r\n";
        batch << "start \"\" \"" << exe.string() << "\"\r\n";
        batch << "del \"%~f0\"\r\n";
        batch.close();

        const std::string command = "start \"\" \"" + script.string() + "\"";
        return std::system(command.c_str()) == 0;
#else
        // the archive holds Project_BO4_Launcher_Linux/, copy its content over the launcher folder
        const fs::path script = temp / "update.sh";
        std::ofstream shell(script);
        shell << "#!/bin/sh\n";
        shell << "while kill -0 " << getpid() << " 2>/dev/null; do sleep 1; done\n";
        shell << "tmp=$(mktemp -d)\n";
        shell << "tar -xzf " << shell_quote(archive.string()) << " -C \"$tmp\" || exit 1\n";
        shell << "cp -rf \"$tmp\"/*/. " << shell_quote(exe_dir.string()) << "/ || exit 1\n";
        shell << "chmod +x " << shell_quote(exe.string()) << "\n";
        shell << "rm -rf \"$tmp\" " << shell_quote(archive.string()) << "\n";
        shell << "cd " << shell_quote(exe_dir.string()) << " && " << shell_quote(exe.string()) << " >/dev/null 2>&1 &\n";
        shell.close();

        fs::permissions(script, fs::perms::owner_exec | fs::perms::owner_read, fs::perm_options::add, ec);

        // a detached child, so it survives the launcher closing and the shell never gets in the way
        std::string spawn_error;
        if (!launcher::spawn_detached("/bin/sh", { script.string() }, {}, exe_dir.string(), &spawn_error)) {
            std::cout << "[update] " << spawn_error << std::endl;
            return false;
        }

        return true;
#endif
    }
}
