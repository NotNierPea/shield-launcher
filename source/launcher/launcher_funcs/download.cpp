#include "std_include.hpp"
#include "download.hpp"

#include <filesystem>

namespace fs = std::filesystem;

namespace {
#ifdef _WIN32
    std::string quote(const std::string& text) {
        std::string out = "\"";
        for (char c : text) {
            if (c == '"')
                out += "\\\"";
            else
                out += c;
        }
        out += "\"";
        return out;
    }
#else
    std::string quote(const std::string& text) {
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
#endif

    // curl is the one tool that is on every Windows 10+ and on every desktop
    // Linux, the other two are fallbacks for stripped down machines
    const char* const curl = "curl -fsSL --retry 2 --connect-timeout 15 -A ShieldLauncher";
    const char* const wget = "wget -q -O";

    bool run(const std::string& command) {
        return std::system(command.c_str()) == 0;
    }

    std::string read_all(const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
}

namespace webdl {
    std::string download(const std::string& url, const std::string& destPath) {
        const fs::path dest(destPath);
        std::error_code ec;
        if (dest.has_parent_path())
            fs::create_directories(dest.parent_path(), ec);
        fs::remove(dest, ec);

        if (run(std::string(curl) + " -o " + quote(destPath) + " " + quote(url)) && fs::exists(dest, ec) && fs::file_size(dest, ec) > 0)
            return {};

        fs::remove(dest, ec);

#ifdef _WIN32
        // no curl.exe: powershell still speaks TLS 1.2
        const std::string powershell = "powershell -NoProfile -Command \"try { Invoke-WebRequest -UseBasicParsing -Uri "
            + quote(url) + " -OutFile " + quote(destPath) + " } catch { exit 1 }\"";
        if (run(powershell) && fs::exists(dest, ec) && fs::file_size(dest, ec) > 0)
            return {};
#else
        if (run(std::string(wget) + " " + quote(destPath) + " " + quote(url)) && fs::exists(dest, ec) && fs::file_size(dest, ec) > 0)
            return {};
#endif

        fs::remove(dest, ec);
        return "Download failed: " + url;
    }

    std::string fetch(const std::string& url, std::string& error) {
        error.clear();

        const fs::path temp = fs::temp_directory_path() / "shield-launcher-fetch";
        const std::string path = temp.string();

        error = download(url, path);
        if (!error.empty())
            return {};

        std::string content = read_all(temp);
        std::error_code ec;
        fs::remove(temp, ec);
        return content;
    }
}
