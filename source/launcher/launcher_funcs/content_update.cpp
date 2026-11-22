#include <std_include.hpp>
#include "content_update.hpp"
#include "dll_loading.hpp"
#include "../launcher_main/launcher_actions.hpp"
#include "../version.hpp"
#include "debugging/serverlog.hpp"
#include <bcrypt.h>
#include <winhttp.h>
#include <zlib.h>
#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

namespace content {
    namespace {
        constexpr const char* default_manifest_url = "https://github.com/NotNierPea/shield-launcher/releases/download/content/manifest.json";
        constexpr int supported_schema = 1;
        constexpr int max_attempts = 4;
        constexpr size_t io_chunk = 1 << 20;

        // utf-8 manifest paths to windows paths
        fs::path from_utf8(const std::string& text) {
            return fs::path(std::u8string(text.begin(), text.end()));
        }

        std::string to_utf8(const fs::path& path) {
            auto text = path.generic_u8string();
            return std::string(text.begin(), text.end());
        }

        std::wstring widen(const std::string& text) {
            int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(size, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
            return wide;
        }

        std::string trim(std::string text) {
            const char* space = " \t\r\n";
            text.erase(0, text.find_first_not_of(space));
            text.erase(text.find_last_not_of(space) + 1);
            return text;
        }

        std::string system_error(DWORD code) {
            char* buffer = nullptr;
            DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
            HMODULE module = nullptr;
            // winhttp's codes are in its own dll
            if (code >= 12000 && code <= 12200) {
                module = GetModuleHandleW(L"winhttp.dll");
                flags |= FORMAT_MESSAGE_FROM_HMODULE;
            }
            FormatMessageA(flags, module, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
            std::string text = buffer ? trim(buffer) : "error " + std::to_string(code);
            if (buffer)
                LocalFree(buffer);
            return text;
        }

        // a manifest path stays inside the game folder: relative, no drive, no ..
        bool safe_path(const std::string& path) {
            if (path.empty() || path[0] == '/' || path[0] == '\\' || path.find(':') != std::string::npos)
                return false;
            std::stringstream stream(path);
            for (std::string part; std::getline(stream, part, '/');) {
                if (part.empty() || part == "." || part == ".." || part.find('\\') != std::string::npos)
                    return false;
            }
            return true;
        }

        bool is_hex_sha(const std::string& text) {
            return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
        }

        /* sha256 */

        class sha256 {
        public:
            sha256() {
                static BCRYPT_ALG_HANDLE algorithm = [] {
                    BCRYPT_ALG_HANDLE handle = nullptr;
                    BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
                    return handle;
                }();
                BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
            }

            ~sha256() {
                if (hash)
                    BCryptDestroyHash(hash);
            }

            sha256(const sha256&) = delete;
            sha256& operator=(const sha256&) = delete;

            void update(const void* data, size_t size) {
                BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0);
            }

            std::string finish() {
                UCHAR digest[32] = {};
                BCryptFinishHash(hash, digest, sizeof(digest), 0);
                static const char* hex = "0123456789abcdef";
                std::string text;
                for (UCHAR byte : digest) {
                    text += hex[byte >> 4];
                    text += hex[byte & 15];
                }
                return text;
            }

        private:
            BCRYPT_HASH_HANDLE hash = nullptr;
        };

        std::string hash_whole_file(const fs::path& path) {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return "";
            sha256 hasher;
            std::vector<char> buffer(io_chunk);
            while (file) {
                file.read(buffer.data(), buffer.size());
                hasher.update(buffer.data(), static_cast<size_t>(file.gcount()));
            }
            return hasher.finish();
        }

        /* fetching: http(s) through winhttp, or a local folder for testing */

        bool is_remote(const std::string& location) {
            return location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0;
        }

        HINTERNET http_session() {
            static HINTERNET session = [] {
                std::wstring agent = L"ShieldLauncher/" + widen(LAUNCHER_VERSION);
                HINTERNET handle = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
                if (!handle)
                    handle = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
                if (handle)
                    WinHttpSetTimeouts(handle, 15000, 15000, 30000, 60000);
                return handle;
            }();
            return session;
        }

        struct handle_closer {
            HINTERNET handle;
            ~handle_closer() { if (handle) WinHttpCloseHandle(handle); }
        };

        // begin gets the status once the headers are in (false stops), sink each piece of the body; returns the status, 0 on a failure
        int fetch(const std::string& location, uint64_t offset, const std::function<bool(int)>& begin,
            const std::function<bool(const char*, size_t)>& sink, std::string& error) {
            if (!is_remote(location)) {
                fs::path path = from_utf8(location);
                std::ifstream file(path, std::ios::binary);
                if (!file)
                    return 404;
                file.seekg(static_cast<std::streamoff>(offset));
                int status = offset ? 206 : 200;
                if (!begin(status))
                    return status;
                std::vector<char> buffer(io_chunk);
                while (file) {
                    file.read(buffer.data(), buffer.size());
                    if (file.gcount() > 0 && !sink(buffer.data(), static_cast<size_t>(file.gcount())))
                        return status;
                }
                return status;
            }

            std::wstring url = widen(location);
            URL_COMPONENTS parts = { sizeof(parts) };
            parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
            if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
                error = "Bad URL: " + location;
                return 0;
            }

            std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
            std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
            bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

            HINTERNET session = http_session();
            if (!session) {
                error = "Couldn't start WinHTTP: " + system_error(GetLastError());
                return 0;
            }

            handle_closer connection{ WinHttpConnect(session, host.c_str(), parts.nPort, 0) };
            handle_closer request{ connection.handle ? WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr };
            if (!request.handle) {
                error = "Couldn't connect: " + system_error(GetLastError());
                return 0;
            }

            std::wstring headers = offset ? L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n" : L"";
            if (!WinHttpSendRequest(request.handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                    static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.handle, nullptr)) {
                error = "Download failed: " + system_error(GetLastError());
                return 0;
            }

            DWORD status = 0;
            DWORD length = sizeof(status);
            WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                &status, &length, WINHTTP_NO_HEADER_INDEX);
            if (!begin(static_cast<int>(status)) || (status != 200 && status != 206))
                return static_cast<int>(status);

            std::vector<char> buffer(64 * 1024);
            for (;;) {
                DWORD read = 0;
                if (!WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
                    error = "Download interrupted: " + system_error(GetLastError());
                    return 0;
                }
                if (read == 0)
                    break;
                if (!sink(buffer.data(), read))
                    break;
            }
            return static_cast<int>(status);
        }

        bool fetch_text(const std::string& location, std::string& text, std::string& error) {
            text.clear();
            int status = fetch(location, 0, [](int) { return true; }, [&](const char* data, size_t size) {
                text.append(data, size);
                return text.size() < (64u << 20);
            }, error);
            if (status != 200) {
                if (error.empty())
                    error = status == 404 ? "The game files aren't published yet (manifest.json not found)." : "Server answered " + std::to_string(status) + ".";
                return false;
            }
            return true;
        }

        /* manifest */

        std::string json_string(const rapidjson::Value& object, const char* key) {
            return object.IsObject() && object.HasMember(key) && object[key].IsString() ? object[key].GetString() : "";
        }

        uint64_t json_u64(const rapidjson::Value& object, const char* key) {
            return object.IsObject() && object.HasMember(key) && object[key].IsUint64() ? object[key].GetUint64() : 0;
        }

        bool read_file_entry(const rapidjson::Value& value, file_entry& entry, bool needs_path) {
            entry.path = json_string(value, "path");
            entry.component = json_string(value, "component");
            entry.size = json_u64(value, "size");
            entry.sha256 = json_string(value, "sha256");
            entry.extract = json_string(value, "extract");
            if ((needs_path && !safe_path(entry.path)) || !is_hex_sha(entry.sha256) || (!entry.extract.empty() && !safe_path(entry.extract)))
                return false;
            if (!entry.extract.empty() && value.HasMember("entries") && value["entries"].IsArray()) {
                for (const auto& item : value["entries"].GetArray()) {
                    package_file inner{ json_string(item, "path"), json_u64(item, "size"),
                        item.HasMember("crc32") && item["crc32"].IsUint() ? item["crc32"].GetUint() : 0 };
                    if (!safe_path(inner.path))
                        return false;
                    entry.entries.push_back(std::move(inner));
                }
            }
            if (!value.HasMember("parts") || !value["parts"].IsArray())
                return false;

            uint64_t total = 0;
            for (const auto& item : value["parts"].GetArray()) {
                part piece{ json_u64(item, "size"), json_string(item, "sha256") };
                if (!is_hex_sha(piece.sha256))
                    return false;
                total += piece.size;
                entry.parts.push_back(piece);
            }
            return total == entry.size;
        }

        bool parse_manifest(const std::string& text, const std::string& location, manifest& result, std::string& error) {
            rapidjson::Document doc;
            doc.Parse(text.c_str(), text.size());
            if (doc.HasParseError() || !doc.IsObject()) {
                error = "manifest.json can't be read.";
                return false;
            }

            result.schema = doc.HasMember("schema") && doc["schema"].IsInt() ? doc["schema"].GetInt() : 0;
            result.version = json_string(doc, "version");
            result.part_size = json_u64(doc, "part_size");
            result.base_url = json_string(doc, "base_url");
            if (result.base_url.empty())
                result.base_url = location.substr(0, location.find_last_of("/\\") + 1);
            else if (result.base_url.back() != '/')
                result.base_url += '/';

            // the launcher is read first: a launcher too old for the schema still finds its update
            if (doc.HasMember("launcher") && doc["launcher"].IsObject()) {
                result.launcher_version = json_string(doc["launcher"], "version");
                file_entry exe;
                if (doc["launcher"].HasMember("file") && read_file_entry(doc["launcher"]["file"], exe, false))
                    result.launcher_file = exe;
            }

            if (result.schema != supported_schema) {
                error = "The game files need a newer launcher. Update the launcher first.";
                return false;
            }

            if (doc.HasMember("components") && doc["components"].IsArray()) {
                for (const auto& item : doc["components"].GetArray()) {
                    component info;
                    info.id = json_string(item, "id");
                    info.name = json_string(item, "name");
                    info.description = json_string(item, "description");
                    info.required = !(item.HasMember("required") && item["required"].IsFalse());
                    if (info.name.empty())
                        info.name = info.id;
                    if (!info.id.empty())
                        result.components.push_back(info);
                }
            }

            if (!doc.HasMember("files") || !doc["files"].IsArray()) {
                error = "manifest.json has no files.";
                return false;
            }

            for (const auto& item : doc["files"].GetArray()) {
                file_entry entry;
                if (!read_file_entry(item, entry, true)) {
                    error = "manifest.json has a bad file entry: " + json_string(item, "path");
                    return false;
                }
                result.files.push_back(std::move(entry));
            }

            if (doc.HasMember("remove") && doc["remove"].IsArray()) {
                for (const auto& item : doc["remove"].GetArray()) {
                    if (item.IsString() && safe_path(item.GetString()))
                        result.remove.push_back(item.GetString());
                }
            }

            return true;
        }

        /* local state: launcher/content-state.json caches each file's hashes by size and time, so files aren't rehashed every start */

        struct local_hash {
            uint64_t size = 0;
            int64_t time = 0;
            std::string sha256;
            uint64_t part_size = 0;
            std::vector<std::string> parts;
        };

        using hash_cache = std::map<std::string, local_hash>;

        fs::path cache_path() {
            return launcher::launcher_dir() / "content-state.json";
        }

        hash_cache load_cache() {
            hash_cache cache;
            std::ifstream file(cache_path(), std::ios::binary);
            if (!file)
                return cache;
            std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            rapidjson::Document doc;
            doc.Parse(text.c_str(), text.size());
            if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("files") || !doc["files"].IsObject())
                return cache;

            for (auto it = doc["files"].MemberBegin(); it != doc["files"].MemberEnd(); ++it) {
                const auto& value = it->value;
                local_hash entry;
                entry.size = json_u64(value, "size");
                entry.time = value.HasMember("time") && value["time"].IsInt64() ? value["time"].GetInt64() : 0;
                entry.sha256 = json_string(value, "sha256");
                entry.part_size = json_u64(value, "part_size");
                if (value.HasMember("parts") && value["parts"].IsArray()) {
                    for (const auto& item : value["parts"].GetArray()) {
                        if (item.IsString())
                            entry.parts.push_back(item.GetString());
                    }
                }
                cache[it->name.GetString()] = std::move(entry);
            }
            return cache;
        }

        void save_cache(const hash_cache& cache) {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            writer.StartObject();
            writer.Key("files");
            writer.StartObject();
            for (const auto& [path, entry] : cache) {
                writer.Key(path.c_str());
                writer.StartObject();
                writer.Key("size");
                writer.Uint64(entry.size);
                writer.Key("time");
                writer.Int64(entry.time);
                writer.Key("sha256");
                writer.String(entry.sha256.c_str());
                writer.Key("part_size");
                writer.Uint64(entry.part_size);
                writer.Key("parts");
                writer.StartArray();
                for (const auto& sha : entry.parts)
                    writer.String(sha.c_str());
                writer.EndArray();
                writer.EndObject();
            }
            writer.EndObject();
            writer.EndObject();

            // written beside and swapped in, a crash never leaves half a cache
            fs::path path = cache_path();
            fs::path temp = path;
            temp += ".tmp";
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                if (!file)
                    return;
                file.write(buffer.GetString(), buffer.GetSize());
            }
            MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        bool stat_file(const fs::path& path, uint64_t& size, int64_t& time) {
            std::error_code ec;
            size = fs::file_size(path, ec);
            if (ec)
                return false;
            auto written = fs::last_write_time(path, ec);
            if (ec)
                return false;
            time = written.time_since_epoch().count();
            return true;
        }

        // the whole file's hash and one per part_size piece in one read
        bool hash_local(const fs::path& path, uint64_t part_size, local_hash& result, const std::function<bool(size_t)>& advance) {
            if (!stat_file(path, result.size, result.time))
                return false;

            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;

            result.part_size = part_size;
            result.parts.clear();
            sha256 whole;
            auto piece = std::make_unique<sha256>();
            uint64_t in_piece = 0;
            std::vector<char> buffer(io_chunk);

            while (file) {
                size_t want = buffer.size();
                if (part_size)
                    want = static_cast<size_t>(std::min<uint64_t>(want, part_size - in_piece));
                file.read(buffer.data(), want);
                size_t got = static_cast<size_t>(file.gcount());
                if (got == 0)
                    break;

                whole.update(buffer.data(), got);
                piece->update(buffer.data(), got);
                in_piece += got;
                if (part_size && in_piece == part_size) {
                    result.parts.push_back(piece->finish());
                    piece = std::make_unique<sha256>();
                    in_piece = 0;
                }
                if (!advance(got))
                    return false;
            }

            if (in_piece || result.parts.empty())
                result.parts.push_back(piece->finish());
            result.sha256 = whole.finish();
            return true;
        }

        // the cached hash when size and time still match, else a fresh one (which goes in the cache)
        bool local_hash_of(const std::string& key, const fs::path& path, uint64_t part_size, hash_cache& cache, bool full_verify,
            local_hash& result, const std::function<bool(size_t)>& advance) {
            uint64_t size = 0;
            int64_t time = 0;
            if (!stat_file(path, size, time))
                return false;

            auto cached = cache.find(key);
            if (!full_verify && cached != cache.end() && cached->second.size == size && cached->second.time == time &&
                cached->second.part_size == part_size && !cached->second.sha256.empty()) {
                result = cached->second;
                return true;
            }

            if (!hash_local(path, part_size, result, advance))
                return false;
            cache[key] = result;
            return true;
        }

        // bytes of the new file that can't be taken from the local one
        uint64_t download_size(const file_entry& file, const local_hash* local) {
            std::unordered_set<std::string> have;
            if (local)
                have.insert(local->parts.begin(), local->parts.end());
            uint64_t total = 0;
            for (const auto& piece : file.parts) {
                if (!have.count(piece.sha256))
                    total += piece.size;
            }
            return total;
        }

        /* packages: a zip unzipped into a folder (zlib's raw inflate, stored and deflated entries, no zip64) */

        struct zip_item {
            std::string name;
            uint16_t method = 0;
            uint32_t crc = 0;
            uint64_t packed = 0;
            uint64_t size = 0;
            uint64_t header = 0;
        };

        uint16_t le16(const uint8_t* at) {
            return static_cast<uint16_t>(at[0] | at[1] << 8);
        }

        uint32_t le32(const uint8_t* at) {
            return le16(at) | static_cast<uint32_t>(le16(at + 2)) << 16;
        }

        bool read_bytes(const fs::path& path, std::vector<uint8_t>& data) {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;
            data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return true;
        }

        // the central directory's entries, files only
        bool zip_items(const std::vector<uint8_t>& data, std::vector<zip_item>& items, std::string& error) {
            error = "The package isn't a valid zip.";
            if (data.size() < 22)
                return false;

            // the end record is in the last 64 KB (its comment can be that long)
            size_t end = std::string::npos;
            size_t lowest = data.size() > 22 + 0xFFFF ? data.size() - 22 - 0xFFFF : 0;
            for (size_t at = data.size() - 22;; --at) {
                if (le32(&data[at]) == 0x06054b50) {
                    end = at;
                    break;
                }
                if (at == lowest)
                    break;
            }
            if (end == std::string::npos)
                return false;

            uint16_t count = le16(&data[end + 10]);
            uint32_t offset = le32(&data[end + 16]);
            if (count == 0xFFFF || offset == 0xFFFFFFFF) {
                error = "The package is a zip64, which isn't supported.";
                return false;
            }

            size_t at = offset;
            for (uint16_t i = 0; i < count; ++i) {
                if (at + 46 > data.size() || le32(&data[at]) != 0x02014b50)
                    return false;
                zip_item item;
                item.method = le16(&data[at + 10]);
                item.crc = le32(&data[at + 16]);
                item.packed = le32(&data[at + 20]);
                item.size = le32(&data[at + 24]);
                uint16_t name_length = le16(&data[at + 28]);
                uint16_t extra_length = le16(&data[at + 30]);
                uint16_t comment_length = le16(&data[at + 32]);
                item.header = le32(&data[at + 42]);
                if (at + 46 + name_length > data.size())
                    return false;
                item.name.assign(reinterpret_cast<const char*>(&data[at + 46]), name_length);
                std::replace(item.name.begin(), item.name.end(), '\\', '/');
                at += 46 + name_length + extra_length + comment_length;

                if (!item.name.empty() && item.name.back() == '/')
                    continue;
                // a zip can't put a file outside its folder
                if (!safe_path(item.name)) {
                    error = "The package has a bad path: " + item.name;
                    return false;
                }
                items.push_back(std::move(item));
            }

            error.clear();
            return true;
        }

        // one entry's bytes, checked against its crc
        bool zip_read(const std::vector<uint8_t>& data, const zip_item& item, std::vector<uint8_t>& out, std::string& error) {
            error = "Couldn't unpack " + item.name + ".";
            if (item.header + 30 > data.size() || le32(&data[item.header]) != 0x04034b50)
                return false;
            uint64_t start = item.header + 30 + le16(&data[item.header + 26]) + le16(&data[item.header + 28]);
            if (start + item.packed > data.size())
                return false;

            out.assign(static_cast<size_t>(item.size), 0);
            if (item.method == 0) {
                if (item.packed != item.size)
                    return false;
                std::copy_n(&data[start], item.size, out.begin());
            }
            else if (item.method == 8) {
                z_stream stream = {};
                if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                    return false;
                stream.next_in = &data[start];
                stream.avail_in = static_cast<uInt>(item.packed);
                stream.next_out = out.data();
                stream.avail_out = static_cast<uInt>(out.size());
                int result = item.size ? inflate(&stream, Z_FINISH) : Z_STREAM_END;
                inflateEnd(&stream);
                if (result != Z_STREAM_END || stream.total_out != item.size)
                    return false;
            }
            else {
                error = item.name + " uses a zip compression that isn't supported.";
                return false;
            }

            if (crc32(0, out.data(), static_cast<uInt>(out.size())) != item.crc)
                return false;
            error.clear();
            return true;
        }

        uint32_t file_crc(const fs::path& path) {
            std::ifstream file(path, std::ios::binary);
            uLong crc = crc32(0, Z_NULL, 0);
            std::vector<char> buffer(io_chunk);
            while (file) {
                file.read(buffer.data(), buffer.size());
                crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer.data()), static_cast<uInt>(file.gcount()));
            }
            return static_cast<uint32_t>(crc);
        }

        // a package by its folder: installed when every listed file is there with its size and crc (extra files don't count),
        // missing when none is; the zip itself is never kept
        file_state package_state(const file_entry& file, const fs::path& folder) {
            if (file.entries.empty())
                return fs::exists(folder) ? file_state::outdated : file_state::missing;

            size_t present = 0;
            bool all_match = true;
            for (const auto& inner : file.entries) {
                fs::path path = folder / from_utf8(inner.path);
                std::error_code ec;
                uint64_t size = fs::file_size(path, ec);
                if (ec) {
                    all_match = false;
                    continue;
                }
                ++present;
                if (size != inner.size || file_crc(path) != inner.crc32)
                    all_match = false;
            }
            return all_match ? file_state::installed : present ? file_state::outdated : file_state::missing;
        }

        // unzips beside the folder, then swaps it in: a file taken out of the package goes away with the old folder
        bool extract_package(const fs::path& zip, const fs::path& folder, const std::string& name, std::string& error) {
            std::vector<uint8_t> data;
            std::vector<zip_item> items;
            if (!read_bytes(zip, data)) {
                error = "Couldn't read " + to_utf8(zip) + ".";
                return false;
            }
            if (!zip_items(data, items, error))
                return false;

            fs::path fresh = folder;
            fresh += ".new";
            fs::path old = folder;
            old += ".old";
            std::error_code ec;
            fs::remove_all(fresh, ec);
            fs::remove_all(old, ec);

            std::vector<uint8_t> bytes;
            for (const auto& item : items) {
                if (!zip_read(data, item, bytes, error))
                    return false;
                fs::path path = fresh / from_utf8(item.name);
                fs::create_directories(path.parent_path(), ec);
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!out.good()) {
                    error = "Couldn't write " + to_utf8(path) + ".";
                    return false;
                }
            }

            bool had_folder = fs::exists(folder);
            if (had_folder && !MoveFileExW(folder.c_str(), old.c_str(), 0)) {
                error = "Couldn't replace " + name + ": " + system_error(GetLastError()) + " Close the game and anything with its files open.";
                fs::remove_all(fresh, ec);
                return false;
            }
            if (!MoveFileExW(fresh.c_str(), folder.c_str(), 0)) {
                error = "Couldn't replace " + name + ": " + system_error(GetLastError());
                if (had_folder)
                    MoveFileExW(old.c_str(), folder.c_str(), 0);
                return false;
            }
            fs::remove_all(old, ec);
            return true;
        }

        /* progress, sent at most ~8 times a second with a speed over the last couple of seconds */

        class progress_meter {
        public:
            progress_meter(const progress_fn& callback, std::string phase, uint64_t total)
                : callback(callback), start(std::chrono::steady_clock::now()) {
                info.phase = std::move(phase);
                info.total = total;
                window_start = start;
            }

            void set_file(const std::string& file) {
                info.file = file;
                send(true);
            }

            void set_done(uint64_t done) {
                info.done = done;
                send(false);
            }

            void add(uint64_t bytes) {
                info.done += bytes;
                window_bytes += bytes;
                send(false);
            }

            uint64_t done() const { return info.done; }

            void send(bool now) {
                auto time = std::chrono::steady_clock::now();
                double window = std::chrono::duration<double>(time - window_start).count();
                if (window >= 2.0) {
                    info.speed = window_bytes / window;
                    window_bytes = 0;
                    window_start = time;
                }
                else if (info.speed == 0 && window >= 0.5) {
                    info.speed = window_bytes / window;
                }

                if (!now && time - last_sent < std::chrono::milliseconds(125))
                    return;
                last_sent = time;
                if (callback)
                    callback(info);
            }

        private:
            const progress_fn& callback;
            progress_info info;
            std::chrono::steady_clock::time_point start;
            std::chrono::steady_clock::time_point window_start;
            std::chrono::steady_clock::time_point last_sent{};
            uint64_t window_bytes = 0;
        };

        // waits seconds, false when cancelled meanwhile
        bool wait(int seconds, const std::atomic<bool>& cancel) {
            for (int i = 0; i < seconds * 10; ++i) {
                if (cancel)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return !cancel;
        }

        // one part into dest (resumed when a piece of it is there), checked against its hash, retried
        bool download_part(const std::string& base_url, const part& piece, const fs::path& dest, progress_meter& meter,
            const std::atomic<bool>& cancel, std::string& error) {
            const uint64_t done_before = meter.done();
            const std::string url = base_url + piece.sha256 + ".bin";

            for (int attempt = 0; attempt < max_attempts; ++attempt) {
                if (cancel) {
                    error = "Cancelled.";
                    return false;
                }
                if (attempt > 0 && !wait(1 << attempt, cancel)) {
                    error = "Cancelled.";
                    return false;
                }

                std::error_code ec;
                uint64_t have = fs::exists(dest, ec) ? fs::file_size(dest, ec) : 0;
                if (ec || have > piece.size) {
                    fs::remove(dest, ec);
                    have = 0;
                }

                if (have < piece.size) {
                    meter.set_done(done_before + have);
                    std::ofstream out;
                    std::string fetch_error;
                    int status = fetch(url, have, [&](int code) {
                        // a server that ignores the range sends it all again
                        bool restart = code == 200 && have > 0;
                        if (code == 200 || code == 206) {
                            out.open(dest, std::ios::binary | (restart ? std::ios::trunc : std::ios::app));
                            if (restart) {
                                have = 0;
                                meter.set_done(done_before);
                            }
                        }
                        return static_cast<bool>(out);
                    }, [&](const char* data, size_t size) {
                        out.write(data, static_cast<std::streamsize>(size));
                        meter.add(size);
                        return out.good() && !cancel;
                    }, fetch_error);
                    out.close();

                    if (cancel) {
                        error = "Cancelled.";
                        return false;
                    }
                    if (status != 200 && status != 206) {
                        error = !fetch_error.empty() ? fetch_error
                            : status == 404 ? "A file is missing on the server (" + piece.sha256.substr(0, 12) + ".bin)."
                            : "The server answered " + std::to_string(status) + ".";
                        logger::write(logger::LOG_LEVEL_WARN, logger::LOG_LABEL_UPDATE, "[CONTENT] %s attempt %d: %s", url.c_str(), attempt + 1, error.c_str());
                        continue;
                    }
                }

                have = fs::file_size(dest, ec);
                if (!ec && have == piece.size && hash_whole_file(dest) == piece.sha256) {
                    meter.set_done(done_before + piece.size);
                    error.clear();
                    return true;
                }

                // a short or corrupt part is fetched again from the start
                error = "A downloaded file didn't match its checksum.";
                logger::write(logger::LOG_LEVEL_WARN, logger::LOG_LABEL_UPDATE, "[CONTENT] %s attempt %d: bad size or hash", url.c_str(), attempt + 1);
                if (!ec && have >= piece.size)
                    fs::remove(dest, ec);
            }
            return false;
        }

        // where a needed part comes from: a downloaded .bin, or a range of the file that's already there
        struct part_source {
            fs::path path;
            uint64_t offset = 0;
        };

        // writes the parts into temp and checks the whole file's hash
        bool write_verified(const file_entry& file, const std::vector<part_source>& sources, const fs::path& temp, std::string& error) {
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                if (!out) {
                    error = "Couldn't write " + to_utf8(temp) + ".";
                    return false;
                }

                sha256 whole;
                std::vector<char> buffer(io_chunk);
                for (size_t i = 0; i < file.parts.size(); ++i) {
                    // an empty file's one part has nothing to read
                    if (file.parts[i].size == 0)
                        continue;
                    std::ifstream in(sources[i].path, std::ios::binary);
                    in.seekg(static_cast<std::streamoff>(sources[i].offset));
                    uint64_t left = file.parts[i].size;
                    while (left > 0 && in) {
                        in.read(buffer.data(), static_cast<std::streamsize>(std::min<uint64_t>(left, buffer.size())));
                        size_t got = static_cast<size_t>(in.gcount());
                        if (got == 0)
                            break;
                        out.write(buffer.data(), got);
                        whole.update(buffer.data(), got);
                        left -= got;
                    }
                    if (left > 0) {
                        error = "Couldn't read a piece of " + file.path + ".";
                        return false;
                    }
                }

                if (!out.good() || whole.finish() != file.sha256) {
                    error = file.path + " didn't match its checksum after downloading.";
                    return false;
                }
            }
            return true;
        }

        // the checked file in temp swapped in for dest
        bool assemble(const file_entry& file, const std::vector<part_source>& sources, const fs::path& temp, const fs::path& dest, std::string& error) {
            if (!write_verified(file, sources, temp, error))
                return false;

            std::error_code ec;
            fs::create_directories(dest.parent_path(), ec);
            if (!MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DWORD code = GetLastError();
                error = "Couldn't replace " + file.path + ": " + system_error(code);
                if (code == ERROR_ACCESS_DENIED)
                    error += " Run the launcher as administrator, or close whatever has the file open.";
                return false;
            }
            return true;
        }

        std::string format_size(uint64_t bytes) {
            char text[32];
            if (bytes >= (1ull << 30))
                sprintf_s(text, "%.2f GB", bytes / double(1ull << 30));
            else
                sprintf_s(text, "%.1f MB", bytes / double(1ull << 20));
            return text;
        }

        bool enough_space(const fs::path& dir, uint64_t needed, std::string& error) {
            ULARGE_INTEGER free_bytes = {};
            if (!GetDiskFreeSpaceExW(dir.c_str(), &free_bytes, nullptr, nullptr))
                return true;
            if (free_bytes.QuadPart >= needed)
                return true;
            error = "Not enough disk space: " + format_size(needed) + " needed, " + format_size(free_bytes.QuadPart) + " free.";
            return false;
        }

        fs::path launcher_exe() {
            wchar_t buffer[MAX_PATH];
            GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            return buffer;
        }
    }

    // kept in the exe as text, publish_content.py reads it to make sure the exe it publishes is the version it claims
    static const char version_marker[] = "SHIELD_LAUNCHER_VERSION=" LAUNCHER_VERSION;

    std::string launcher_version() {
        return version_marker + sizeof("SHIELD_LAUNCHER_VERSION=") - 1;
    }

    int compare_versions(const std::string& a, const std::string& b) {
        auto numbers = [](const std::string& text) {
            std::vector<uint64_t> result;
            static const std::regex digits(R"(\d+)");
            for (std::sregex_iterator it(text.begin(), text.end(), digits), end; it != end; ++it)
                result.push_back(std::stoull(it->str()));
            return result;
        };

        auto left = numbers(a);
        auto right = numbers(b);
        size_t count = std::max(left.size(), right.size());
        for (size_t i = 0; i < count; ++i) {
            uint64_t x = i < left.size() ? left[i] : 0;
            uint64_t y = i < right.size() ? right[i] : 0;
            if (x != y)
                return x < y ? -1 : 1;
        }
        return 0;
    }

    std::string manifest_location() {
        std::ifstream file(launcher::launcher_dir() / "content-url.txt");
        std::string location;
        if (file)
            std::getline(file, location);
        location = trim(location);
        if (location.empty())
            return default_manifest_url;

        // a local folder holds manifest.json beside its parts
        if (!is_remote(location) && fs::is_directory(from_utf8(location)))
            location = to_utf8(from_utf8(location) / "manifest.json");
        return location;
    }

    status check(bool full_verify, const progress_fn& progress, const std::atomic<bool>& cancel) {
        status result;

        fs::path game = launcher::game_dir();
        if (game.empty()) {
            result.error = "BlackOps4.exe not found! Put the launcher in the game folder (project-bo4/launcher).";
            return result;
        }

        std::string location = manifest_location();
        if (location == "none") {
            result.error = "Game file checks are off (launcher/content-url.txt says none).";
            return result;
        }

        std::string text;
        if (!fetch_text(location, text, result.error)) {
            logger::write(logger::LOG_LEVEL_ERROR, logger::LOG_LABEL_UPDATE, "[CONTENT] %s: %s", location.c_str(), result.error.c_str());
            return result;
        }

        bool parsed = parse_manifest(text, location, result.data, result.error);
        result.launcher_update = result.data.launcher_file && compare_versions(result.data.launcher_version, launcher_version()) > 0;
        if (!parsed)
            return result;

        hash_cache cache = load_cache();

        // what has to be hashed, for the progress
        uint64_t to_hash = 0;
        for (const auto& file : result.data.files) {
            uint64_t size = 0;
            int64_t time = 0;
            fs::path path = game / from_utf8(file.path);
            if (!file.extract.empty() || !stat_file(path, size, time))
                continue;
            auto cached = cache.find(file.path);
            if (full_verify || cached == cache.end() || cached->second.size != size || cached->second.time != time || cached->second.part_size != result.data.part_size)
                to_hash += size;
        }

        progress_meter meter(progress, "checking", to_hash);
        std::map<std::string, size_t> component_index;
        for (const auto& info : result.data.components) {
            component_index[info.id] = result.components.size();
            result.components.push_back({ info });
        }

        for (size_t i = 0; i < result.data.files.size(); ++i) {
            if (cancel) {
                result.error = "Cancelled.";
                return result;
            }

            const auto& file = result.data.files[i];
            fs::path path = game / from_utf8(file.path);
            meter.set_file(file.path);

            file_status state{ i, file_state::missing, file.size };
            local_hash local;
            if (!file.extract.empty()) {
                state.state = package_state(file, game / from_utf8(file.extract));
                state.download = state.state == file_state::installed ? 0 : file.size;
            }
            else if (fs::exists(path) && local_hash_of(file.path, path, result.data.part_size, cache, full_verify, local, [&](size_t bytes) {
                    meter.add(bytes);
                    return !cancel;
                })) {
                if (local.size == file.size && local.sha256 == file.sha256) {
                    state.state = file_state::installed;
                    state.download = 0;
                }
                else {
                    state.state = file_state::outdated;
                    state.download = download_size(file, local.part_size == result.data.part_size ? &local : nullptr);
                }
            }

            // a file of an unlisted component gets one of its own
            if (!component_index.count(file.component)) {
                component_index[file.component] = result.components.size();
                result.components.push_back({ component{ file.component, file.component.empty() ? "Other files" : file.component, "", true } });
            }

            auto& owner = result.components[component_index[file.component]];
            owner.files.push_back(state);
            owner.size += file.size;
            owner.download += state.download;
        }

        result.ready = true;
        for (auto& owner : result.components) {
            bool any_missing = false;
            bool any_outdated = false;
            for (const auto& state : owner.files) {
                any_missing |= state.state == file_state::missing;
                any_outdated |= state.state == file_state::outdated;
            }
            owner.state = any_missing ? file_state::missing : any_outdated ? file_state::outdated : file_state::installed;
            if (owner.info.required && owner.state != file_state::installed)
                result.ready = false;
            result.download += owner.download;
        }

        // only the manifest's files stay in the cache
        hash_cache kept;
        for (const auto& file : result.data.files) {
            auto it = cache.find(file.path);
            if (it != cache.end() && file.extract.empty())
                kept[file.path] = it->second;
        }
        save_cache(kept);

        result.ok = true;
        logger::write(logger::LOG_LEVEL_INFO, logger::LOG_LABEL_UPDATE, "[CONTENT] manifest %s, %zu files, %llu bytes to download, ready %d",
            result.data.version.c_str(), result.data.files.size(), result.download, result.ready ? 1 : 0);
        return result;
    }

    bool download_all(const status& current, const progress_fn& progress, const std::atomic<bool>& cancel, std::string& error) {
        fs::path game = launcher::game_dir();
        if (!current.ok || game.empty()) {
            error = current.error.empty() ? "Check for game files first." : current.error;
            return false;
        }

        const manifest& data = current.data;
        hash_cache cache = load_cache();
        fs::path temp = game / "project-bo4" / "temp" / "launcher_dl";
        std::error_code ec;
        fs::create_directories(temp / "files", ec);

        // per file the source of each part, and the parts that have to be downloaded
        struct plan_entry {
            const file_entry* file;
            std::vector<part_source> sources;
        };

        std::vector<plan_entry> plan;
        std::map<std::string, part> needed;
        uint64_t largest = 0;

        for (const auto& owner : current.components) {
            for (const auto& state : owner.files) {
                if (state.state == file_state::installed)
                    continue;

                const auto& file = data.files[state.index];
                fs::path dest = game / from_utf8(file.path);

                // offsets of the local file's parts by hash, when the cache still describes it
                std::map<std::string, uint64_t> local_parts;
                uint64_t size = 0;
                int64_t time = 0;
                auto cached = cache.find(file.path);
                if (file.extract.empty() && cached != cache.end() && cached->second.part_size == data.part_size && stat_file(dest, size, time) &&
                    cached->second.size == size && cached->second.time == time) {
                    for (size_t i = 0; i < cached->second.parts.size(); ++i)
                        local_parts.emplace(cached->second.parts[i], i * data.part_size);
                }

                plan_entry entry{ &file };
                for (const auto& piece : file.parts) {
                    if (piece.size == 0) {
                        entry.sources.push_back({});
                        continue;
                    }
                    auto local = local_parts.find(piece.sha256);
                    if (local != local_parts.end()) {
                        entry.sources.push_back({ dest, local->second });
                    }
                    else {
                        entry.sources.push_back({ temp / (piece.sha256 + ".bin"), 0 });
                        needed[piece.sha256] = piece;
                    }
                }
                largest = std::max(largest, file.size);
                plan.push_back(std::move(entry));
            }
        }

        uint64_t total = 0;
        for (const auto& [sha, piece] : needed)
            total += piece.size;

        // the parts, plus the biggest file while it's put together
        if (!enough_space(temp, total + largest, error))
            return false;

        logger::write(logger::LOG_LEVEL_INFO, logger::LOG_LABEL_UPDATE, "[CONTENT] downloading %zu parts, %llu bytes, for %zu files",
            needed.size(), total, plan.size());

        progress_meter meter(progress, "downloading", total);
        for (const auto& entry : plan) {
            for (const auto& piece : entry.file->parts) {
                auto it = needed.find(piece.sha256);
                if (it == needed.end())
                    continue;
                meter.set_file(entry.file->path);
                if (!download_part(data.base_url, piece, temp / (piece.sha256 + ".bin"), meter, cancel, error))
                    return false;
                needed.erase(it);
            }
        }

        if (DllLoading::isGameRunning()) {
            error = "Close Black Ops 4 to install the update. The download is kept.";
            return false;
        }

        progress_meter installing(progress, "installing", plan.size());
        for (size_t i = 0; i < plan.size(); ++i) {
            const auto& file = *plan[i].file;
            installing.set_done(i);
            installing.set_file(file.path);

            fs::path built = temp / "files" / (std::to_string(i) + ".new");

            // a package: the zip is put together in temp, unzipped into its folder, then dropped
            if (!file.extract.empty()) {
                installing.set_file(file.extract);
                bool ok = write_verified(file, plan[i].sources, built, error) &&
                    extract_package(built, game / from_utf8(file.extract), file.extract, error);
                fs::remove(built, ec);
                if (!ok) {
                    save_cache(cache);
                    return false;
                }
                continue;
            }

            fs::path dest = game / from_utf8(file.path);
            if (!assemble(file, plan[i].sources, built, dest, error)) {
                save_cache(cache);
                return false;
            }

            // the new file's hashes are known, no need to read it again
            local_hash entry;
            stat_file(dest, entry.size, entry.time);
            entry.sha256 = file.sha256;
            entry.part_size = data.part_size;
            for (const auto& piece : file.parts)
                entry.parts.push_back(piece.sha256);
            cache[file.path] = entry;
        }

        for (const auto& path : data.remove) {
            fs::remove(game / from_utf8(path), ec);
            cache.erase(path);
        }

        save_cache(cache);
        fs::remove_all(temp, ec);
        installing.set_done(plan.size());
        logger::write(logger::LOG_LEVEL_INFO, logger::LOG_LABEL_UPDATE, "[CONTENT] installed %zu files", plan.size());
        return true;
    }

    bool update_launcher(const status& current, const progress_fn& progress, const std::atomic<bool>& cancel, std::string& error) {
        if (!current.launcher_update || !current.data.launcher_file) {
            error = "No launcher update.";
            return false;
        }

        const file_entry& file = *current.data.launcher_file;
        fs::path exe = launcher_exe();
        fs::path temp = exe.parent_path() / "launcher_dl";
        std::error_code ec;
        fs::create_directories(temp, ec);

        progress_meter meter(progress, "launcher", file.size);
        meter.set_file("Shield_Launcher.exe");
        std::vector<part_source> sources;
        for (const auto& piece : file.parts) {
            fs::path dest = temp / (piece.sha256 + ".bin");
            if (!download_part(current.data.base_url, piece, dest, meter, cancel, error))
                return false;
            sources.push_back({ dest, 0 });
        }

        fs::path fresh = exe;
        fresh += ".new";
        fs::path old = exe;
        old += ".old";

        // windows lets a running exe be renamed, so the old one steps aside for the new one
        fs::remove(old, ec);
        file_entry named = file;
        named.path = "Shield_Launcher.exe";
        if (!write_verified(named, sources, fresh, error))
            return false;

        if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = "Couldn't move the old launcher aside: " + system_error(GetLastError());
            fs::remove(fresh, ec);
            return false;
        }
        if (!MoveFileExW(fresh.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = "Couldn't put the new launcher in place: " + system_error(GetLastError());
            MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
            return false;
        }

        fs::remove_all(temp, ec);
        logger::write(logger::LOG_LEVEL_INFO, logger::LOG_LABEL_UPDATE, "[CONTENT] launcher updated to %s", current.data.launcher_version.c_str());
        return true;
    }

    void cleanup_old_launcher() {
        fs::path old = launcher_exe();
        old += ".old";
        std::error_code ec;
        if (!fs::exists(old) || fs::remove(old, ec))
            return;

        // the previous launcher may still be closing, so this tries for a while
        std::thread([old] {
            for (int i = 0; i < 20; ++i) {
                std::error_code ec;
                if (fs::remove(old, ec) || !fs::exists(old))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }).detach();
    }

    bool restart_launcher() {
        fs::path exe = launcher_exe();
        std::wstring command = L"\"" + exe.wstring() + L"\" --updated";
        STARTUPINFOW startup = { sizeof(startup) };
        PROCESS_INFORMATION process = {};
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd, &startup, &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }
}
