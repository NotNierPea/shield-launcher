#include "std_include.hpp"
#include "zip_archive.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <zlib.h>

namespace fs = std::filesystem;

namespace {
    uint16_t readU16(const unsigned char* p) {
        return static_cast<uint16_t>(p[0] | (p[1] << 8));
    }

    uint32_t readU32(const unsigned char* p) {
        return static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) |
               (static_cast<uint32_t>(p[3]) << 24);
    }

    // Locates the end of central directory record.
    bool findEndOfCentralDirectory(std::ifstream& file, uint32_t& centralDirOffset, uint16_t& entryCount) {
        file.seekg(0, std::ios::end);
        const auto fileSize = static_cast<std::streamoff>(file.tellg());
        if (fileSize < 22) {
            return false;
        }

        const std::streamoff window = std::min<std::streamoff>(fileSize, 22 + 65536);
        std::vector<unsigned char> tail(static_cast<size_t>(window));
        file.seekg(fileSize - window);
        file.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
        if (file.gcount() != static_cast<std::streamsize>(tail.size())) {
            return false;
        }

        for (std::streamoff i = static_cast<std::streamoff>(tail.size()) - 22; i >= 0; --i) {
            const auto* p = tail.data() + i;
            if (p[0] == 0x50 && p[1] == 0x4b && p[2] == 0x05 && p[3] == 0x06) {
                entryCount = readU16(p + 10);
                centralDirOffset = readU32(p + 16);
                return true;
            }
        }

        return false;
    }
}

namespace {
    bool readEntryData(std::ifstream& file, const zip::Entry& entry, std::vector<unsigned char>& data) {
        // Local header: 30 bytes then the file name and extra field
        file.seekg(entry.localHeaderOffset);
        unsigned char header[30];
        file.read(reinterpret_cast<char*>(header), sizeof(header));
        if (file.gcount() != static_cast<std::streamsize>(sizeof(header))) {
            return false;
        }

        if (!(header[0] == 0x50 && header[1] == 0x4b && header[2] == 0x03 && header[3] == 0x04)) {
            return false;
        }

        const uint16_t nameLen = readU16(header + 26);
        const uint16_t extraLen = readU16(header + 28);
        file.seekg(static_cast<std::streamoff>(nameLen) + extraLen, std::ios::cur);

        std::vector<unsigned char> compressed(entry.compressedSize);
        if (entry.compressedSize > 0) {
            file.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
            if (file.gcount() != static_cast<std::streamsize>(compressed.size())) {
                return false;
            }
        }

        if (entry.method == 0) {
            data = std::move(compressed);
        } else if (entry.method == 8) {
            data.resize(entry.uncompressedSize);
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
                return false;
            }

            stream.next_in = compressed.data();
            stream.avail_in = static_cast<uInt>(compressed.size());
            stream.next_out = data.data();
            stream.avail_out = static_cast<uInt>(data.size());

            const int result = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);

            if (result != Z_STREAM_END) {
                return false;
            }

            data.resize(stream.total_out);
        } else {
            return false;
        }

        const uLong crc = crc32(crc32(0L, Z_NULL, 0), data.data(), static_cast<uInt>(data.size()));
        return static_cast<uint32_t>(crc) == entry.crc32;
    }

    bool writeFile(const std::string& path, const std::vector<unsigned char>& data) {
        try {
            fs::path destination(path);
            if (destination.has_parent_path()) {
                fs::create_directories(destination.parent_path());
            }

            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) {
                return false;
            }

            if (!data.empty()) {
                output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            }

            return output.good();
        } catch (const std::exception&) {
            return false;
        }
    }
}

namespace zip {
    bool list(const std::string& zipPath, std::vector<Entry>& out) {
        out.clear();

        std::ifstream file(zipPath, std::ios::binary);
        if (!file) {
            return false;
        }

        uint32_t centralDirOffset = 0;
        uint16_t entryCount = 0;
        if (!findEndOfCentralDirectory(file, centralDirOffset, entryCount)) {
            return false;
        }

        file.seekg(centralDirOffset);
        if (!file) {
            return false;
        }

        for (uint16_t index = 0; index < entryCount; ++index) {
            unsigned char header[46];
            file.read(reinterpret_cast<char*>(header), sizeof(header));
            if (file.gcount() != static_cast<std::streamsize>(sizeof(header))) {
                return false;
            }

            if (!(header[0] == 0x50 && header[1] == 0x4b && header[2] == 0x01 && header[3] == 0x02)) {
                return false;
            }

            Entry entry;
            entry.method = readU16(header + 10);
            entry.crc32 = readU32(header + 16);
            entry.compressedSize = readU32(header + 20);
            entry.uncompressedSize = readU32(header + 24);

            const uint16_t nameLen = readU16(header + 28);
            const uint16_t extraLen = readU16(header + 30);
            const uint16_t commentLen = readU16(header + 32);
            entry.localHeaderOffset = readU32(header + 42);

            entry.name.assign(nameLen, '\0');
            file.read(entry.name.data(), nameLen);
            if (file.gcount() != static_cast<std::streamsize>(nameLen)) {
                return false;
            }

            file.seekg(extraLen + commentLen, std::ios::cur);

            out.push_back(std::move(entry));
        }

        return true;
    }

    bool findEntry(const std::string& zipPath, const std::string& entryName, Entry& out) {
        std::vector<Entry> entries;
        if (!list(zipPath, entries)) {
            return false;
        }

        for (const auto& entry : entries) {
            if (entry.name == entryName) {
                out = entry;
                return true;
            }
        }

        return false;
    }

    bool extractEntry(const std::string& zipPath, const Entry& entry, const std::string& destPath) {
        std::ifstream file(zipPath, std::ios::binary);
        if (!file) {
            return false;
        }

        std::vector<unsigned char> data;
        if (!readEntryData(file, entry, data)) {
            return false;
        }

        return writeFile(destPath, data);
    }

    bool extractEntry(const std::string& zipPath, const std::string& entryName, const std::string& destPath) {
        Entry entry;
        if (!findEntry(zipPath, entryName, entry)) {
            return false;
        }

        return extractEntry(zipPath, entry, destPath);
    }

    bool extractAll(const std::string& zipPath, const std::string& destDir) {
        std::vector<Entry> entries;
        if (!list(zipPath, entries)) {
            return false;
        }

        std::ifstream file(zipPath, std::ios::binary);
        if (!file) {
            return false;
        }

        bool ok = true;

        for (const auto& entry : entries) {
            // directory entries end with a slash and carry no data
            if (!entry.name.empty() && entry.name.back() == '/') {
                continue;
            }

            // never let an archive escape the destination directory
            if (entry.name.find("..") != std::string::npos ||
                entry.name.find(':') != std::string::npos ||
                (!entry.name.empty() && (entry.name[0] == '/' || entry.name[0] == '\\'))) {
                ok = false;
                continue;
            }

            std::vector<unsigned char> data;
            if (!readEntryData(file, entry, data)) {
                ok = false;
                continue;
            }

            const fs::path destination = fs::path(destDir) / entry.name;
            if (!writeFile(destination.string(), data)) {
                ok = false;
            }
        }

        return ok;
    }
}
