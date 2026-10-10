#include "diagnostics_report.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>

namespace pulse::diagnostics {
namespace {
namespace fs = std::filesystem;
void Put(std::ostream& s, uint32_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) s.put(static_cast<char>(n >> (8 * i)));
}
uint32_t Crc(const std::vector<char>& data) {
    uint32_t crc = ~0u;
    for (unsigned char c : data) {
        crc ^= c;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
struct Entry { std::string name; uint32_t crc, size, offset; };
}
bool CreateSupportArchive(const std::wstring& directory, const std::wstring& path, std::wstring* error) {
    bool created = false;
    try {
        const fs::path root = fs::absolute(directory).lexically_normal(), output = fs::absolute(path).lexically_normal();
        if (output.parent_path() != root || fs::exists(output)) throw 1;
        const DWORD root_attrs = GetFileAttributesW(root.c_str());
        if (root_attrs == INVALID_FILE_ATTRIBUTES || (root_attrs & FILE_ATTRIBUTE_REPARSE_POINT)) throw 1;
        std::vector<fs::path> files;
        for (const auto& item : fs::recursive_directory_iterator(root)) {
            const DWORD attrs = GetFileAttributesW(item.path().c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) throw 1;
            if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(item.path());
        }
        if (files.empty() || files.size() > 1024) throw 1;
        std::sort(files.begin(), files.end());
        HANDLE reserve = CreateFileW(output.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (reserve == INVALID_HANDLE_VALUE) throw 1;
        CloseHandle(reserve); created = true;
        std::ofstream zip(output, std::ios::binary | std::ios::trunc);
        if (!zip) throw 1;
        std::vector<Entry> entries; uint64_t total = 0;
        for (const auto& file : files) {
            const auto bytes = fs::file_size(file);
            if (bytes > 64 * 1024 * 1024 || total + bytes > 256 * 1024 * 1024) throw 1;
            total += bytes;
            const auto utf8 = file.lexically_relative(root).generic_u8string();
            std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            if (name.empty() || name.size() > 65535 || name.find("..") != name.npos) throw 1;
            HANDLE input = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (input == INVALID_HANDLE_VALUE) throw 1;
            BY_HANDLE_FILE_INFORMATION info{}; DWORD read = 0; std::vector<char> data(static_cast<size_t>(bytes));
            const bool valid = GetFileInformationByHandle(input, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                ReadFile(input, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) && read == data.size();
            CloseHandle(input); if (!valid) throw 1;
            Entry entry{name, Crc(data), static_cast<uint32_t>(bytes), static_cast<uint32_t>(zip.tellp())};
            Put(zip, 0x04034b50, 4); Put(zip, 20, 2); Put(zip, 0x800, 2); Put(zip, 0, 2);
            Put(zip, 0, 2); Put(zip, 33, 2); Put(zip, entry.crc, 4); Put(zip, entry.size, 4); Put(zip, entry.size, 4);
            Put(zip, static_cast<uint32_t>(name.size()), 2); Put(zip, 0, 2); zip.write(name.data(), name.size()); zip.write(data.data(), data.size());
            entries.push_back(entry); if (!zip) throw 1;
        }
        const auto central = static_cast<uint32_t>(zip.tellp());
        for (const auto& e : entries) {
            Put(zip, 0x02014b50, 4); Put(zip, 20, 2); Put(zip, 20, 2); Put(zip, 0x800, 2); Put(zip, 0, 2);
            Put(zip, 0, 2); Put(zip, 33, 2); Put(zip, e.crc, 4); Put(zip, e.size, 4); Put(zip, e.size, 4);
            Put(zip, static_cast<uint32_t>(e.name.size()), 2); Put(zip, 0, 2); Put(zip, 0, 2); Put(zip, 0, 2); Put(zip, 0, 2); Put(zip, 0, 4); Put(zip, e.offset, 4);
            zip.write(e.name.data(), e.name.size());
        }
        const auto end = static_cast<uint32_t>(zip.tellp());
        Put(zip, 0x06054b50, 4); Put(zip, 0, 2); Put(zip, 0, 2); Put(zip, static_cast<uint32_t>(entries.size()), 2); Put(zip, static_cast<uint32_t>(entries.size()), 2);
        Put(zip, end - central, 4); Put(zip, central, 4); Put(zip, 0, 2); zip.close();
        if (!zip) throw 1;
        return true;
    } catch (...) {
        if (created) DeleteFileW(path.c_str());
        if (error) *error = L"Could not create diagnostics ZIP; the exported folder is still available.";
        return false;
    }
}
}
