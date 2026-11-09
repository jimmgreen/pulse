#include "network_index.h"
#include "../fs/drive_kind.h"
#include "../fs/wsl_path.h"
#include "network_crawl_schedule.h"
#include "index_config.h"
#include "index_query.h"
#include "../common/utf8_file.h"
#include "../common/json_utils.h"
#include "../common/config_json.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <queue>
#include <sstream>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <winnetwk.h>

namespace pulse::index {
namespace {

#pragma pack(push, 1)
struct NetworkHeader {
    char magic[4];
    uint32_t version;
    uint64_t item_count;
    uint64_t records_off;
    uint64_t pool_off;
    uint64_t pool_chars;
};

struct NetworkRecord {
    uint32_t path_off;
    uint32_t path_len;
    uint32_t name_off;
    uint16_t name_len;
    uint16_t flags;
    uint64_t size;
    uint64_t mtime;
};
#pragma pack(pop)

constexpr uint32_t kNetworkVersion = 1;
constexpr uint16_t kRecordDirectory = 1;
constexpr uint64_t kMaxConfigBytes = 4ull * 1024 * 1024;
// The crawl thread sleeps until the next crawl is due (network_crawl_schedule.h)
// but wakes at least this often for the batched change-journal flush.
constexpr auto kCrawlIdleWake = std::chrono::seconds(60);
constexpr uint64_t kMaxShardAge100ns = 30ull * 24 * 3600 * 10000000;
// Change overlay bounds. Past them the overlay stops growing and a (debounced)
// crawl brings the shard up to date instead.
constexpr size_t kOverlayLimit = 50000;
constexpr size_t kSubtreeScanLimit = 20000;

void SetError(std::wstring* error, const std::wstring& value) {
    if (error) *error = value;
}

std::wstring Win32Message(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring out = L"错误 " + std::to_wstring(code);
    if (message) {
        while (*message && iswspace(message[wcslen(message) - 1]))
            message[wcslen(message) - 1] = 0;
        out += L"：";
        out += message;
        LocalFree(message);
    }
    return out;
}

bool ReadBytes(const std::wstring& path, std::vector<uint8_t>& bytes, std::wstring* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, Win32Message(GetLastError()));
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxConfigBytes) {
        const DWORD code = GetLastError() == ERROR_SUCCESS ? ERROR_FILE_TOO_LARGE : GetLastError();
        CloseHandle(file);
        SetError(error, Win32Message(code));
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD read = 0;
        const DWORD remaining = static_cast<DWORD>((std::min)(
            bytes.size() - done, static_cast<size_t>(UINT32_MAX)));
        if (!ReadFile(file, bytes.data() + done, remaining, &read, nullptr) || read == 0) {
            const DWORD code = GetLastError() == ERROR_SUCCESS ? ERROR_HANDLE_EOF : GetLastError();
            CloseHandle(file);
            SetError(error, Win32Message(code));
            return false;
        }
        done += read;
    }
    CloseHandle(file);
    return true;
}

bool WriteBytesAtomic(const std::wstring& path, const std::vector<uint8_t>& bytes,
                      std::wstring* error) {
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, Win32Message(GetLastError()));
        return false;
    }
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        const DWORD remaining = static_cast<DWORD>((std::min)(
            bytes.size() - done, static_cast<size_t>(UINT32_MAX)));
        if (!WriteFile(file, bytes.data() + done, remaining, &written, nullptr) || written == 0) {
            const DWORD code = GetLastError() == ERROR_SUCCESS ? ERROR_WRITE_FAULT : GetLastError();
            CloseHandle(file);
            DeleteFileW(temp.c_str());
            SetError(error, Win32Message(code));
            return false;
        }
        done += written;
    }
    if (!FlushFileBuffers(file)) {
        const DWORD code = GetLastError();
        CloseHandle(file);
        DeleteFileW(temp.c_str());
        SetError(error, Win32Message(code));
        return false;
    }
    CloseHandle(file);
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        SetError(error, Win32Message(GetLastError()));
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool EqualPath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool StartsWithPath(const std::wstring_view value, const std::wstring_view prefix) {
    if (prefix.empty()) return true;
    if (value.size() < prefix.size()) return false;
    if (CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(),
                             static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) return false;
    return value.size() == prefix.size() || prefix.back() == L'\\' ||
           value[prefix.size()] == L'\\';
}

std::wstring LongPath(const std::wstring& path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

uint64_t HashPath(const std::wstring& path) {
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : path) {
        const wchar_t folded = static_cast<wchar_t>(towupper(c));
        hash ^= static_cast<uint16_t>(folded);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring Hex64(uint64_t value) {
    wchar_t buffer[17]{};
    swprintf_s(buffer, L"%016llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::wstring NetworkDataDir() {
    const std::wstring base = UserIndexRoot();
    if (base.empty()) return {};
    const std::wstring dir = base + L"\\NetworkIndex";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return {};
    return dir;
}

std::wstring ShardPath(const std::wstring& root, int slot = -1) {
    const std::wstring dir = NetworkDataDir();
    if (dir.empty()) return {};
    std::wstring path = dir + L"\\network-" + Hex64(HashPath(root));
    if (slot >= 0) path += L"-" + std::to_wstring(slot);
    return path + L".bin";
}

bool WriteAll(HANDLE file, const void* data, size_t bytes) {
    const auto* cursor = static_cast<const uint8_t*>(data);
    while (bytes) {
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes, static_cast<size_t>(1u << 30)));
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

uint64_t FileTimeValue(const FILETIME& value) {
    return (static_cast<uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

// Case-insensitive path keys for the change overlay. Lookups take a
// wstring_view so the search thread does not allocate per record.
wchar_t FoldPathChar(wchar_t c) {
    if (c < 0x80) return c >= L'a' && c <= L'z' ? static_cast<wchar_t>(c - L'a' + L'A') : c;
    return static_cast<wchar_t>(reinterpret_cast<uintptr_t>(
        CharUpperW(reinterpret_cast<LPWSTR>(static_cast<uintptr_t>(c)))));
}

struct PathKeyHash {
    using is_transparent = void;
    size_t operator()(std::wstring_view value) const noexcept {
        uint64_t hash = 1469598103934665603ull;
        for (wchar_t c : value) {
            hash ^= static_cast<uint16_t>(FoldPathChar(c));
            hash *= 1099511628211ull;
        }
        return static_cast<size_t>(hash);
    }
};

struct PathKeyEqual {
    using is_transparent = void;
    bool operator()(std::wstring_view a, std::wstring_view b) const noexcept {
        return a.size() == b.size() &&
               CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                    static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }
};

template <typename Value>
using PathMap = std::unordered_map<std::wstring, Value, PathKeyHash, PathKeyEqual>;
using PathSet = std::unordered_set<std::wstring, PathKeyHash, PathKeyEqual>;

bool CopyHandle(HANDLE source, HANDLE destination) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(source, zero, nullptr, FILE_BEGIN)) return false;
    std::vector<uint8_t> buffer(1024 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            return false;
        if (!read) return true;
        if (!WriteAll(destination, buffer.data(), read)) return false;
    }
}

class ShardBuilder {
public:
    explicit ShardBuilder(const std::wstring& base)
        : records_path_(base + L".records.tmp"), pool_path_(base + L".pool.tmp") {
        records_ = CreateFileW(records_path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        pool_ = CreateFileW(pool_path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    }

    ~ShardBuilder() {
        if (records_ != INVALID_HANDLE_VALUE) CloseHandle(records_);
        if (pool_ != INVALID_HANDLE_VALUE) CloseHandle(pool_);
        DeleteFileW(records_path_.c_str());
        DeleteFileW(pool_path_.c_str());
    }

    bool Valid() const {
        return records_ != INVALID_HANDLE_VALUE && pool_ != INVALID_HANDLE_VALUE;
    }

    bool Add(const std::wstring& path, const WIN32_FIND_DATAW& find) {
        const size_t name_length = wcslen(find.cFileName);
        if (!Valid() || path.size() > UINT32_MAX || name_length > UINT16_MAX ||
            pool_chars_ > UINT32_MAX - path.size()) return false;
        NetworkRecord record{};
        record.path_off = static_cast<uint32_t>(pool_chars_);
        record.path_len = static_cast<uint32_t>(path.size());
        record.name_len = static_cast<uint16_t>(name_length);
        record.name_off = record.path_len - record.name_len;
        record.flags = (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? kRecordDirectory : 0;
        record.size = (static_cast<uint64_t>(find.nFileSizeHigh) << 32) | find.nFileSizeLow;
        record.mtime = FileTimeValue(find.ftLastWriteTime);
        if (!WriteAll(records_, &record, sizeof(record)) ||
            !WriteAll(pool_, path.data(), path.size() * sizeof(wchar_t))) return false;
        ++count_;
        pool_chars_ += path.size();
        return true;
    }

    uint64_t Count() const { return count_; }

    bool Save(const std::wstring& path) {
        if (!Valid() || path.empty()) return false;
        const std::wstring temp = path + L".tmp";
        HANDLE output = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) return false;
        NetworkHeader header{{'P', 'N', 'E', 'T'}, kNetworkVersion, count_,
                             sizeof(NetworkHeader),
                             sizeof(NetworkHeader) + count_ * sizeof(NetworkRecord),
                             pool_chars_};
        const bool ok = WriteAll(output, &header, sizeof(header)) &&
                        CopyHandle(records_, output) && CopyHandle(pool_, output) &&
                        FlushFileBuffers(output);
        CloseHandle(output);
        if (!ok || !MoveFileExW(temp.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    }

private:
    std::wstring records_path_;
    std::wstring pool_path_;
    HANDLE records_ = INVALID_HANDLE_VALUE;
    HANDLE pool_ = INVALID_HANDLE_VALUE;
    uint64_t count_ = 0;
    uint64_t pool_chars_ = 0;
};

bool MatchNetworkRecord(const NetworkRecord& record, std::wstring_view path,
                        std::wstring_view name, const CompiledQuery& query,
                        bool folders_only) {
    const bool is_dir = (record.flags & kRecordDirectory) != 0;
    if (folders_only && !is_dir) return false;
    if (query.groups.empty()) return true;
    for (const auto& group : query.groups) {
        bool matched = true;
        for (const auto& term : group) {
            if (!MatchTerm(path, name, is_dir, record.size, record.mtime, term)) {
                matched = false;
                break;
            }
        }
        if (matched) return true;
    }
    return false;
}

int CompareName(const Hit& a, const Hit& b) {
    const int by_name = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE);
    if (by_name != CSTR_EQUAL) return by_name == CSTR_LESS_THAN ? -1 : 1;
    const int by_path = CompareStringOrdinal(a.path.c_str(), -1, b.path.c_str(), -1, TRUE);
    return by_path == CSTR_LESS_THAN ? -1 : by_path == CSTR_GREATER_THAN ? 1 : 0;
}

bool BetterHit(const Hit& a, const Hit& b, const Query& query,
               const CompiledQuery& compiled) {
    if (query.rank) {
        const int as = RankName(a.name.data(), static_cast<uint32_t>(a.name.size()), a.is_dir, compiled);
        const int bs = RankName(b.name.data(), static_cast<uint32_t>(b.name.size()), b.is_dir, compiled);
        if (as != bs) return as > bs;
        return CompareName(a, b) < 0;
    }
    int cmp = 0;
    if (query.sort == ResultSort::Size && a.size != b.size) cmp = a.size < b.size ? -1 : 1;
    else if (query.sort == ResultSort::Mtime && a.mtime != b.mtime) cmp = a.mtime < b.mtime ? -1 : 1;
    else cmp = CompareName(a, b);
    if (query.sort_desc) cmp = -cmp;
    return cmp < 0;
}

} // namespace

struct NetworkIndex::Shard {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
    const uint8_t* view = nullptr;
    size_t bytes = 0;
    const NetworkRecord* records = nullptr;
    const wchar_t* pool = nullptr;
    size_t count = 0;
    int slot = -1;
    uint64_t modified = 0;
    // Storage of in-memory shards (change overlay); empty for mapped files.
    std::vector<NetworkRecord> owned_records;
    std::vector<wchar_t> owned_pool;

    ~Shard() {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    static std::shared_ptr<Shard> Open(const std::wstring& path, int slot = -1) {
        auto shard = std::make_shared<Shard>();
        shard->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (shard->file == INVALID_HANDLE_VALUE) return {};
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(shard->file, &size) || size.QuadPart < sizeof(NetworkHeader) ||
            static_cast<uint64_t>(size.QuadPart) > SIZE_MAX) return {};
        FILETIME write_time{};
        GetFileTime(shard->file, nullptr, nullptr, &write_time);
        shard->modified = FileTimeValue(write_time);
        shard->slot = slot;
        shard->bytes = static_cast<size_t>(size.QuadPart);
        shard->mapping = CreateFileMappingW(shard->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!shard->mapping) return {};
        shard->view = static_cast<const uint8_t*>(MapViewOfFile(shard->mapping, FILE_MAP_READ, 0, 0, 0));
        if (!shard->view) return {};
        const auto* header = reinterpret_cast<const NetworkHeader*>(shard->view);
        if (memcmp(header->magic, "PNET", 4) != 0 || header->version != kNetworkVersion ||
            header->records_off > shard->bytes || header->pool_off > shard->bytes ||
            header->item_count > (shard->bytes - header->records_off) / sizeof(NetworkRecord) ||
            header->pool_chars > (shard->bytes - header->pool_off) / sizeof(wchar_t)) return {};
        shard->records = reinterpret_cast<const NetworkRecord*>(shard->view + header->records_off);
        shard->pool = reinterpret_cast<const wchar_t*>(shard->view + header->pool_off);
        shard->count = static_cast<size_t>(header->item_count);
        for (size_t i = 0; i < shard->count; ++i) {
            const auto& record = shard->records[i];
            if (record.path_off > header->pool_chars ||
                record.path_len > header->pool_chars - record.path_off ||
                record.name_off > record.path_len || record.name_len > record.path_len - record.name_off)
                return {};
        }
        return shard;
    }

    static std::shared_ptr<Shard> FromMemory(std::vector<NetworkRecord> records,
                                             std::vector<wchar_t> pool) {
        auto shard = std::make_shared<Shard>();
        shard->owned_records = std::move(records);
        shard->owned_pool = std::move(pool);
        shard->records = shard->owned_records.data();
        shard->pool = shard->owned_pool.data();
        shard->count = shard->owned_records.size();
        return shard;
    }
};

// Guarded by mu_. Entries and removals stamped before a successful crawl
// started are dropped when its shard is published.
struct NetworkIndex::Overlay {
    struct Entry {
        bool is_dir = false;
        uint64_t size = 0;
        uint64_t mtime = 0;
        std::chrono::steady_clock::time_point seen{};
    };
    PathMap<Entry> entries;                                   // created or changed
    PathMap<std::chrono::steady_clock::time_point> removed;   // deleted or renamed away
    std::vector<std::wstring> pending_scans;
    // Last change dropped because the overlay was full.
    std::chrono::steady_clock::time_point last_skipped{};
    std::chrono::steady_clock::time_point watch_started{};
    std::shared_ptr<const OverlayView> view;

    size_t Size() const { return entries.size() + removed.size(); }
    bool CanApplyScan(std::wstring_view path, std::chrono::steady_clock::time_point started) const {
        const auto newer = entries.find(path);
        if (newer != entries.end() && newer->second.seen >= started) return false;
        for (;;) {
            const auto removed_at = removed.find(path);
            if (removed_at != removed.end() && removed_at->second >= started) return false;
            const auto slash = path.rfind(L'\\');
            if (slash == std::wstring_view::npos || slash <= 2) break;
            path = path.substr(0, slash);
        }
        return true;
    }
};

struct NetworkIndex::OverlayView {
    std::shared_ptr<Shard> added;   // live overlay entries as records
    PathSet replaced;               // shard records superseded by an entry
    PathSet removed;                // hides the shard record and its subtree

    bool Hides(std::wstring_view path) const {
        if (replaced.find(path) != replaced.end()) return true;
        if (removed.empty()) return false;
        if (removed.find(path) != removed.end()) return true;
        for (size_t slash = path.rfind(L'\\'); slash != std::wstring_view::npos && slash > 2;
             slash = path.rfind(L'\\', slash - 1)) {
            if (removed.find(path.substr(0, slash)) != removed.end()) return true;
        }
        return false;
    }
};

std::wstring NormalizeNetworkRoot(std::wstring path) {
    while (!path.empty() && iswspace(path.back())) path.pop_back();
    size_t first = 0;
    while (first < path.size() && iswspace(path[first])) ++first;
    if (first) path.erase(0, first);
    if (path.size() >= 2 && path.front() == L'\"' && path.back() == L'\"')
        path = path.substr(1, path.size() - 2);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0) path = L"\\\\" + path.substr(8);

    if (path.size() >= 2 && path[1] == L':') {
        DWORD bytes = 0;
        const DWORD result = WNetGetUniversalNameW(path.c_str(), UNIVERSAL_NAME_INFO_LEVEL, nullptr, &bytes);
        constexpr DWORD maximum_bytes = sizeof(UNIVERSAL_NAME_INFOW) + 32768 * sizeof(wchar_t);
        if (result == ERROR_MORE_DATA && bytes >= sizeof(UNIVERSAL_NAME_INFOW) && bytes <= maximum_bytes) {
            std::vector<uint8_t> buffer(bytes);
            if (WNetGetUniversalNameW(path.c_str(), UNIVERSAL_NAME_INFO_LEVEL,
                                      buffer.data(), &bytes) == NO_ERROR) {
                const auto* info = reinterpret_cast<const UNIVERSAL_NAME_INFOW*>(buffer.data());
                if (info->lpUniversalName) path = info->lpUniversalName;
            }
        }
    }
    while (path.size() > 2 && path.back() == L'\\') path.pop_back();
    if (path.rfind(L"\\\\", 0) != 0) return {};
    const size_t server_end = path.find(L'\\', 2);
    if (server_end == std::wstring::npos || server_end == 2 || server_end + 1 >= path.size()) return {};
    const size_t share_end = path.find(L'\\', server_end + 1);
    const size_t share_len = (share_end == std::wstring::npos ? path.size() : share_end) - server_end - 1;
    if (!share_len) return {};
    return path;
}

std::wstring NetworkConfigPath() {
    const std::wstring root = UserIndexRoot();
    return root.empty() ? L"" : root + L"\\network-index.json";
}

bool LoadNetworkRoots(std::vector<std::wstring>& roots, std::wstring* error) {
    const std::wstring path = NetworkConfigPath();
    if (path.empty()) {
        SetError(error, L"无法定位当前用户配置目录");
        return false;
    }
    return LoadNetworkRootsFile(path, roots, error);
}

bool LoadNetworkRootsFile(const std::wstring& path, std::vector<std::wstring>& roots,
                          std::wstring* error) {
    SetError(error, {});
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            roots.clear();
            return true;
        }
    }
    std::vector<uint8_t> bytes;
    if (!ReadBytes(path, bytes, error)) return false;
    std::wstring json;
    if (!pulse::DecodeUtf8Bytes(bytes, json)) {
        SetError(error, L"网络索引配置不是有效的 UTF-8 文件");
        return false;
    }
    std::vector<std::wstring> raw_roots;
    if (!pulse::json::ConfigSyntax(json, false).StringArrayMember(L"roots", raw_roots)) {
        SetError(error, L"网络索引配置已损坏");
        return false;
    }
    std::vector<std::wstring> parsed;
    for (const auto& raw : raw_roots) {
        std::wstring root = NormalizeNetworkRoot(raw);
        if (root.empty() || std::any_of(raw.begin(), raw.end(), [](wchar_t c) { return c < 0x20; })) {
            SetError(error, L"网络索引配置包含无效的服务器文件夹");
            return false;
        }
        // A WSL root added before 1.0.57 is left out: crawling and watching it
        // kept the distribution running and `wsl --shutdown` waiting (#360).
        if (pulse::fs::IsWslPath(root)) continue;
        if (std::none_of(parsed.begin(), parsed.end(), [&](const auto& existing) {
                return EqualPath(existing, root);
            })) parsed.push_back(std::move(root));
    }
    roots = std::move(parsed);
    return true;
}

bool SaveNetworkRoots(const std::vector<std::wstring>& roots, std::wstring* error) {
    const std::wstring path = NetworkConfigPath();
    if (path.empty()) {
        SetError(error, L"无法定位当前用户配置目录");
        return false;
    }
    return SaveNetworkRootsFile(path, roots, error);
}

bool SaveNetworkRootsFile(const std::wstring& path, const std::vector<std::wstring>& roots,
                          std::wstring* error) {
    std::wstring json = L"{\n  \"version\":1,\n  \"roots\":[";
    for (size_t i = 0; i < roots.size(); ++i) {
        std::wstring escaped;
        pulse::json::Escape(roots[i], escaped);
        if (i) json += L",";
        json += L"\n    \"" + escaped + L"\"";
    }
    if (!roots.empty()) json += L"\n  ";
    json += L"]\n}\n";
    std::vector<uint8_t> bytes;
    if (!pulse::EncodeUtf8Bytes(json, bytes)) {
        SetError(error, L"无法将网络索引配置编码为 UTF-8");
        return false;
    }
    return WriteBytesAtomic(path, bytes, error);
}

ChangeState NetworkIndex::ChangeCoverage(const std::wstring& path) const {
    const auto normalized = NormalizeNetworkRoot(NormalizeChangePath(path));
    std::lock_guard lock(mu_);
    for (const auto& root : roots_) {
        if (!EqualPath(normalized, root.info.path) &&
            !(normalized.size() > root.info.path.size() && normalized[root.info.path.size()] == L'\\' &&
              CompareStringOrdinal(normalized.c_str(), static_cast<int>(root.info.path.size()), root.info.path.c_str(), -1, TRUE) == CSTR_EQUAL)) continue;
        if (root.info.building) return ChangeState::Scanning;
        if (!root.info.online) return ChangeState::Offline;
        return ChangeState::Gap;
    }
    return ChangeState::NotCovered;
}

void NetworkIndex::SetChangeLease(const std::wstring& owner, bool enabled) {
    const bool seed = changes_.Lease(owner, enabled);
    {
        std::lock_guard lock(change_seed_mutex_);
        if (seed) change_seed_owners_.insert(owner);
        if (!enabled) change_seed_owners_.erase(owner);
    }
    if (seed) crawl_cv_.notify_one();
}

void NetworkIndex::SeedPendingChanges() {
    std::unordered_set<std::wstring> owners;
    { std::lock_guard lock(change_seed_mutex_); owners.swap(change_seed_owners_); }
    for (const auto& owner : owners) SeedChanges(owner);
}

void NetworkIndex::SeedChanges(const std::wstring& owner) {
    std::vector<ChangeRecord> baseline;
    const auto since = ChangeTracker::Now() - 7 * 86400;
    {
        std::lock_guard lock(mu_);
        if (std::any_of(roots_.begin(), roots_.end(), [](const auto& root) { return root.info.building; })) return;
        for (const auto& root : roots_) {
            if (!root.shard) continue;
            for (size_t i = 0; i < root.shard->count && baseline.size() < 100000; ++i) {
                const auto& record = root.shard->records[i];
                if (IsChangeJournalName(std::wstring_view(root.shard->pool + record.path_off, record.path_len))) continue;
                const auto seconds = record.mtime > 116444736000000000ULL
                    ? record.mtime / 10000000 - 11644473600ULL : record.mtime;
                if (seconds < since) continue;
                ChangeRecord event; event.path.assign(root.shard->pool + record.path_off, record.path_len);
                event.time = seconds; event.is_dir = (record.flags & kRecordDirectory) != 0;
                baseline.push_back(std::move(event));
            }
        }
    }
    changes_.Seed(owner, std::move(baseline));
}

void NetworkIndex::ObserveChanges(const std::wstring& root, const BYTE* data, DWORD bytes) {
    struct Update {
        std::wstring path;
        bool removed = false;
        bool scan = false;   // new directory: its children are not reported
        Overlay::Entry entry;
    };
    std::vector<Update> updates;
    bool gap = false;
    std::wstring old_path;
    size_t offset = 0;
    while (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= bytes) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(data + offset);
        if (info->FileNameLength % sizeof(wchar_t) ||
            info->FileNameLength > bytes - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName)) { gap = true; break; }
        ChangeRecord event;
        event.path = root + L"\\" + std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t));
        if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
            old_path = event.path;
            updates.push_back({event.path, true});
        } else {
            // A removed entry cannot be queried; skip the SMB round trip.
            // Otherwise one query yields everything the overlay record needs.
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            const bool exists = info->Action != FILE_ACTION_REMOVED &&
                GetFileAttributesExW(LongPath(event.path).c_str(), GetFileExInfoStandard, &attributes);
            event.is_dir = exists && (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            Update update{event.path, !exists};
            if (exists) {
                update.entry.is_dir = event.is_dir;
                update.entry.size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
                update.entry.mtime = FileTimeValue(attributes.ftLastWriteTime);
                update.scan = event.is_dir && (info->Action == FILE_ACTION_ADDED ||
                                               info->Action == FILE_ACTION_RENAMED_NEW_NAME);
            }
            updates.push_back(std::move(update));
            if (info->Action == FILE_ACTION_ADDED) event.kind = ChangeKind::Created;
            else if (info->Action == FILE_ACTION_REMOVED) event.kind = ChangeKind::Deleted;
            else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) { event.kind = ChangeKind::Renamed; event.old_path = std::move(old_path); }
            changes_.Record(std::move(event));
        }
        if (!info->NextEntryOffset) break;
        if (info->NextEntryOffset > bytes - offset || info->NextEntryOffset < offsetof(FILE_NOTIFY_INFORMATION, FileName)) { gap = true; break; }
        offset += info->NextEntryOffset;
    }
    // WatchLoop re-arms the same handle after each read, so the redirector
    // queues changes in between; lost events (overflow, failed reads) are
    // reported as a gap by the caller.
    if (gap) changes_.Gap();

    bool scan_queued = false;
    bool needs_crawl = gap;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        for (auto& state : roots_) {
            if (!EqualPath(state.info.path, root)) continue;
            if (!state.overlay) state.overlay = std::make_shared<Overlay>();
            Overlay& overlay = *state.overlay;
            for (auto& update : updates) {
                if (overlay.Size() >= kOverlayLimit) {
                    overlay.last_skipped = now;
                    needs_crawl = true;
                    break;
                }
                if (update.removed) {
                    if (const auto it = overlay.entries.find(update.path); it != overlay.entries.end())
                        overlay.entries.erase(it);
                    overlay.removed.insert_or_assign(update.path, now);
                    continue;
                }
                update.entry.seen = now;
                if (update.scan) {
                    overlay.pending_scans.push_back(update.path);
                    scan_queued = true;
                }
                overlay.entries.insert_or_assign(std::move(update.path), update.entry);
            }
            overlay.view.reset();
            break;
        }
    }
    if (scan_queued) crawl_cv_.notify_one();
    if (needs_crawl) NoteRootChanged(root);
}

std::shared_ptr<const NetworkIndex::OverlayView> NetworkIndex::OverlayViewLocked(RootState& root) {
    if (!root.overlay) return {};
    Overlay& overlay = *root.overlay;
    if (overlay.view || (overlay.entries.empty() && overlay.removed.empty())) return overlay.view;
    auto view = std::make_shared<OverlayView>();
    // An entry is gone if it, or a directory above it, was removed after the
    // entry was last seen (deleted or renamed away together with its parent).
    auto removed_since = [&](std::wstring_view path, std::chrono::steady_clock::time_point seen) {
        for (size_t end = path.size(); end > 2 && end != std::wstring_view::npos;
             end = path.rfind(L'\\', end - 1)) {
            const auto it = overlay.removed.find(path.substr(0, end));
            if (it != overlay.removed.end() && it->second > seen) return true;
        }
        return false;
    };
    std::vector<NetworkRecord> records;
    std::vector<wchar_t> pool;
    records.reserve(overlay.entries.size());
    for (const auto& [path, entry] : overlay.entries) {
        view->replaced.insert(path);
        if (removed_since(path, entry.seen)) continue;
        const size_t slash = path.rfind(L'\\');
        const size_t name_length = slash == std::wstring::npos ? path.size() : path.size() - slash - 1;
        if (path.size() > UINT32_MAX || name_length > UINT16_MAX ||
            pool.size() > UINT32_MAX - path.size()) continue;
        NetworkRecord record{};
        record.path_off = static_cast<uint32_t>(pool.size());
        record.path_len = static_cast<uint32_t>(path.size());
        record.name_len = static_cast<uint16_t>(name_length);
        record.name_off = record.path_len - record.name_len;
        record.flags = entry.is_dir ? kRecordDirectory : 0;
        record.size = entry.size;
        record.mtime = entry.mtime;
        pool.insert(pool.end(), path.begin(), path.end());
        records.push_back(record);
    }
    for (const auto& item : overlay.removed) view->removed.insert(item.first);
    view->added = Shard::FromMemory(std::move(records), std::move(pool));
    overlay.view = view;
    return overlay.view;
}

void NetworkIndex::ScanPendingSubtrees() {
    struct Job {
        std::wstring root;
        std::wstring directory;
        std::shared_ptr<Overlay> overlay;
        uint64_t generation = 0;
    };
    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& root : roots_) {
            if (!root.overlay) continue;
            for (auto& directory : root.overlay->pending_scans)
                jobs.push_back({root.info.path, std::move(directory), root.overlay, generation_});
            root.overlay->pending_scans.clear();
        }
    }
    for (const auto& job : jobs) {
        if (!running_) return;
        const auto scan_start = std::chrono::steady_clock::now();
        std::vector<std::pair<std::wstring, Overlay::Entry>> found;
        bool truncated = false;
        DWORD failure = ERROR_SUCCESS;
        std::vector<std::wstring> stack{job.directory};
        while (!stack.empty() && running_ && !truncated && failure == ERROR_SUCCESS) {
            const std::wstring directory = std::move(stack.back());
            stack.pop_back();
            const std::wstring pattern = LongPath(directory) + L"\\*";
            WIN32_FIND_DATAW find{};
            HANDLE handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &find,
                                             FindExSearchNameMatch, nullptr,
                                             FIND_FIRST_EX_LARGE_FETCH);
            if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER) {
                handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &find,
                                          FindExSearchNameMatch, nullptr, 0);
            }
            if (handle == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES)
                    failure = error ? error : ERROR_GEN_FAILURE;
                continue;
            }
            do {
                if (!running_) { truncated = true; break; }
                if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0)
                    continue;
                if (found.size() >= kSubtreeScanLimit) {
                    truncated = true;
                    break;
                }
                Overlay::Entry entry;
                entry.is_dir = (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                entry.size = (static_cast<uint64_t>(find.nFileSizeHigh) << 32) | find.nFileSizeLow;
                entry.mtime = FileTimeValue(find.ftLastWriteTime);
                entry.seen = scan_start;
                std::wstring full = directory + L"\\" + find.cFileName;
                if (entry.is_dir && !(find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back(full);
                found.emplace_back(std::move(full), entry);
            } while (FindNextFileW(handle, &find));
            const DWORD enumeration_error = GetLastError();
            if (!truncated && enumeration_error != ERROR_NO_MORE_FILES)
                failure = enumeration_error ? enumeration_error : ERROR_GEN_FAILURE;
            FindClose(handle);
        }
        if (!running_) return;
        if (failure != ERROR_SUCCESS) found.clear();
        // A subtree too large for the overlay is left to a crawl.
        bool needs_crawl = truncated || failure != ERROR_SUCCESS;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& root : roots_) {
                if (!EqualPath(root.info.path, job.root)) continue;
                // Removing and re-adding the same path creates a different
                // overlay; an old job must never populate the new root.
                if (root.overlay != job.overlay) break;
                if (generation_ != job.generation) {
                    // An unrelated root edit invalidated the batch. Retry
                    // this still-current root rather than losing its scan.
                    root.overlay->pending_scans.push_back(job.directory);
                    break;
                }
                Overlay& overlay = *root.overlay;
                if (failure != ERROR_SUCCESS) {
                    root.info.error = Win32Message(failure);
                    root.info.state = L"子目录扫描未完成，等待重新扫描";
                }
                for (auto& [path, entry] : found) {
                    const auto newer = overlay.entries.find(path);
                    if (newer != overlay.entries.end() && newer->second.seen >= scan_start) continue;
                    bool removed = false;
                    for (size_t end = path.size(); end > 2 && end != std::wstring::npos;
                         end = path.rfind(L'\\', end - 1)) {
                        const auto tombstone = overlay.removed.find(std::wstring_view(path).substr(0, end));
                        if (tombstone != overlay.removed.end() && tombstone->second >= scan_start) {
                            removed = true;
                            break;
                        }
                    }
                    if (removed) continue;
                    if (overlay.Size() >= kOverlayLimit) {
                        overlay.last_skipped = scan_start;
                        needs_crawl = true;
                        break;
                    }
                    overlay.entries.insert_or_assign(std::move(path), entry);
                }
                overlay.view.reset();
                if (needs_crawl) {
                    const auto now = std::chrono::steady_clock::now();
                    if (!root.change_pending) root.change_first = now;
                    root.change_pending = true;
                    root.change_last = now;
                }
                break;
            }
        }
        if (needs_crawl) crawl_cv_.notify_one();
    }
}

void NetworkIndex::NoteRootChanged(const std::wstring& path) {
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        for (auto& root : roots_) {
            if (!EqualPath(root.info.path, path)) continue;
            if (!root.change_pending) {
                root.change_pending = true;
                root.change_first = now;
                first = true;
            }
            root.change_last = now;
            break;
        }
    }
    // Later changes only push the due time back; the crawl thread re-reads
    // it when it wakes.
    if (first) crawl_cv_.notify_one();
}

std::chrono::steady_clock::time_point NetworkIndex::CollectDueRootsLocked(
        std::chrono::steady_clock::time_point now, std::vector<std::wstring>& due) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point unset{};
    Clock::time_point next = Clock::time_point::max();
    for (auto& root : roots_) {
        if (root.info.building) continue;
        Clock::time_point when = Clock::time_point::max();
        if (dirty_roots_.count(root.info.path)) when = now;
        if (root.startup_due != unset) when = (std::min)(when, root.startup_due);
        if (root.change_pending) {
            when = (std::min)(when, crawl_schedule::ChangeDue(root.change_first, root.change_last,
                                                               root.last_crawl_end, root.last_crawl));
        }
        // Until the first crawl of this session the start-up rule applies;
        // the watch may simply not be armed yet.
        if ((!root.info.watching || root.last_crawl_failed) && root.last_crawl_end != unset) {
            when = (std::min)(when, crawl_schedule::UnwatchedDue(root.last_crawl_end, root.last_crawl,
                                                                  root.last_crawl_failed));
        }
        if (when <= now) {
            due.push_back(root.info.path);
            dirty_roots_.erase(root.info.path);
            root.startup_due = unset;
            // Changes seen from here on belong to the next crawl.
            root.change_pending = false;
        } else {
            next = (std::min)(next, when);
        }
    }
    // Requests for roots that were removed meanwhile.
    std::erase_if(dirty_roots_, [this](const std::wstring& path) {
        return std::none_of(roots_.begin(), roots_.end(), [&](const RootState& root) {
            return root.info.path == path;
        });
    });
    return next;
}

bool NetworkIndex::ReadConfig(const std::wstring& path, std::vector<std::wstring>& configured) {
    std::wstring error;
    const bool loaded = !path.empty() && LoadNetworkRootsFile(path, configured, &error);
    std::lock_guard<std::mutex> lock(mu_);
    config_error_ = loaded ? L"" : error.empty() ? L"无法读取网络索引配置" : error;
    return loaded;
}

void NetworkIndex::Start(HWND notify, UINT status_msg, UINT search_msg) {
    changes_.Open(NetworkDataDir());
    Stop();
    notify_ = notify;
    status_msg_ = status_msg;
    search_msg_ = search_msg;
    std::vector<std::wstring> configured;
    std::wstring error;
    const bool config_loaded = LoadNetworkRoots(configured, &error);
    {
        std::lock_guard<std::mutex> lock(mu_);
        roots_.clear();
        config_error_ = config_loaded ? L"" : error.empty() ? L"无法读取网络索引配置" : error;
        dirty_roots_.clear();
        const auto started = std::chrono::steady_clock::now();
        FILETIME now_time{};
        GetSystemTimeAsFileTime(&now_time);
        const uint64_t now_value = FileTimeValue(now_time);
        for (const auto& path : configured) {
            RootState state;
            state.info.path = path;
            auto first = Shard::Open(ShardPath(path, 0), 0);
            auto second = Shard::Open(ShardPath(path, 1), 1);
            if (first && second) state.shard = first->modified >= second->modified ? first : second;
            else if (first) state.shard = std::move(first);
            else if (second) state.shard = std::move(second);
            else state.shard = Shard::Open(ShardPath(path));
            state.info.indexed_items = state.shard ? state.shard->count : 0;
            state.info.state = state.shard ? L"等待服务器校验" : L"等待扫描";
            if (state.shard) {
                // Search the previous session's index right away; the watch
                // covers changes from now on and a reconcile catches up on
                // what happened while the agent was not running.
                const uint64_t age = (std::min)(kMaxShardAge100ns,
                    now_value > state.shard->modified ? now_value - state.shard->modified : 0);
                state.startup_due = crawl_schedule::StartupDue(started,
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<uint64_t, std::ratio<1, 10000000>>(age)));
            } else {
                dirty_roots_.insert(path);
            }
            roots_.push_back(std::move(state));
        }
        ++generation_;
    }
    running_ = true;
    watch_wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    crawl_thread_ = std::thread([this] { CrawlLoop(); });
    watch_thread_ = std::thread([this] { WatchLoop(); });
    search_thread_ = std::thread([this] { SearchLoop(); });
}

void NetworkIndex::Stop() {
    running_ = false;
    if (watch_wake_event_) SetEvent(watch_wake_event_);
    // The crawl thread checks running_ under mu_ before a long wait; taking
    // the lock here orders this notification after that check.
    { std::lock_guard<std::mutex> lock(mu_); }
    crawl_cv_.notify_all();
    search_cv_.notify_all();
    if (crawl_thread_.joinable()) CancelSynchronousIo(crawl_thread_.native_handle());
    if (crawl_thread_.joinable()) crawl_thread_.join();
    if (watch_thread_.joinable()) watch_thread_.join();
    if (search_thread_.joinable()) search_thread_.join();
    changes_.Flush();
    if (watch_wake_event_) {
        CloseHandle(watch_wake_event_);
        watch_wake_event_ = nullptr;
    }
}

void NetworkIndex::NotifyStatus() const {
    if (notify_ && status_msg_) PostMessageW(notify_, status_msg_, 0, 0);
}

std::vector<NetworkRootInfo> NetworkIndex::Roots() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<NetworkRootInfo> result;
    result.reserve(roots_.size());
    for (const auto& root : roots_) result.push_back(root.info);
    return result;
}

std::wstring NetworkIndex::ConfigError() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_error_;
}

bool NetworkIndex::ReloadConfigurationLocked(std::wstring* error) {
    std::vector<std::wstring> configured;
    std::wstring load_error;
    if (!LoadNetworkRoots(configured, &load_error)) {
        config_error_ = load_error.empty() ? L"无法读取网络索引配置" : std::move(load_error);
        SetError(error, config_error_);
        NotifyStatus();
        return false;
    }
    config_error_.clear();
    SetError(error, {});
    const bool unchanged = configured.size() == roots_.size() &&
        std::equal(configured.begin(), configured.end(), roots_.begin(),
            [](const std::wstring& path, const RootState& root) { return EqualPath(path, root.info.path); });
    if (!unchanged) {
        std::vector<RootState> refreshed;
        refreshed.reserve(configured.size());
        for (const auto& path : configured) {
            auto existing = std::find_if(roots_.begin(), roots_.end(),
                [&](const RootState& root) { return EqualPath(path, root.info.path); });
            if (existing != roots_.end()) {
                refreshed.push_back(std::move(*existing));
            } else {
                RootState root;
                root.info.path = path;
                root.info.state = L"等待扫描";
                refreshed.push_back(std::move(root));
                dirty_roots_.insert(path);
            }
        }
        roots_ = std::move(refreshed);
        ++generation_;
        crawl_cv_.notify_one();
        if (watch_wake_event_) SetEvent(watch_wake_event_);
    }
    NotifyStatus();
    return true;
}

bool NetworkIndex::AddRoot(const std::wstring& raw, std::wstring* error) {
    const std::wstring path = NormalizeNetworkRoot(raw);
    if (path.empty()) {
        SetError(error, L"请选择服务器共享中的文件夹（UNC 路径）");
        return false;
    }
    if (pulse::fs::IsWslPath(path)) {
        SetError(error, L"WSL 中的文件不能加入网络索引");
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!ReloadConfigurationLocked(error)) return false;
    if (std::any_of(roots_.begin(), roots_.end(), [&](const RootState& root) {
            return EqualPath(root.info.path, path);
        })) return true;
    std::vector<std::wstring> configured;
    configured.reserve(roots_.size() + 1);
    for (const auto& root : roots_) configured.push_back(root.info.path);
    configured.push_back(path);
    if (!SaveNetworkRoots(configured, error)) return false;
    RootState state;
    state.info.path = path;
    state.info.state = L"等待扫描";
    roots_.push_back(std::move(state));
    dirty_roots_.insert(path);
    ++generation_;
    crawl_cv_.notify_one();
    if (watch_wake_event_) SetEvent(watch_wake_event_);
    NotifyStatus();
    return true;
}

bool NetworkIndex::RemoveRoot(const std::wstring& raw, std::wstring* error) {
    const std::wstring path = NormalizeNetworkRoot(raw);
    std::lock_guard<std::mutex> lock(mu_);
    if (!ReloadConfigurationLocked(error)) return false;
    auto found = std::find_if(roots_.begin(), roots_.end(), [&](const RootState& root) {
        return EqualPath(root.info.path, path);
    });
    if (found == roots_.end()) return true;
    std::vector<std::wstring> configured;
    for (const auto& root : roots_)
        if (!EqualPath(root.info.path, path)) configured.push_back(root.info.path);
    if (!SaveNetworkRoots(configured, error)) return false;
    roots_.erase(found);
    dirty_roots_.erase(path);
    DeleteFileW(ShardPath(path).c_str());
    DeleteFileW(ShardPath(path, 0).c_str());
    DeleteFileW(ShardPath(path, 1).c_str());
    ++generation_;
    crawl_cv_.notify_one();
    if (watch_wake_event_) SetEvent(watch_wake_event_);
    NotifyStatus();
    return true;
}

void NetworkIndex::Rebuild(const std::wstring& raw) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::wstring path = raw.empty() ? L"" : NormalizeNetworkRoot(raw);
    for (auto& root : roots_) {
        if (path.empty() || EqualPath(root.info.path, path)) {
            dirty_roots_.insert(root.info.path);
            root.info.state = L"等待重新扫描";
        }
    }
    crawl_cv_.notify_one();
    NotifyStatus();
}

bool NetworkIndex::RootStillCurrent(const std::wstring& path, uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_ && generation == generation_ &&
           std::any_of(roots_.begin(), roots_.end(), [&](const RootState& root) {
               return EqualPath(root.info.path, path);
           });
}

void NetworkIndex::BuildRoot(const std::wstring& path, uint64_t generation) {
    auto abandon = [&] {
        if (!running_) return;
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& root : roots_) {
            if (!EqualPath(root.info.path, path)) continue;
            root.info.building = false;
            root.info.progress = 0;
            root.info.indexed_items = root.shard ? root.shard->count : 0;
            root.info.state = L"等待重新扫描";
            dirty_roots_.insert(root.info.path);
            break;
        }
    };
    const auto crawl_start = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& root : roots_) {
            if (!EqualPath(root.info.path, path)) continue;
            // The directory watch stays armed during the crawl.
            root.info.building = true;
            root.info.progress = 0;
            root.info.state = L"正在扫描服务器文件夹";
            root.info.error.clear();
            break;
        }
    }
    NotifyStatus();

    ShardBuilder builder(ShardPath(path) + L".build");
    std::vector<std::wstring> stack{path};
    DWORD failure = builder.Valid() ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    bool root_accessible = true;
    uint64_t since_notify = 0;
    while (!stack.empty() && running_ && failure == ERROR_SUCCESS) {
        if (!RootStillCurrent(path, generation)) {
            abandon();
            return;
        }
        std::wstring directory = std::move(stack.back());
        stack.pop_back();
        const std::wstring pattern = LongPath(directory) + L"\\*";
        WIN32_FIND_DATAW find{};
        HANDLE handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &find,
                                         FindExSearchNameMatch, nullptr,
                                         FIND_FIRST_EX_LARGE_FETCH);
        if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER) {
            handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &find,
                                      FindExSearchNameMatch, nullptr, 0);
        }
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES) {
                failure = error ? error : ERROR_GEN_FAILURE;
                if (EqualPath(directory, path)) root_accessible = false;
            }
            continue;
        }
        do {
            if (!running_) { failure = ERROR_OPERATION_ABORTED; break; }
            if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0)
                continue;
            std::wstring full = directory + L"\\" + find.cFileName;
            if (!builder.Add(full, find)) {
                failure = ERROR_WRITE_FAULT;
                break;
            }
            if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(std::move(full));
            if (++since_notify >= 2048) {
                since_notify = 0;
                std::lock_guard<std::mutex> lock(mu_);
                for (auto& root : roots_) {
                    if (!EqualPath(root.info.path, path)) continue;
                    root.info.indexed_items = builder.Count();
                    root.info.state = L"正在扫描 · " + std::to_wstring(builder.Count()) + L" 项";
                    break;
                }
                NotifyStatus();
            }
        } while (FindNextFileW(handle, &find));
        const DWORD enumeration_error = GetLastError();
        if (failure == ERROR_SUCCESS && enumeration_error != ERROR_NO_MORE_FILES)
            failure = enumeration_error ? enumeration_error : ERROR_GEN_FAILURE;
        FindClose(handle);
    }
    if (!running_) return;
    if (!RootStillCurrent(path, generation)) {
        abandon();
        return;
    }

    const bool server_accessible = root_accessible;
    int next_slot = 0;
    bool watch_gap = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& root : roots_) {
            if (!EqualPath(root.info.path, path)) continue;
            if (root.shard && root.shard->slot == 0) next_slot = 1;
            watch_gap = root.overlay && root.overlay->watch_started >= crawl_start;
            break;
        }
    }
    if (failure == ERROR_SUCCESS && watch_gap) {
        abandon();
        NotifyStatus();
        return;
    }
    const std::wstring publish_path = ShardPath(path, next_slot);
    std::shared_ptr<Shard> shard;
    if (failure == ERROR_SUCCESS && builder.Save(publish_path))
        shard = Shard::Open(publish_path, next_slot);
    if (failure == ERROR_SUCCESS && !shard) {
        failure = ERROR_WRITE_FAULT;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (generation != generation_) {
            for (auto& root : roots_) {
                if (!EqualPath(root.info.path, path)) continue;
                root.info.building = false;
                root.info.state = L"等待重新扫描";
                dirty_roots_.insert(root.info.path);
                break;
            }
            return;
        }
        for (auto& root : roots_) {
            if (!EqualPath(root.info.path, path)) continue;
            root.info.building = false;
            if (failure == ERROR_SUCCESS && root.overlay && root.overlay->watch_started >= crawl_start) {
                // This crawl started before notification coverage. It may
                // have missed changes in directories already enumerated.
                // Keep the old snapshot until a watched crawl completes.
                shard.reset();
                DeleteFileW(publish_path.c_str());
                root.info.progress = 0;
                root.info.indexed_items = root.shard ? root.shard->count : 0;
                root.info.state = L"等待重新扫描";
                dirty_roots_.insert(root.info.path);
                break;
            }
            root.info.progress = failure == ERROR_SUCCESS ? 100 : 0;
            root.last_crawl_end = std::chrono::steady_clock::now();
            root.last_crawl_failed = failure != ERROR_SUCCESS;
            if (failure == ERROR_SUCCESS) {
                root.last_crawl = root.last_crawl_end - crawl_start;
                const auto old_shard = root.shard;
                root.shard = std::move(shard);
                root.info.online = true;
                root.info.indexed_items = root.shard ? root.shard->count : 0;
                root.info.state = (root.info.watching ? L"实时监视 · " : L"已同步 · ") +
                    std::to_wstring(root.info.indexed_items) + L" 项";
                root.info.error.clear();
                if (old_shard && old_shard->slot != root.shard->slot)
                    DeleteFileW(ShardPath(path, old_shard->slot).c_str());
                DeleteFileW(ShardPath(path).c_str());
                if (root.overlay) {
                    // The new shard already reflects everything seen before
                    // the crawl started.
                    Overlay& overlay = *root.overlay;
                    std::erase_if(overlay.entries, [&](const auto& item) {
                        return item.second.seen < crawl_start;
                    });
                    std::erase_if(overlay.removed, [&](const auto& item) {
                        return item.second < crawl_start;
                    });
                    overlay.view.reset();
                    // Changes dropped while the overlay was full may postdate
                    // what this crawl saw.
                    if (overlay.last_skipped >= crawl_start && !root.change_pending) {
                        root.change_pending = true;
                        root.change_first = root.change_last = root.last_crawl_end;
                    }
                    overlay.last_skipped = {};
                }
            } else {
                root.info.online = server_accessible && root.shard != nullptr;
                root.info.indexed_items = root.shard ? root.shard->count : 0;
                root.info.state = server_accessible && root.shard
                    ? L"同步失败 · 继续使用旧索引"
                    : root.shard ? L"服务器离线 · 已保留索引" : L"无法访问";
                root.info.error = Win32Message(failure);
            }
            break;
        }
    }
    NotifyStatus();
    if (watch_wake_event_) SetEvent(watch_wake_event_);
}

void NetworkIndex::CrawlLoop() {
    while (running_) {
        SeedPendingChanges();
        changes_.Flush(false);
        ScanPendingSubtrees();
        std::vector<std::wstring> work;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mu_);
            if (!running_) return;
            const auto now = std::chrono::steady_clock::now();
            const auto next = CollectDueRootsLocked(now, work);
            const bool scans_pending = std::any_of(roots_.begin(), roots_.end(), [](const RootState& root) {
                return root.overlay && !root.overlay->pending_scans.empty();
            });
            if (work.empty() && scans_pending) continue;
            if (work.empty()) {
                // Root edits, rebuilds, observed changes, lost watches and
                // Stop notify; otherwise sleep until the next crawl is due.
                crawl_cv_.wait_until(lock, (std::min)(next, now + kCrawlIdleWake));
                continue;
            }
            generation = generation_;
        }
        for (const auto& root : work) {
            if (!running_) return;
            BuildRoot(root, generation);
        }
    }
}

void NetworkIndex::SetWatching(const std::wstring& path, bool watching) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& root : roots_) {
            if (!EqualPath(root.info.path, path)) continue;
            if (watching && !root.info.watching) {
                if (!root.overlay) root.overlay = std::make_shared<Overlay>();
                root.overlay->watch_started = std::chrono::steady_clock::now();
                // Also reconcile when the first baseline was published
                // before the watch could be established.
                dirty_roots_.insert(root.info.path);
                if (!root.info.building) root.info.state = L"监视已启动 · 等待校验";
            }
            root.info.watching = watching;
            if (watching) {
                root.info.online = true;
            }
            break;
        }
    }
    crawl_cv_.notify_one();
}

void NetworkIndex::WatchLoop() {
    constexpr DWORD kNotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
    // Heap-allocated so the OVERLAPPED and buffer never move while a read is
    // pending.
    struct Watch {
        std::wstring path;
        HANDLE directory = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED overlapped{};
        std::vector<uint8_t> buffer = std::vector<uint8_t>(32 * 1024);

        Watch() = default;
        Watch(const Watch&) = delete;
        Watch& operator=(const Watch&) = delete;
        ~Watch() {
            if (directory != INVALID_HANDLE_VALUE) {
                CancelIoEx(directory, &overlapped);
                DWORD transferred = 0;
                GetOverlappedResult(directory, &overlapped, &transferred, TRUE);
                CloseHandle(directory);
            }
            if (event) CloseHandle(event);
        }
        bool Arm() {
            ResetEvent(event);
            overlapped = OVERLAPPED{};
            overlapped.hEvent = event;
            return ReadDirectoryChangesW(directory, buffer.data(), static_cast<DWORD>(buffer.size()),
                                         TRUE, kNotifyFilter, nullptr, &overlapped, nullptr) != FALSE;
        }
    };
    std::vector<std::unique_ptr<Watch>> watches;
    bool refresh = true;
    while (running_) {
        if (refresh) {
            // Roots were added/removed or a crawl finished. Keep watches that
            // are still wanted armed (no gap) and only open the new ones.
            refresh = false;
            std::vector<std::wstring> paths;
            {
                std::lock_guard<std::mutex> lock(mu_);
                for (const auto& root : roots_) {
                    if (paths.size() + 1 < MAXIMUM_WAIT_OBJECTS)
                        paths.push_back(root.info.path);
                }
            }
            std::erase_if(watches, [&](const std::unique_ptr<Watch>& watch) {
                return std::none_of(paths.begin(), paths.end(), [&](const std::wstring& path) {
                    return EqualPath(path, watch->path);
                });
            });
            for (const auto& path : paths) {
                if (std::any_of(watches.begin(), watches.end(), [&](const std::unique_ptr<Watch>& watch) {
                        return EqualPath(path, watch->path);
                    })) continue;
                auto watch = std::make_unique<Watch>();
                watch->path = path;
                watch->directory = CreateFileW(LongPath(path).c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
                if (watch->directory == INVALID_HANDLE_VALUE) continue;
                watch->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!watch->event || !watch->Arm()) continue;
                SetWatching(path, true);
                watches.push_back(std::move(watch));
            }
            NotifyStatus();
        }

        std::vector<HANDLE> events;
        if (watch_wake_event_) events.push_back(watch_wake_event_);
        for (const auto& watch : watches) events.push_back(watch->event);
        if (events.empty()) return;
        const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(),
                                                  FALSE, INFINITE);
        if (!running_) return;
        if (wait == WAIT_OBJECT_0 && watch_wake_event_) {
            refresh = true;
            continue;
        }
        const size_t first_watch = watch_wake_event_ ? 1 : 0;
        if (wait < WAIT_OBJECT_0 + first_watch || wait >= WAIT_OBJECT_0 + events.size()) {
            // WAIT_FAILED: rebuild the set rather than spin on a bad handle.
            Sleep(1000);
            refresh = true;
            continue;
        }
        const size_t index = static_cast<size_t>(wait - WAIT_OBJECT_0) - first_watch;
        Watch& watch = *watches[index];
        DWORD bytes = 0;
        const bool read = GetOverlappedResult(watch.directory, &watch.overlapped, &bytes, FALSE) != FALSE;
        const DWORD read_error = read ? ERROR_SUCCESS : GetLastError();
        // Observed changes go to the overlay; only lost events (overflow with
        // 0 bytes, failed read) need a crawl.
        if (read && bytes) {
            ObserveChanges(watch.path, watch.buffer.data(), bytes);
        } else {
            changes_.Gap();
            NoteRootChanged(watch.path);
        }
        // Overflow keeps the handle usable; any other failure (server gone,
        // session reset) retires the watch and the root falls back to
        // scheduled reconciles until a crawl brings it back.
        if ((!read && read_error != ERROR_NOTIFY_ENUM_DIR) || !watch.Arm()) {
            const std::wstring path = watch.path;
            watches.erase(watches.begin() + static_cast<std::ptrdiff_t>(index));
            SetWatching(path, false);
            crawl_cv_.notify_one();
            NotifyStatus();
        }
    }
}

void NetworkIndex::SearchAsync(const Query& query, uint32_t id) {
    if (!running_) return;
    latest_search_id_ = id;
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_query_ = query;
        pending_id_ = id;
        have_pending_search_ = true;
    }
    search_cv_.notify_one();
}

void NetworkIndex::SearchLoop() {
    while (running_) {
        Query query;
        uint32_t id = 0;
        std::vector<std::shared_ptr<Shard>> shards;
        // Parallel to shards: overlay whose changes supersede the shard's
        // records (null for the overlay's own in-memory shard).
        std::vector<std::shared_ptr<const OverlayView>> overlays;
        std::vector<std::wstring> owner_paths;
        std::vector<size_t> shard_owners;
        {
            std::unique_lock<std::mutex> lock(mu_);
            search_cv_.wait(lock, [this] { return !running_ || have_pending_search_; });
            if (!running_) return;
            query = pending_query_;
            id = pending_id_;
            have_pending_search_ = false;
            for (auto& root : roots_) {
                if (!root.info.online || !root.shard) continue;
                const size_t owner = owner_paths.size();
                owner_paths.push_back(root.info.path);
                auto view = OverlayViewLocked(root);
                shards.push_back(root.shard);
                overlays.push_back(view);
                shard_owners.push_back(owner);
                if (view && view->added->count) {
                    shards.push_back(view->added);
                    overlays.push_back(nullptr);
                    shard_owners.push_back(owner);
                }
            }
        }
        SearchResult result;
        const CompiledQuery compiled = ParseQuery(query.needle);
        struct Candidate {
            const Shard* shard = nullptr;
            const NetworkRecord* record = nullptr;
            size_t shard_order = 0;
            size_t record_order = 0;
            int score = 0;
        };
        auto name_of = [](const Candidate& candidate) {
            const auto& record = *candidate.record;
            return std::wstring_view(candidate.shard->pool + record.path_off + record.name_off,
                                     record.name_len);
        };
        auto path_of = [](const Candidate& candidate) {
            const auto& record = *candidate.record;
            return std::wstring_view(candidate.shard->pool + record.path_off, record.path_len);
        };
        auto compare_text = [](std::wstring_view a, std::wstring_view b) {
            const int value = CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                                   b.data(), static_cast<int>(b.size()), TRUE);
            return value == CSTR_LESS_THAN ? -1 : value == CSTR_GREATER_THAN ? 1 : 0;
        };
        auto better = [&](const Candidate& a, const Candidate& b) {
            if (query.rank && a.score != b.score) return a.score > b.score;
            int comparison = 0;
            if (!query.rank && query.sort == ResultSort::Size &&
                a.record->size != b.record->size)
                comparison = a.record->size < b.record->size ? -1 : 1;
            else if (!query.rank && query.sort == ResultSort::Mtime &&
                     a.record->mtime != b.record->mtime)
                comparison = a.record->mtime < b.record->mtime ? -1 : 1;
            else {
                comparison = compare_text(name_of(a), name_of(b));
                if (!comparison) comparison = compare_text(path_of(a), path_of(b));
            }
            if (!query.rank && query.sort_desc) comparison = -comparison;
            if (comparison) return comparison < 0;
            if (a.shard_order != b.shard_order) return a.shard_order < b.shard_order;
            return a.record_order < b.record_order;
        };
        const size_t wanted = query.offset > SIZE_MAX - query.limit
            ? SIZE_MAX : query.offset + query.limit;
        const bool index_order = !query.rank && query.sort == ResultSort::Index;
        // The most specific available root owns its descendants, including
        // its overlay and deletions. Unavailable children leave the parent
        // authoritative. Root directories themselves still belong to their
        // parent because a root's own shard does not enumerate itself.
        const auto owner_of = [&](std::wstring_view path) {
            size_t owner = SIZE_MAX;
            size_t longest = 0;
            for (size_t i = 0; i < owner_paths.size(); ++i) {
                const auto& scope = owner_paths[i];
                if (scope.size() <= longest || path.size() <= scope.size()) continue;
                if (!StartsWithPath(path, scope)) continue;
                owner = i;
                longest = scope.size();
            }
            return owner;
        };
        // Shards can hold millions of records, so the scan is split into
        // fixed chunks matched in parallel. Each chunk keeps its own count
        // plus either its first `wanted` hits (index order) or a local top
        // list; merging in chunk order reproduces the sequential result.
        struct Chunk {
            size_t shard = 0, begin = 0, end = 0;
            size_t total = 0;
            std::vector<Candidate> hits;
        };
        constexpr size_t kChunkRecords = 1u << 16;
        std::vector<Chunk> chunks;
        size_t records = 0;
        for (size_t shard_index = 0; shard_index < shards.size(); ++shard_index) {
            const size_t count = shards[shard_index]->count;
            records += count;
            for (size_t begin = 0; begin < count; begin += kChunkRecords)
                chunks.push_back({shard_index, begin, (std::min)(count, begin + kChunkRecords)});
        }
        const auto scan_chunk = [&](Chunk& chunk) {
            const auto& shard = shards[chunk.shard];
            const OverlayView* overlay = overlays[chunk.shard].get();
            std::priority_queue<Candidate, std::vector<Candidate>, decltype(better)> top(better);
            for (size_t i = chunk.begin; i < chunk.end; ++i) {
                if ((i & 4095u) == 0 && latest_search_id_.load() != id) return;
                const auto& record = shard->records[i];
                const std::wstring_view path(shard->pool + record.path_off, record.path_len);
                if (!query.path_prefix.empty() && !StartsWithPath(path, query.path_prefix)) continue;
                const std::wstring_view name(path.data() + record.name_off, record.name_len);
                if (!MatchNetworkRecord(record, path, name, compiled, query.folders_only)) continue;
                if (owner_of(path) != shard_owners[chunk.shard]) continue;
                if (overlay && overlay->Hides(path)) continue;
                Candidate candidate{shard.get(), &record, chunk.shard, i,
                    query.rank ? RankName(name.data(), static_cast<uint32_t>(name.size()),
                                          (record.flags & kRecordDirectory) != 0, compiled) : 0};
                ++chunk.total;
                if (index_order) {
                    if (chunk.hits.size() < wanted) chunk.hits.push_back(candidate);
                } else if (wanted) {
                    if (top.size() < wanted) top.push(candidate);
                    else if (better(candidate, top.top())) {
                        top.pop();
                        top.push(candidate);
                    }
                }
            }
            if (!index_order) {
                chunk.hits.reserve(top.size());
                while (!top.empty()) {
                    chunk.hits.push_back(top.top());
                    top.pop();
                }
            }
        };
        const unsigned hardware = std::thread::hardware_concurrency();
        unsigned workers = hardware < 2 ? 1u : (std::min)(hardware, 6u);
        if (records < 200000 || chunks.size() < 2) workers = 1;
        workers = (std::min)(workers, static_cast<unsigned>((std::max)(chunks.size(), size_t{1})));
        if (workers <= 1) {
            for (auto& chunk : chunks) {
                scan_chunk(chunk);
                if (latest_search_id_.load() != id) break;
            }
        } else {
            std::atomic<size_t> next_chunk{0};
            std::vector<std::thread> pool;
            pool.reserve(workers);
            for (unsigned w = 0; w < workers; ++w) {
                pool.emplace_back([&] {
                    for (size_t c = next_chunk.fetch_add(1); c < chunks.size(); c = next_chunk.fetch_add(1)) {
                        if (latest_search_id_.load() != id) return;
                        scan_chunk(chunks[c]);
                    }
                });
            }
            for (auto& thread : pool) thread.join();
        }
        size_t total = 0;
        std::vector<Candidate> in_order;
        std::priority_queue<Candidate, std::vector<Candidate>, decltype(better)> top(better);
        if (index_order) in_order.reserve((std::min)(wanted, static_cast<size_t>(4096)));
        for (const auto& chunk : chunks) {
            if (index_order) {
                for (size_t k = 0; k < chunk.hits.size(); ++k) {
                    if (total + k + 1 > query.offset && in_order.size() < query.limit)
                        in_order.push_back(chunk.hits[k]);
                }
            } else {
                for (const auto& candidate : chunk.hits) {
                    if (top.size() < wanted) top.push(candidate);
                    else if (better(candidate, top.top())) {
                        top.pop();
                        top.push(candidate);
                    }
                }
            }
            total += chunk.total;
        }
        if (latest_search_id_.load() != id) continue;
        result.total = total;
        std::vector<Candidate> selected;
        if (index_order) {
            selected = std::move(in_order);
        } else {
            std::vector<Candidate> ranked;
            ranked.reserve(top.size());
            while (!top.empty()) {
                ranked.push_back(top.top());
                top.pop();
            }
            std::sort(ranked.begin(), ranked.end(), better);
            const size_t begin = (std::min)(query.offset, ranked.size());
            const size_t end = (std::min)(ranked.size(), begin + query.limit);
            selected.reserve(end - begin);
            for (size_t i = begin; i < end; ++i) selected.push_back(ranked[i]);
        }
        result.hits.reserve(selected.size());
        for (const auto& candidate : selected) {
            Hit hit;
            hit.path.assign(path_of(candidate));
            hit.name.assign(name_of(candidate));
            hit.is_dir = (candidate.record->flags & kRecordDirectory) != 0;
            hit.size = candidate.record->size;
            hit.mtime = candidate.record->mtime;
            result.hits.push_back(std::move(hit));
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (latest_search_id_.load() != id) continue;
            result_id_ = id;
            result_ = std::move(result);
        }
        if (notify_ && search_msg_) PostMessageW(notify_, search_msg_, id, 0);
    }
}

bool NetworkIndex::TakeResult(uint32_t id, SearchResult& result) {
    std::lock_guard<std::mutex> lock(mu_);
    if (result_id_ != id) return false;
    result = std::move(result_);
    result_id_ = 0;
    return true;
}

bool IsNetworkFolderPath(const std::wstring& raw) {
    std::wstring_view path(raw);
    if (path.size() >= 8 && CompareStringOrdinal(path.data(), 8, L"\\\\?\\UNC\\", 8, TRUE) == CSTR_EQUAL)
        return true;
    if (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\")) path.remove_prefix(4);
    else if (path.size() >= 3 && path[0] == L'\\' && path[1] == L'\\') return true;
    if (path.size() >= 2 && path[1] == L':' && iswalpha(path[0])) {
        const wchar_t root[4] = {path[0], L':', L'\\', 0};
        return ::pulse::fs::IsRemoteLikeDrive(root);  // mapped drive or cloud mount
    }
    return false;
}

bool NetworkRootsCover(const std::vector<NetworkRootInfo>& roots, const std::wstring& path,
                       bool require_ready) {
    std::wstring_view value(path);
    while (value.size() > 3 && value.back() == L'\\') value.remove_suffix(1);
    for (const auto& root : roots) {
        // A root still on its first crawl (or re-crawling) has no complete
        // shard to answer from; the live walk stays correct meanwhile.
        if (require_ready && (!root.online || root.building || root.indexed_items == 0)) continue;
        if (!root.path.empty() && StartsWithPath(value, root.path)) return true;
    }
    return false;
}

void LiveNetworkWalk(const std::wstring& raw_folder, const std::wstring& needle, bool folders_only,
                     LiveNetworkMatches& out, std::mutex& out_mutex,
                     const std::function<bool()>& cancelled,
                     const std::function<void()>& progress) {
    Query query;
    query.path_prefix = raw_folder;
    query.needle = needle;
    query.folders_only = folders_only;
    query.rank = false;
    LiveNetworkWalk(query, out, out_mutex, cancelled, progress);
}

bool CanReuseLiveNetworkWalk(const Query& previous, const Query& next) {
    return previous.session_id == next.session_id && previous.subscribe == next.subscribe &&
        previous.needle == next.needle && previous.path_prefix == next.path_prefix &&
        previous.folders_only == next.folders_only && previous.rank == next.rank &&
        previous.sort == next.sort && previous.sort_desc == next.sort_desc;
}

void LiveNetworkWalk(const Query& query, LiveNetworkMatches& out, std::mutex& out_mutex,
                     const std::function<bool()>& cancelled,
                     const std::function<void()>& progress) {
    std::wstring folder = query.path_prefix;
    std::replace(folder.begin(), folder.end(), L'/', L'\\');
    while (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();
    const CompiledQuery compiled = ParseQuery(query.needle);
    {
        std::lock_guard lock(out_mutex);
        out = {};
        out.order = query;
    }
    std::vector<std::wstring> stack{folder};
    ULONGLONG last_progress = GetTickCount64();
    bool stopped = false;
    bool unreported = false;
    const bool ordered = query.rank || query.sort != ResultSort::Index;
    const auto better = [&](const Hit& a, const Hit& b) { return BetterHit(a, b, query, compiled); };
    constexpr size_t kBatchSize = 256;
    std::vector<Hit> found;
    found.reserve(kBatchSize);
    const auto flush = [&] {
        if (!found.empty()) {
            std::lock_guard lock(out_mutex);
            out.total += found.size();
            for (auto& hit : found) {
                if (out.hits.size() < kSearchPageCap) {
                    out.hits.push_back(std::move(hit));
                    if (ordered) std::push_heap(out.hits.begin(), out.hits.end(), better);
                } else if (ordered && better(hit, out.hits.front())) {
                    std::pop_heap(out.hits.begin(), out.hits.end(), better);
                    out.hits.back() = std::move(hit);
                    std::push_heap(out.hits.begin(), out.hits.end(), better);
                }
            }
            found.clear();
            unreported = true;
        }
        const ULONGLONG now = GetTickCount64();
        if (unreported && progress && now - last_progress >= 250) {
            last_progress = now;
            unreported = false;
            progress();
        }
    };
    while (!stack.empty() && !stopped) {
        if (cancelled && cancelled()) {
            stopped = true;
            break;
        }
        std::wstring directory = std::move(stack.back());
        stack.pop_back();
        std::wstring pattern = LongPath(directory);
        if (pattern.back() != L'\\') pattern += L'\\';
        pattern += L'*';
        WIN32_FIND_DATAW find{};
        HANDLE handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &find,
                                         FindExSearchNameMatch, nullptr,
                                         FIND_FIRST_EX_LARGE_FETCH);
        if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER) {
            handle = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &find,
                                      FindExSearchNameMatch, nullptr, 0);
        }
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES) {
                std::lock_guard<std::mutex> lock(out_mutex);
                if (!out.error) out.error = error ? error : ERROR_GEN_FAILURE;
            }
            continue;
        }
        const std::wstring base = directory.back() == L'\\' ? directory : directory + L'\\';
        do {
            if (cancelled && cancelled()) { stopped = true; break; }
            if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0)
                continue;
            const bool is_dir = (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            std::wstring full = base + find.cFileName;
            NetworkRecord record{};
            record.flags = is_dir ? kRecordDirectory : 0;
            record.size = is_dir ? 0
                : (static_cast<uint64_t>(find.nFileSizeHigh) << 32) | find.nFileSizeLow;
            record.mtime = FileTimeValue(find.ftLastWriteTime);
            const std::wstring_view name(find.cFileName);
            if (MatchNetworkRecord(record, full, name, compiled, query.folders_only))
                found.push_back(Hit{full, std::wstring(name), is_dir, record.size, record.mtime});
            if (found.size() >= kBatchSize) flush();
            if (is_dir && !(find.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                stack.push_back(std::move(full));
        } while (FindNextFileW(handle, &find));
        const DWORD enumeration_error = GetLastError();
        if (!stopped && enumeration_error != ERROR_NO_MORE_FILES) {
            std::lock_guard<std::mutex> lock(out_mutex);
            if (!out.error) out.error = enumeration_error ? enumeration_error : ERROR_GEN_FAILURE;
        }
        FindClose(handle);
        flush();
    }
    std::lock_guard<std::mutex> lock(out_mutex);
    out.finished = true;
    out.complete = !stopped;
}

SearchResult SelectLiveNetworkHits(const Query& query, const LiveNetworkMatches& matches) {
    SearchResult result;
    result.total = matches.total;
    result.error = matches.error;
    if (matches.total > matches.hits.size() &&
        (query.rank != matches.order.rank || query.sort != matches.order.sort ||
         query.sort_desc != matches.order.sort_desc)) {
        // A differently ordered prefix cannot answer this page. Production
        // cache reuse rejects this combination and starts another walk.
        result.error = ERROR_INVALID_PARAMETER;
        return result;
    }
    const CompiledQuery compiled = ParseQuery(query.needle);
    struct Ranked {
        const Hit* hit = nullptr;
        int score = 0;
    };
    std::vector<Ranked> order;
    order.reserve(matches.hits.size());
    for (const auto& hit : matches.hits) {
        order.push_back({&hit, query.rank
            ? RankName(hit.name.data(), static_cast<uint32_t>(hit.name.size()), hit.is_dir, compiled)
            : 0});
    }
    if (query.rank || query.sort != ResultSort::Index) {
        auto better = [&](const Ranked& a, const Ranked& b) {
            if (query.rank) {
                if (a.score != b.score) return a.score > b.score;
                return CompareName(*a.hit, *b.hit) < 0;
            }
            return BetterHit(*a.hit, *b.hit, query, compiled);
        };
        const size_t wanted = query.offset > SIZE_MAX - query.limit
            ? SIZE_MAX : query.offset + query.limit;
        if (order.size() > wanted) {
            std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(wanted),
                              order.end(), better);
            order.resize(wanted);
        } else {
            std::sort(order.begin(), order.end(), better);
        }
    }
    const size_t begin = (std::min)(query.offset, order.size());
    const size_t end = begin + (std::min)(query.limit, order.size() - begin);
    result.hits.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) result.hits.push_back(*order[i].hit);
    return result;
}

SearchResult MergeSearchResults(const Query& query, SearchResult local,
                                SearchResult network, bool deduplicate_paths) {
    SearchResult result;
    result.error = local.error ? local.error : network.error;
    result.total = local.total + network.total;
    result.hits.reserve(local.hits.size() + network.hits.size());
    for (auto& hit : local.hits) result.hits.push_back(std::move(hit));
    for (auto& hit : network.hits) result.hits.push_back(std::move(hit));
    const CompiledQuery compiled = ParseQuery(query.needle);
    if (query.rank || query.sort != ResultSort::Index) {
        std::sort(result.hits.begin(), result.hits.end(), [&](const Hit& a, const Hit& b) {
            return BetterHit(a, b, query, compiled);
        });
    }
    if (deduplicate_paths) {
        std::vector<Hit> unique;
        unique.reserve(result.hits.size());
        for (auto& hit : result.hits) {
            if (std::any_of(unique.begin(), unique.end(), [&](const Hit& existing) {
                return CompareStringOrdinal(existing.path.c_str(), -1, hit.path.c_str(), -1, TRUE) == CSTR_EQUAL &&
                    CompareStringOrdinal(existing.recycle_path.c_str(), -1, hit.recycle_path.c_str(), -1, TRUE) == CSTR_EQUAL;
            })) {
                if (result.total) --result.total;
            } else unique.push_back(std::move(hit));
        }
        result.hits = std::move(unique);
    }
    const size_t begin = (std::min)(query.offset, result.hits.size());
    const size_t end = (std::min)(result.hits.size(), begin + query.limit);
    if (begin) result.hits.erase(result.hits.begin(), result.hits.begin() + begin);
    if (result.hits.size() > end - begin) result.hits.resize(end - begin);
    return result;
}

} // namespace pulse::index

namespace pulse::index {
void LiveNetworkWalk(const std::wstring& folder, const std::wstring& needle, bool folders_only,
    LiveNetworkMatches& out, std::mutex& mutex, const std::function<bool()>& cancelled,
    const std::function<void()>& progress, const Query* selection) {
    if (selection) { auto query = *selection; query.path_prefix = folder; query.needle = needle;
        query.folders_only = folders_only; LiveNetworkWalk(query, out, mutex, cancelled, progress); }
    else LiveNetworkWalk(folder, needle, folders_only, out, mutex, cancelled, progress);
}
}
