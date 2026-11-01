#include "std_include.hpp"
#include "json_utils.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/filewritestream.h>

namespace fs = std::filesystem;

namespace {
    // fopen_s() only exists on MSVC
    inline FILE* openFile(const char* path, const char* mode) {
#ifdef _WIN32
        FILE* fp = nullptr;
        fopen_s(&fp, path, mode);
        return fp;
#else
        return std::fopen(path, mode);
#endif
    }

    std::string currentUserName() {
#ifdef _WIN32
        char username[256] = {0};
        DWORD size = sizeof(username);
        GetUserNameA(username, &size);
        return std::string(username);
#else
        const char* user = std::getenv("USER");
        return std::string(user ? user : "Player");
#endif
    }
}

namespace JsonUtils {
    void createDefaultJson(const std::string& jsonPath) {
        fs::path path(jsonPath);
        if (!fs::exists(path.parent_path())) {
            fs::create_directories(path.parent_path());
        }

        rapidjson::Document doc;
        doc.SetObject();
        auto& allocator = doc.GetAllocator();

        const std::string username = currentUserName();

        rapidjson::Value demonware(rapidjson::kObjectType);
        demonware.AddMember("ipv4", "78.157.42.107", allocator);
        doc.AddMember("demonware", demonware, allocator);

        rapidjson::Value identity(rapidjson::kObjectType);
        identity.AddMember("name", rapidjson::StringRef(username.c_str()), allocator);
        doc.AddMember("identity", identity, allocator);

        FILE* fp = openFile(jsonPath.c_str(), "wb");
        if (fp) {
            char writeBuffer[65536];
            rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
            rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
            doc.Accept(writer);
            fclose(fp);
        }
    }

    std::string getJsonItem(const std::string& jsonPath, const std::string& spot, const std::string& name) {
        if (!fs::exists(jsonPath)) {
            createDefaultJson(jsonPath);
        }

        FILE* fp = openFile(jsonPath.c_str(), "rb");
        if (!fp) return "";

        char readBuffer[65536];
        rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
        rapidjson::Document doc;
        doc.ParseStream(is);
        fclose(fp);

        if (doc.HasParseError() || !doc.IsObject()) return "";

        if (!doc.HasMember(spot.c_str()) || !doc[spot.c_str()].IsObject()) return "";

        const auto& spotObj = doc[spot.c_str()];
        if (!spotObj.HasMember(name.c_str()) || !spotObj[name.c_str()].IsString()) return "";

        return spotObj[name.c_str()].GetString();
    }

    bool replaceJsonValue(const std::string& jsonPath, const std::string& value,
                         const std::string& spot, const std::string& key) {
        if (!fs::exists(jsonPath)) {
            createDefaultJson(jsonPath);
        }

        FILE* fp = openFile(jsonPath.c_str(), "rb");
        if (!fp) return false;

        char readBuffer[65536];
        rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
        rapidjson::Document doc;
        doc.ParseStream(is);
        fclose(fp);

        if (doc.HasParseError() || !doc.IsObject()) return false;

        if (!doc.HasMember(spot.c_str())) {
            doc.AddMember(
                rapidjson::Value(spot.c_str(), doc.GetAllocator()).Move(),
                rapidjson::Value(rapidjson::kObjectType),
                doc.GetAllocator()
            );
        }

        auto& spotObj = doc[spot.c_str()];
        if (!spotObj.IsObject()) return false;

        if (spotObj.HasMember(key.c_str())) {
            spotObj[key.c_str()].SetString(value.c_str(), doc.GetAllocator());
        } else {
            spotObj.AddMember(
                rapidjson::Value(key.c_str(), doc.GetAllocator()).Move(),
                rapidjson::Value(value.c_str(), doc.GetAllocator()).Move(),
                doc.GetAllocator()
            );
        }

        fp = openFile(jsonPath.c_str(), "wb");
        if (!fp) return false;

        char writeBuffer[65536];
        rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
        rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
        doc.Accept(writer);
        fclose(fp);

        return true;
    }
}