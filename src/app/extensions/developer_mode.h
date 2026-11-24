#pragma once
// Developer mode (devmode_plan.md M1/M2). Header-only so every target that already
// links package_store / install_service sees it without new CMake sources.
//
// Layout, a sibling of the store root (never inside it, never the signature policy):
//   <store>/../extensions-dev/developer-mode.json  {"formatVersion":1,"enabled":true}
//   <store>/../extensions-dev/dev-packages.json    {"formatVersion":1,"packages":[{id,source,revision}]}
//   <store>/../extensions-dev/<id>/<revision>/     shadow copy: manifest.json + native files
#include "../../common/json_value.h"
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace pulse::extensions::devmode {
inline constexpr size_t kMaxDevPackages = 8;
inline constexpr size_t kMaxDevFileBytes = 64 * 1024;

inline std::filesystem::path DeveloperRoot(const std::filesystem::path& store_root) {
    if (store_root.empty()) return {};
    return store_root.lexically_normal().parent_path() / L"extensions-dev";
}
inline bool ReadSmallFile(const std::filesystem::path& path, std::string& bytes) {
    bytes.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    bool ok = GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= static_cast<LONGLONG>(kMaxDevFileBytes);
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    }
    CloseHandle(file);
    if (!ok) bytes.clear();
    return ok;
}
// Off unless the file exists, parses, and says enabled:true. Any doubt means off.
inline bool Enabled(const std::filesystem::path& store_root) {
    try {
        const auto root = DeveloperRoot(store_root);
        if (root.empty()) return false;
        std::string bytes;
        if (!ReadSmallFile(root / L"developer-mode.json", bytes)) return false;
        json::Value value;
        if (!json::Parse(bytes, value, nullptr, {kMaxDevFileBytes, 3}) || !value.is_object()) return false;
        int64_t version = 0;
        const auto* format = value.find("formatVersion");
        const auto* enabled = value.find("enabled");
        return format && format->get_int(version, 1, 1) && enabled && enabled->is_bool() && enabled->as_bool();
    } catch (...) { return false; }
}

struct DevPackageEntry {
    std::string id;
    std::wstring source;   // the developer's folder (re-copied by 重新加载)
    std::string revision;  // SHA-256 of the shadow manifest bytes
};
inline bool HexRevision(const std::string& value) {
    if (value.size() != 64) return false;
    for (const char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
// Missing file = no packages. Malformed entries are dropped (the UI shows what is left).
inline std::vector<DevPackageEntry> ReadRegistry(const std::filesystem::path& store_root) {
    std::vector<DevPackageEntry> out;
    try {
        const auto root = DeveloperRoot(store_root);
        if (root.empty()) return out;
        std::string bytes;
        if (!ReadSmallFile(root / L"dev-packages.json", bytes)) return out;
        json::Value value;
        if (!json::Parse(bytes, value, nullptr, {kMaxDevFileBytes, 4}) || !value.is_object()) return out;
        const auto* packages = value.find("packages");
        if (!packages || !packages->is_array()) return out;
        for (const auto& item : packages->items()) {
            if (!item.is_object() || out.size() >= kMaxDevPackages) continue;
            DevPackageEntry entry{item.get_string("id"), json::Wide(item.get_string("source")), item.get_string("revision")};
            if (entry.id.empty() || entry.id.size() > 64 || !HexRevision(entry.revision)) continue;
            bool duplicate = false;
            for (const auto& existing : out) duplicate |= existing.id == entry.id;
            if (!duplicate) out.push_back(std::move(entry));
        }
    } catch (...) { out.clear(); }
    return out;
}
inline std::filesystem::path ShadowDirectory(const std::filesystem::path& store_root, const DevPackageEntry& entry) {
    return DeveloperRoot(store_root) / json::Wide(entry.id) / json::Wide(entry.revision);
}
}
