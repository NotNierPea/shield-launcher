#include "std_include.hpp"
#include "../launcher_main/launcher_actions.hpp"

#ifdef _WIN32
// Linux only, the Windows host talks to wine through its own code path
#else

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <cctype>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
    std::string home_dir() {
        const char* home = std::getenv("HOME");
        return home ? std::string(home) : std::string();
    }

    bool has_proton_script(const fs::path& dir) {
        std::error_code ec;
        return fs::exists(dir / "proton", ec);
    }

    // a bare wine build exposes bin/wine, a Proton build files/bin/wine
    fs::path wine_binary(const std::string& runtime_path) {
        std::error_code ec;
        fs::path dir(runtime_path);
        if (fs::exists(dir / "files" / "bin" / "wine", ec))
            return dir / "files" / "bin" / "wine";
        if (fs::exists(dir / "bin" / "wine", ec))
            return dir / "bin" / "wine";
        return "wine";
    }

    bool prefix_has_battlenet(const fs::path& prefix) {
        std::error_code ec;
        return fs::exists(prefix / "drive_c" / "Program Files (x86)" / "Battle.net", ec);
    }

    void add_runtime(std::vector<launcher::linux_runtime>& out, const fs::path& dir) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return;

        const bool proton = has_proton_script(dir);
        const bool wine = fs::exists(dir / "files" / "bin" / "wine", ec) || fs::exists(dir / "bin" / "wine", ec);
        if (!proton && !wine)
            return;

        const std::string canonical = fs::weakly_canonical(dir, ec).string();
        for (const auto& existing : out) {
            if (existing.path == canonical)
                return;
        }

        launcher::linux_runtime runtime;
        runtime.name = dir.filename().string();
        runtime.path = canonical;
        runtime.kind = proton ? "proton" : "wine";
        out.push_back(runtime);
    }

    void add_runtimes_from(std::vector<launcher::linux_runtime>& out, const fs::path& parent) {
        std::error_code ec;
        if (!fs::is_directory(parent, ec))
            return;

        for (const auto& entry : fs::directory_iterator(parent, ec)) {
            if (entry.is_directory())
                add_runtime(out, entry.path());
        }
    }

    std::string read_file(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    // runs to completion, the wineboot that builds a prefix takes a few minutes
    bool run_blocking(const std::string& program, const std::vector<std::string>& args,
                      const std::vector<std::pair<std::string, std::string>>& env,
                      int timeout_seconds, std::string* error) {
        std::vector<char*> argv;
        std::vector<std::string> storage{program};
        for (const auto& arg : args)
            storage.push_back(arg);
        for (auto& arg : storage)
            argv.push_back(arg.data());
        argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid < 0) {
            if (error) *error = "fork() failed";
            return false;
        }

        if (pid == 0) {
            for (const auto& [key, value] : env)
                setenv(key.c_str(), value.c_str(), 1);
            execvp(argv[0], argv.data());
            _exit(127);
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        int status = 0;
        while (true) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid)
                return true;
            if (result < 0) {
                if (error) *error = "waitpid() failed";
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                if (error) *error = program + " timed out";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

namespace launcher {
    bool spawn_detached(const std::string& program, const std::vector<std::string>& args,
                        const std::vector<std::pair<std::string, std::string>>& env,
                        const std::string& work_dir, std::string* error) {
        std::vector<std::string> argv_storage;
        argv_storage.push_back(program);
        for (const auto& arg : args)
            argv_storage.push_back(arg);

        std::vector<char*> argv;
        for (auto& arg : argv_storage)
            argv.push_back(arg.data());
        argv.push_back(nullptr);

        pid_t middle = fork();
        if (middle < 0) {
            if (error) *error = "fork() failed";
            return false;
        }

        if (middle == 0) {
            pid_t child = fork();
            if (child < 0)
                _exit(1);
            if (child > 0)
                _exit(0);

            setsid();
            if (!work_dir.empty() && chdir(work_dir.c_str()) != 0)
                _exit(1);

            for (const auto& [key, value] : env)
                setenv(key.c_str(), value.c_str(), 1);

            execvp(argv[0], argv.data());
            _exit(127);
        }

        int status = 0;
        waitpid(middle, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (error) *error = "Could not start " + program;
            return false;
        }

        return true;
    }

    namespace {
        // battle.net keeps the game language in the launch options of the game, in the
        // prefix's user registry. Wine stores it as plain text, so the launcher edits it
        // directly instead of starting a wine process for every change
        const char* vipr_section = "[Software\\\\Blizzard Entertainment\\\\Battle.net\\\\Launch Options\\\\VIPR]";

        const char* game_languages[] = { "enUS", "frFR", "deDE", "itIT", "esES", "jaJP", "ruRU", "koKR", "ptBR", "esMX", "zhTW", "plPL" };

        bool known_language(const std::string& locale) {
            return std::any_of(std::begin(game_languages), std::end(game_languages),
                [&](const char* code) { return locale == code; });
        }

        fs::path user_registry_path() {
            const std::string prefix = load_linux_settings().prefix_path;
            if (prefix.empty())
                return {};

            std::error_code ec;
            const fs::path root(prefix);
            if (fs::exists(root / "user.reg", ec))
                return root / "user.reg";
            if (fs::exists(root / "pfx" / "user.reg", ec))
                return root / "pfx" / "user.reg";
            return {};
        }

        std::vector<std::string> read_lines(const fs::path& file) {
            std::vector<std::string> lines;
            std::ifstream stream(file, std::ios::binary);

            for (std::string line; std::getline(stream, line);) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }

            return lines;
        }

        bool write_lines(const fs::path& file, const std::vector<std::string>& lines) {
            std::ofstream stream(file, std::ios::binary | std::ios::trunc);
            if (!stream)
                return false;

            for (const auto& line : lines)
                stream << line << '\n';

            return stream.good();
        }

        bool in_section(const std::string& line, const char* section) {
            return line.rfind(section, 0) == 0;
        }

        bool is_section(const std::string& line) {
            return !line.empty() && line.front() == '[';
        }

        // the value of "NAME"="VALUE" inside its line, empty when there is none
        std::string value_in_line(const std::string& line, const char* name) {
            const std::string key = std::string("\"") + name + "\"=";
            if (line.rfind(key, 0) != 0)
                return {};

            const size_t open = line.find('"', key.size());
            if (open == std::string::npos)
                return {};

            const size_t close = line.find('"', open + 1);
            if (close == std::string::npos)
                return {};

            return line.substr(open + 1, close - open - 1);
        }
    }

    std::string game_language() {
        const fs::path file = user_registry_path();
        if (file.empty())
            return "";

        bool inside = false;
        for (const auto& line : read_lines(file)) {
            if (is_section(line)) {
                inside = in_section(line, vipr_section);
                continue;
            }

            if (!inside)
                continue;

            const std::string locale = value_in_line(line, "LOCALE");
            if (!locale.empty())
                return known_language(locale) ? locale : "enUS";
        }

        // no launch options yet: battle.net's own default
        return "enUS";
    }

    bool set_game_language(const std::string& locale) {
        if (!known_language(locale))
            return false;

        const fs::path file = user_registry_path();
        if (file.empty())
            return false;

        std::vector<std::string> lines = read_lines(file);
        if (lines.empty())
            return false;

        const std::pair<const char*, std::string> values[] = {
            { "LOCALE", locale },
            { "LOCALE_AUDIO", locale },
        };

        size_t section = lines.size();
        for (size_t i = 0; i < lines.size(); ++i) {
            if (in_section(lines[i], vipr_section)) {
                section = i;
                break;
            }
        }

        if (section == lines.size()) {
            // no section: append it, then the two values
            lines.emplace_back(vipr_section);
            for (const auto& [name, value] : values)
                lines.push_back("\"" + std::string(name) + "\"=\"" + value + "\"");
            return write_lines(file, lines);
        }

        for (const auto& [name, value] : values) {
            const std::string entry = "\"" + std::string(name) + "\"=\"" + value + "\"";

            bool written = false;
            size_t insert_at = section + 1;
            for (size_t i = section + 1; i < lines.size(); ++i) {
                if (is_section(lines[i]))
                    break;

                if (lines[i].rfind(std::string("\"") + name + "\"=", 0) == 0) {
                    lines[i] = entry;
                    written = true;
                    break;
                }

                ++insert_at;
            }

            if (!written)
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), entry);
        }

        return write_lines(file, lines);
    }

    std::vector<linux_runtime> linux_runtimes() {
        std::vector<linux_runtime> runtimes;
        const std::string home = home_dir();
        if (home.empty())
            return runtimes;

        const fs::path h(home);

        // steam
        add_runtimes_from(runtimes, h / ".local/share/Steam/steamapps/common");
        add_runtimes_from(runtimes, h / ".steam/root/steamapps/common");
        add_runtimes_from(runtimes, h / ".local/share/Steam/compatibilitytools.d");
        add_runtimes_from(runtimes, h / ".steam/root/compatibilitytools.d");

        // heroic and umu
        add_runtimes_from(runtimes, h / ".config/heroic/tools/proton");
        add_runtimes_from(runtimes, h / ".config/heroic/tools/wine");
        add_runtimes_from(runtimes, h / ".local/share/umu/compatibilitytools");

        std::sort(runtimes.begin(), runtimes.end(), [](const linux_runtime& a, const linux_runtime& b) {
            return a.name < b.name;
        });

        return runtimes;
    }

    std::string detect_linux_prefix() {
        const std::string home = home_dir();
        if (home.empty())
            return {};

        const fs::path h(home);
        std::error_code ec;

        // heroic layout: Prefixes/<store>/<prefix>
        for (const fs::path& root : { h / "Games/Heroic/Prefixes", h / ".config/heroic/prefixes" }) {
            if (!fs::is_directory(root, ec))
                continue;

            for (const auto& store : fs::directory_iterator(root, ec)) {
                if (!store.is_directory())
                    continue;

                for (const auto& prefix : fs::directory_iterator(store.path(), ec)) {
                    if (!prefix.is_directory())
                        continue;
                    if (prefix_has_battlenet(prefix.path()))
                        return prefix.path().string();
                }
            }
        }

        // steam layout: compatdata/<appid>/pfx
        const fs::path compat_data = h / ".local/share/Steam/steamapps/compatdata";
        if (fs::is_directory(compat_data, ec)) {
            for (const auto& app : fs::directory_iterator(compat_data, ec)) {
                if (!app.is_directory())
                    continue;

                const fs::path pfx = app.path() / "pfx";
                if (prefix_has_battlenet(pfx))
                    return pfx.string();
            }
        }

        return {};
    }

    bool linux_prefix_ready(const std::string& dir) {
        if (dir.empty())
            return false;

        std::error_code ec;
        const fs::path p(dir);
        if (!fs::is_directory(p, ec))
            return false;

        // wine writes system.reg/user.reg at the prefix root, heroic points "pfx" back at the same directory
        return fs::exists(p / "system.reg", ec) || fs::exists(p / "pfx" / "system.reg", ec);
    }

    linux_settings load_linux_settings() {
        linux_settings values;
        values.backend = "umu";

        rapidjson::Document doc;
        const fs::path path = launcher_dir() / "launcher-config.json";
        const std::string content = read_file(path);
        if (!content.empty())
            doc.Parse(content.c_str());

        if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("linux") && doc["linux"].IsObject()) {
            const auto& section = doc["linux"];
            if (section.HasMember("runtimePath") && section["runtimePath"].IsString())
                values.runtime_path = section["runtimePath"].GetString();
            if (section.HasMember("prefixPath") && section["prefixPath"].IsString())
                values.prefix_path = section["prefixPath"].GetString();
            if (section.HasMember("backend") && section["backend"].IsString())
                values.backend = section["backend"].GetString();
        }

        if (values.runtime_path.empty()) {
            const auto all = linux_runtimes();
            if (!all.empty())
                values.runtime_path = all.front().path;
        }
        if (values.prefix_path.empty())
            values.prefix_path = detect_linux_prefix();

        return values;
    }

    bool save_linux_settings(const linux_settings& values, std::string& error) {
        const fs::path path = launcher_dir() / "launcher-config.json";

        rapidjson::Document doc;
        const std::string content = read_file(path);
        if (!content.empty())
            doc.Parse(content.c_str());
        if (doc.HasParseError() || !doc.IsObject())
            doc.SetObject();

        auto& allocator = doc.GetAllocator();
        rapidjson::Value section(rapidjson::kObjectType);
        section.AddMember("runtimePath", rapidjson::Value(values.runtime_path.c_str(), allocator), allocator);
        section.AddMember("prefixPath", rapidjson::Value(values.prefix_path.c_str(), allocator), allocator);
        section.AddMember("backend", rapidjson::Value(values.backend.c_str(), allocator), allocator);

        if (doc.HasMember("linux"))
            doc["linux"] = section;
        else
            doc.AddMember("linux", section, allocator);

        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            error = "Failed to save launcher-config.json.";
            return false;
        }

        out << buffer.GetString();
        return true;
    }

    bool create_linux_prefix(const linux_settings& values, std::string& error) {
        if (values.prefix_path.empty()) {
            error = "No prefix directory selected.";
            return false;
        }

        std::error_code ec;
        fs::create_directories(values.prefix_path, ec);

        const std::vector<std::pair<std::string, std::string>> env = {
            { "WINEPREFIX", values.prefix_path },
            { "WINEDEBUG", "-all" },
        };

        if (!run_blocking(wine_binary(values.runtime_path).string(), { "wineboot", "-u" }, env, 300, &error))
            return false;

        if (!linux_prefix_ready(values.prefix_path)) {
            error = "The prefix was not created in " + values.prefix_path;
            return false;
        }

        return true;
    }

    bool linux_game_running() {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/proc", ec)) {
            if (!entry.is_directory())
                continue;

            const std::string name = entry.path().filename().string();
            if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0])))
                continue;

            const std::string cmdline = read_file(entry.path() / "cmdline");
            if (cmdline.find("BlackOps4.exe") != std::string::npos)
                return true;
        }

        return false;
    }

    namespace {
        // the game itself, through umu-run or a plain wine binary
        bool launch_under_wine(const std::string& exe_path, const std::string& arguments,
                               std::string* error) {
            std::error_code ec;
            if (!fs::exists(exe_path, ec)) {
                if (error) *error = "Game executable not found: " + exe_path;
                return false;
            }

            linux_settings values = load_linux_settings();
            if (values.prefix_path.empty()) {
                if (error) *error = "No Wine prefix selected. Open the Linux settings and pick one.";
                return false;
            }

            if (!linux_prefix_ready(values.prefix_path)) {
                if (error) *error = "The prefix was not created in " + values.prefix_path;
                return false;
            }

            std::vector<std::pair<std::string, std::string>> env = {
                { "WINEPREFIX", values.prefix_path },
                // the support dll sits next to the exe and must win over wine's builtin
                { "WINEDLLOVERRIDES", "XInput9_1_0.dll=n,b" },
                { "WINEDEBUG", "-all" },
            };

            std::vector<std::string> args;
            std::string program;
            if (values.backend == "umu") {
                env.push_back({ "PROTONPATH", values.runtime_path });
                env.push_back({ "GAMEID", "0" });
                env.push_back({ "STORE", "none" });
                program = "umu-run";
                args.push_back(exe_path);
            }
            else {
                program = wine_binary(values.runtime_path).string();
                args.push_back(exe_path);
            }

            std::stringstream stream(arguments);
            for (std::string token; stream >> token;)
                args.push_back(token);

            const std::string work_dir = fs::path(exe_path).parent_path().string();
            return spawn_detached(program, args, env, work_dir, error);
        }
    }

    bool launch_linux_game(const std::string& exe_path, const std::string& arguments, std::string* error) {
        return launch_under_wine(exe_path, arguments, error);
    }
}

#endif // _WIN32
