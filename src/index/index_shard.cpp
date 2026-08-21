#include "index_shard.h"
#include "../common/json_utils.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <windows.h>
#include <vector>

namespace pulse::index {
namespace {
void SetError(std::wstring* error, const wchar_t* text) { if (error) *error = text; }
bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}
std::wstring Hex(uint64_t value) {
    wchar_t text[32]{};
    swprintf_s(text, L"%016I64X", value);
    return text;
}
std::wstring ManifestJson(const ShardManifest& v) {
    std::wstring id;
    pulse::json::Escape(v.source_id, id);
    return L"{\n  \"version\":" + std::to_wstring(v.version) +
           L",\n  \"generation\":" + std::to_wstring(v.generation) +
           L",\n  \"active_built\":" + std::to_wstring(v.active_built) +
           L",\n  \"active_wal_bytes\":" + std::to_wstring(v.active_wal_bytes) +
           L",\n  \"active_slot\":" + std::to_wstring(v.active_slot) +
           L",\n  \"previous_slot\":" + std::to_wstring(v.previous_slot) +
           L",\n  \"active_bytes\":" + std::to_wstring(v.active_bytes) +
           L",\n  \"active_crc64\":" + std::to_wstring(v.active_crc64) +
           L",\n  \"source_id\":\"" + id + L"\"\n}\n";
}

uint64_t ReadU64(const std::wstring& text, const wchar_t* key) {
    size_t pos = pulse::json::ValuePosition(text, key);
    if (pos == std::wstring::npos || pos >= text.size() || text[pos] < L'0' || text[pos] > L'9')
        return 0;
    uint64_t value = 0;
    while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') {
        const uint64_t digit = static_cast<uint64_t>(text[pos] - L'0');
        if (value > (UINT64_MAX - digit) / 10) return UINT64_MAX;
        value = value * 10 + digit;
        ++pos;
    }
    return value;
}

bool ReadFilePrefix(const std::wstring& path, char* magic, uint32_t& version,
                    uint64_t& built) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BYTE header[24]{};
    DWORD got = 0;
    const bool ok = ReadFile(h, header, sizeof(header), &got, nullptr) && got >= sizeof(header);
    CloseHandle(h);
    if (!ok) return false;
    memcpy(magic, header, 4);
    memcpy(&version, header + 4, sizeof(version));
    memcpy(&built, header + 16, sizeof(built));
    return true;
}

std::wstring SlotPath(const ShardPaths& paths, uint8_t slot) {
    return slot == 0 ? paths.base_a : paths.base_b;
}

bool WriteThroughFile(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const size_t bytes = text.size() * sizeof(wchar_t);
    DWORD written = 0;
    const bool ok = bytes <= 0xffffffffu &&
        WriteFile(h, text.data(), static_cast<DWORD>(bytes), &written, nullptr) &&
        written == bytes && FlushFileBuffers(h) != FALSE;
    CloseHandle(h);
    return ok;
}
}

uint64_t ShardIdHash(const std::wstring& source_id) {
    uint64_t hash = 14695981039346656037ull;
    for (wchar_t c : source_id) {
        hash ^= static_cast<uint16_t>(towupper(c));
        hash *= 1099511628211ull;
    }
    return hash;
}

ShardPaths MakeShardPaths(const std::wstring& root, const std::wstring& source_id) {
    ShardPaths paths;
    if (root.empty() || source_id.empty()) return paths;
    paths.directory = root + L"\\" + Hex(ShardIdHash(source_id));
    EnsureDirectory(root);
    EnsureDirectory(paths.directory);
    paths.manifest = paths.directory + L"\\manifest.json";
    paths.base_a = paths.directory + L"\\base-a.bin";
    paths.base_b = paths.directory + L"\\base-b.bin";
    paths.wal_a = paths.directory + L"\\wal-a.log";
    paths.wal_b = paths.directory + L"\\wal-b.log";
    return paths;
}

bool LoadShardManifest(const std::wstring& path, ShardManifest& out, std::wstring* error) {
    out = {};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND) return true;
        SetError(error, L"无法读取分片清单");
        return false;
    }
    LARGE_INTEGER size{};
    std::wstring json;
    DWORD bytes = 0;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 &&
        size.QuadPart <= static_cast<LONGLONG>(64 * 1024) &&
        (size.QuadPart % sizeof(wchar_t)) == 0) {
        json.resize(static_cast<size_t>(size.QuadPart / sizeof(wchar_t)));
        if (!ReadFile(h, json.data(), static_cast<DWORD>(size.QuadPart), &bytes, nullptr) ||
            bytes != static_cast<DWORD>(size.QuadPart)) json.clear();
    }
    CloseHandle(h);
    if (json.find(L'{') == std::wstring::npos) { SetError(error, L"分片清单损坏"); return false; }
    out.version = static_cast<uint32_t>(std::max<int64_t>(1, pulse::json::ExtractInt(json, L"version", 1)));
    out.generation = ReadU64(json, L"generation");
    out.active_built = ReadU64(json, L"active_built");
    out.active_wal_bytes = ReadU64(json, L"active_wal_bytes");
    out.active_slot = static_cast<uint8_t>(std::clamp<int64_t>(pulse::json::ExtractInt(json, L"active_slot", 0), 0, 1));
    out.previous_slot = static_cast<uint8_t>(std::clamp<int64_t>(pulse::json::ExtractInt(json, L"previous_slot", 1), 0, 1));
    out.active_bytes = ReadU64(json, L"active_bytes");
    out.active_crc64 = ReadU64(json, L"active_crc64");
    out.source_id = pulse::json::ExtractString(json, L"source_id", L"");
    return true;
}

bool SaveShardManifest(const std::wstring& path, const ShardManifest& value, std::wstring* error) {
    if (path.empty() || value.source_id.empty()) { SetError(error, L"分片清单参数无效"); return false; }
    const std::wstring temp = path + L".tmp";
    if (!WriteThroughFile(temp, ManifestJson(value))) {
        SetError(error, L"无法创建分片清单临时文件"); return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str()); SetError(error, L"无法发布分片清单"); return false;
    }
    return true;
}

uint64_t FileCrc64(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t hash = 14695981039346656037ull;
    std::vector<BYTE> buffer(1u << 20);
    DWORD got = 0;
    while (ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) && got) {
        for (DWORD i = 0; i < got; ++i) {
            hash ^= buffer[i];
            hash *= 1099511628211ull;
        }
    }
    const bool eof = GetLastError() == ERROR_HANDLE_EOF || got == 0;
    CloseHandle(h);
    return eof ? hash : 0;
}

bool ValidateShardBase(const std::wstring& path, uint64_t expected_bytes,
                       uint64_t expected_crc64) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    char magic[4]{};
    uint32_t version = 0;
    uint64_t built = 0;
    const bool size_ok = GetFileSizeEx(h, &size) && size.QuadPart >= 128;
    CloseHandle(h);
    if (!size_ok || size.QuadPart > 0x7fffffffffffffffll) return false;
    if (!ReadFilePrefix(path, magic, version, built) || memcmp(magic, "PIDX", 4) != 0 || version < 7 || version > 9)
        return false;
    if (expected_bytes && static_cast<uint64_t>(size.QuadPart) != expected_bytes) return false;
    if (expected_crc64 && FileCrc64(path) != expected_crc64) return false;
    return built != 0 || version >= 9;
}

bool ResolveActiveShard(const ShardPaths& paths, ShardManifest& manifest,
                        std::wstring& active_path, std::wstring* error) {
    ShardManifest loaded;
    const bool manifest_ok = LoadShardManifest(paths.manifest, loaded, error);
    if (manifest_ok && !loaded.source_id.empty()) {
        const std::wstring candidate = SlotPath(paths, loaded.active_slot);
        if (ValidateShardBase(candidate, loaded.active_bytes, loaded.active_crc64)) {
            manifest = loaded;
            active_path = candidate;
            return true;
        }
        const std::wstring fallback = SlotPath(paths, loaded.previous_slot);
        if (loaded.previous_slot != loaded.active_slot && ValidateShardBase(fallback)) {
            char magic[4]{};
            uint32_t version = 0;
            uint64_t fallback_built = 0;
            if (!ReadFilePrefix(fallback, magic, version, fallback_built)) return false;
            loaded.active_slot = loaded.previous_slot;
            loaded.active_built = fallback_built;
            HANDLE h = CreateFileW(fallback.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            LARGE_INTEGER size{};
            loaded.active_bytes = h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &size)
                ? static_cast<uint64_t>(size.QuadPart) : 0;
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            loaded.active_crc64 = FileCrc64(fallback);
            loaded.previous_slot = static_cast<uint8_t>(1 - loaded.active_slot);
            loaded.generation = loaded.generation ? loaded.generation - 1 : 0;
            SaveShardManifest(paths.manifest, loaded, nullptr);
            manifest = loaded;
            active_path = fallback;
            return true;
        }
    }

    uint64_t best_built = 0;
    uint8_t best_slot = 0;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        const std::wstring candidate = SlotPath(paths, slot);
        if (!ValidateShardBase(candidate)) continue;
        char magic[4]{};
        uint32_t version = 0;
        uint64_t built = 0;
        if (ReadFilePrefix(candidate, magic, version, built) && built >= best_built) {
            best_built = built;
            best_slot = slot;
        }
    }
    if (!best_built) {
        if (error && error->empty()) *error = L"没有可用的 V9 分片";
        return false;
    }
    manifest = {};
    manifest.version = 9;
    manifest.generation = 1;
    manifest.active_built = best_built;
    manifest.active_slot = best_slot;
    manifest.previous_slot = static_cast<uint8_t>(1 - best_slot);
    manifest.source_id = L"unknown";
    active_path = SlotPath(paths, best_slot);
    return true;
}

bool PublishShardBase(const ShardPaths& paths, const std::wstring& temp_path,
                      uint64_t built_unix, uint64_t wal_bytes,
                      ShardManifest& published, std::wstring* error,
                      const std::wstring& source_id) {
    if (!ValidateShardBase(temp_path)) {
        SetError(error, L"V9 base 校验失败");
        return false;
    }
    ShardManifest current;
    std::wstring active;
    ResolveActiveShard(paths, current, active, nullptr);
    const uint8_t target = current.source_id.empty() ? 0 : static_cast<uint8_t>(1 - current.active_slot);
    const std::wstring target_path = SlotPath(paths, target);
    if (!MoveFileExW(temp_path.c_str(), target_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SetError(error, L"无法发布 V9 base");
        return false;
    }
    ShardManifest next;
    next.version = 9;
    next.generation = current.generation + 1;
    next.active_built = built_unix;
    next.active_wal_bytes = wal_bytes;
    next.active_slot = target;
    next.previous_slot = current.source_id.empty() ? static_cast<uint8_t>(1 - target) : current.active_slot;
    next.active_bytes = static_cast<uint64_t>(-1);
    HANDLE h = CreateFileW(target_path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (GetFileSizeEx(h, &size)) next.active_bytes = static_cast<uint64_t>(size.QuadPart);
        CloseHandle(h);
    }
    next.active_crc64 = FileCrc64(target_path);
    next.source_id = source_id.empty()
        ? (current.source_id.empty() ? L"aggregate" : current.source_id)
        : source_id;
    if (!SaveShardManifest(paths.manifest, next, error)) return false;
    published = next;
    return true;
}
} // namespace pulse::index
