#include "network_index.h"
#include "index_config.h"
#include "index_query.h"
#include "../common/json_utils.h"
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <queue>
#include <sstream>
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
constexpr auto kReconcileInterval = std::chrono::minutes(5);
constexpr auto kCrawlWakeInterval = std::chrono::milliseconds(250);

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

bool DecodeUtf8(const std::vector<uint8_t>& bytes, std::wstring& text) {
    size_t offset = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        offset = 3;
    if (offset == bytes.size()) {
        text.clear();
        return true;
    }
    const int byte_count = static_cast<int>(bytes.size() - offset);
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           reinterpret_cast<const char*>(bytes.data() + offset),
                                           byte_count, nullptr, 0);
    if (chars <= 0) return false;
    text.resize(static_cast<size_t>(chars));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               reinterpret_cast<const char*>(bytes.data() + offset), byte_count,
                               text.data(), chars) == chars;
}

bool EncodeUtf8(const std::wstring& text, std::vector<uint8_t>& bytes) {
    if (text.empty()) {
        bytes.clear();
        return true;
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return false;
    bytes.resize(static_cast<size_t>(size));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                               static_cast<int>(text.size()),
                               reinterpret_cast<char*>(bytes.data()), size,
                               nullptr, nullptr) == size;
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

std::vector<std::wstring> ExtractStringArray(const std::wstring& json,
                                             const std::wstring& key) {
    std::vector<std::wstring> result;
    const std::wstring marker = L"\"" + key + L"\"";
    size_t at = json.find(marker);
    if (at == std::wstring::npos || (at = json.find(L'[', at + marker.size())) == std::wstring::npos)
        return result;
    ++at;
    while (at < json.size()) {
        while (at < json.size() && iswspace(json[at])) ++at;
        if (at == json.size() || json[at] == L']') break;
        if (json[at++] != L'\"') return {};
        std::wstring value;
        while (at < json.size() && json[at] != L'\"') {
            if (json[at] != L'\\') {
                value.push_back(json[at++]);
                continue;
            }
            if (++at >= json.size()) return {};
            switch (json[at]) {
            case L'\"': value.push_back(L'\"'); break;
            case L'\\': value.push_back(L'\\'); break;
            case L'n': value.push_back(L'\n'); break;
            case L'r': value.push_back(L'\r'); break;
            case L't': value.push_back(L'\t'); break;
            default: value.push_back(json[at]); break;
            }
            ++at;
        }
        if (at >= json.size()) return {};
        ++at;
        result.push_back(std::move(value));
        while (at < json.size() && iswspace(json[at])) ++at;
        if (at < json.size() && json[at] == L',') ++at;
    }
    return result;
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
    auto match_term = [&](const Term& term) {
        if (term.folder && !is_dir) return false;
        if (term.file && is_dir) return false;
        if (!term.exts.empty()) {
            bool ok = MatchExt(name.data(), static_cast<uint32_t>(name.size()), term);
            if (term.ext_not) ok = !ok;
            if (!ok) return false;
        }
        if (term.size_how != SizeHow::Any) {
            bool ok = MatchSize(record.size, term);
            if (term.size_not) ok = !ok;
            if (!ok) return false;
        }
        if (term.date_how != DateHow::Any) {
            bool ok = record.mtime && MatchDate(record.mtime, term);
            if (term.date_not) ok = !ok;
            if (!ok) return false;
        }
        if (term.name_how != NameHow::Any) {
            bool ok = false;
            if (!term.name_in_path) {
                ok = MatchName(name.data(), static_cast<uint32_t>(name.size()), term);
            } else if (term.name_how == NameHow::Wildcard) {
                ok = WildcardFolded(path.data(), static_cast<uint32_t>(path.size()), term.name);
            } else {
                size_t begin = 0;
                while (begin < path.size()) {
                    const size_t end = path.find(L'\\', begin);
                    const size_t length = (end == std::wstring_view::npos ? path.size() : end) - begin;
                    if (length && MatchName(path.data() + begin, static_cast<uint32_t>(length), term)) {
                        ok = true;
                        break;
                    }
                    if (end == std::wstring_view::npos) break;
                    begin = end + 1;
                }
            }
            if (term.name_not) ok = !ok;
            if (!ok) return false;
        }
        return true;
    };
    for (const auto& group : query.groups) {
        bool matched = true;
        for (const auto& term : group) {
            if (!match_term(term)) {
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
        WNetGetUniversalNameW(path.c_str(), UNIVERSAL_NAME_INFO_LEVEL, nullptr, &bytes);
        if (GetLastError() == ERROR_MORE_DATA && bytes) {
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
        roots.clear();
        SetError(error, L"无法定位当前用户配置目录");
        return false;
    }
    return LoadNetworkRootsFile(path, roots, error);
}

bool LoadNetworkRootsFile(const std::wstring& path, std::vector<std::wstring>& roots,
                          std::wstring* error) {
    roots.clear();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetLastError() == ERROR_FILE_NOT_FOUND) return true;
    std::vector<uint8_t> bytes;
    if (!ReadBytes(path, bytes, error)) return false;
    std::wstring json;
    if (!DecodeUtf8(bytes, json)) {
        SetError(error, L"网络索引配置不是有效的 UTF-8 文件");
        return false;
    }
    if (json.find(L'{') == std::wstring::npos) {
        SetError(error, L"网络索引配置已损坏");
        return false;
    }
    for (const auto& raw : ExtractStringArray(json, L"roots")) {
        std::wstring root = NormalizeNetworkRoot(raw);
        if (root.empty()) continue;
        if (std::none_of(roots.begin(), roots.end(), [&](const auto& existing) {
                return EqualPath(existing, root);
            })) roots.push_back(std::move(root));
    }
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
    if (!EncodeUtf8(json, bytes)) {
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
    std::lock_guard lock(change_seed_mutex_);
    if (seed) change_seed_owners_.insert(owner);
    if (!enabled) change_seed_owners_.erase(owner);
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
    std::wstring old_path;
    size_t offset = 0;
    while (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= bytes) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(data + offset);
        if (info->FileNameLength % sizeof(wchar_t) ||
            info->FileNameLength > bytes - offset - offsetof(FILE_NOTIFY_INFORMATION, FileName)) { changes_.Gap(); break; }
        ChangeRecord event;
        event.path = root + L"\\" + std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t));
        if (info->Action == FILE_ACTION_RENAMED_OLD_NAME) old_path = event.path;
        else {
            const auto attributes = GetFileAttributesW(LongPath(event.path).c_str());
            event.is_dir = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (info->Action == FILE_ACTION_ADDED) event.kind = ChangeKind::Created;
            else if (info->Action == FILE_ACTION_REMOVED) event.kind = ChangeKind::Deleted;
            else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) { event.kind = ChangeKind::Renamed; event.old_path = std::move(old_path); }
            changes_.Record(std::move(event));
        }
        if (!info->NextEntryOffset) break;
        if (info->NextEntryOffset > bytes - offset || info->NextEntryOffset < offsetof(FILE_NOTIFY_INFORMATION, FileName)) { changes_.Gap(); break; }
        offset += info->NextEntryOffset;
    }
    // The SMB watcher is rearmed after reconciliation; its gap is explicit.
    changes_.Gap();
}

void NetworkIndex::Start(HWND notify, UINT status_msg, UINT search_msg) {
    changes_.Open(NetworkDataDir());
    Stop();
    notify_ = notify;
    status_msg_ = status_msg;
    search_msg_ = search_msg;
    std::vector<std::wstring> configured;
    std::wstring error;
    LoadNetworkRoots(configured, &error);
    {
        std::lock_guard<std::mutex> lock(mu_);
        roots_.clear();
        dirty_roots_.clear();
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
            roots_.push_back(std::move(state));
            dirty_roots_.insert(path);
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

bool NetworkIndex::AddRoot(const std::wstring& raw, std::wstring* error) {
    const std::wstring path = NormalizeNetworkRoot(raw);
    if (path.empty()) {
        SetError(error, L"请选择服务器共享中的文件夹（UNC 路径）");
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
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
            root.info.state = L"等待重新扫描";
            dirty_roots_.insert(root.info.path);
            break;
        }
    };
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& root : roots_) {
            if (!EqualPath(root.info.path, path)) continue;
            root.info.building = true;
            root.info.watching = false;
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
    bool storage_failure = !builder.Valid();
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
            if (EqualPath(directory, path)) {
                failure = GetLastError();
                root_accessible = false;
            }
            continue;
        }
        do {
            if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0)
                continue;
            std::wstring full = directory + L"\\" + find.cFileName;
            if (!builder.Add(full, find)) {
                failure = ERROR_WRITE_FAULT;
                storage_failure = true;
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
        FindClose(handle);
    }
    if (!running_) return;
    if (!RootStillCurrent(path, generation)) {
        abandon();
        return;
    }

    const bool server_accessible = root_accessible && (failure == ERROR_SUCCESS || storage_failure);
    int next_slot = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& root : roots_) {
            if (EqualPath(root.info.path, path) && root.shard && root.shard->slot == 0)
                next_slot = 1;
        }
    }
    const std::wstring publish_path = ShardPath(path, next_slot);
    std::shared_ptr<Shard> shard;
    if (failure == ERROR_SUCCESS && builder.Save(publish_path))
        shard = Shard::Open(publish_path, next_slot);
    if (failure == ERROR_SUCCESS && !shard) {
        failure = ERROR_WRITE_FAULT;
        storage_failure = true;
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
            root.info.progress = failure == ERROR_SUCCESS ? 100 : 0;
            if (failure == ERROR_SUCCESS) {
                const auto old_shard = root.shard;
                root.shard = std::move(shard);
                root.info.online = true;
                root.info.indexed_items = root.shard ? root.shard->count : 0;
                root.info.state = L"已同步 · " + std::to_wstring(root.info.indexed_items) + L" 项";
                root.info.error.clear();
                if (old_shard && old_shard->slot != root.shard->slot)
                    DeleteFileW(ShardPath(path, old_shard->slot).c_str());
                DeleteFileW(ShardPath(path).c_str());
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
    auto next_reconcile = std::chrono::steady_clock::now() + kReconcileInterval;
    while (running_) {
        SeedPendingChanges();
        changes_.Flush();
        std::vector<std::wstring> work;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mu_);
            crawl_cv_.wait_for(lock, kCrawlWakeInterval, [this] {
                return !running_ || !dirty_roots_.empty();
            });
            if (!running_) return;
            const auto now = std::chrono::steady_clock::now();
            if (dirty_roots_.empty() && now < next_reconcile) continue;
            if (now >= next_reconcile) {
                for (const auto& root : roots_) dirty_roots_.insert(root.info.path);
                next_reconcile = now + kReconcileInterval;
            }
            work.assign(dirty_roots_.begin(), dirty_roots_.end());
            dirty_roots_.clear();
            generation = generation_;
        }
        for (const auto& root : work) {
            if (!running_) return;
            BuildRoot(root, generation);
        }
    }
}

void NetworkIndex::WatchLoop() {
    struct Watch {
        std::wstring path;
        HANDLE directory = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED overlapped{};
        std::vector<uint8_t> buffer = std::vector<uint8_t>(32 * 1024);

        Watch() = default;
        Watch(const Watch&) = delete;
        Watch& operator=(const Watch&) = delete;
        Watch(Watch&& other) noexcept
            : path(std::move(other.path)), directory(other.directory), event(other.event),
              overlapped(other.overlapped), buffer(std::move(other.buffer)) {
            other.directory = INVALID_HANDLE_VALUE;
            other.event = nullptr;
            overlapped.hEvent = event;
        }
        ~Watch() {
            if (directory != INVALID_HANDLE_VALUE) {
                CancelIoEx(directory, &overlapped);
                DWORD transferred = 0;
                GetOverlappedResult(directory, &overlapped, &transferred, TRUE);
                CloseHandle(directory);
            }
            if (event) CloseHandle(event);
        }
    };

    while (running_) {
        std::vector<std::wstring> paths;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& root : roots_) {
                if (root.info.online && paths.size() + 1 < MAXIMUM_WAIT_OBJECTS)
                    paths.push_back(root.info.path);
            }
        }
        std::vector<Watch> watches;
        std::vector<HANDLE> events;
        if (watch_wake_event_) events.push_back(watch_wake_event_);
        watches.reserve(paths.size());
        for (const auto& path : paths) {
            watches.emplace_back();
            Watch& watch = watches.back();
            watch.path = path;
            watch.directory = CreateFileW(LongPath(path).c_str(), FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (watch.directory == INVALID_HANDLE_VALUE) {
                watches.pop_back();
                continue;
            }
            watch.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!watch.event) {
                watches.pop_back();
                continue;
            }
            watch.overlapped.hEvent = watch.event;
            if (!ReadDirectoryChangesW(watch.directory, watch.buffer.data(),
                    static_cast<DWORD>(watch.buffer.size()), TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                    nullptr, &watch.overlapped, nullptr)) {
                watches.pop_back();
                continue;
            }
            events.push_back(watch.event);
            {
                std::lock_guard<std::mutex> lock(mu_);
                for (auto& root : roots_) {
                    if (!EqualPath(root.info.path, path)) continue;
                    root.info.watching = true;
                    root.info.state = L"实时监视 · " +
                        std::to_wstring(root.info.indexed_items) + L" 项";
                    break;
                }
            }
        }
        NotifyStatus();
        if (events.empty()) return;
        const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(),
                                                  FALSE, INFINITE);
        if (!running_) return;
        if (wait > WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + events.size()) {
            const size_t index = static_cast<size_t>(wait - WAIT_OBJECT_0 - 1);
            if (index < watches.size()) {
                DWORD bytes = 0;
                if (GetOverlappedResult(watches[index].directory, &watches[index].overlapped, &bytes, FALSE) && bytes)
                    ObserveChanges(watches[index].path, watches[index].buffer.data(), bytes);
                else changes_.Gap();
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    dirty_roots_.insert(watches[index].path);
                    for (auto& root : roots_) {
                        if (EqualPath(root.info.path, watches[index].path)) {
                            root.info.watching = false;
                            break;
                        }
                    }
                }
                crawl_cv_.notify_one();
            }
        }
        // A root changed, was added/removed, or completed its first crawl.
        // Recreate all overlapped requests so the watched set stays coherent.
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
        {
            std::unique_lock<std::mutex> lock(mu_);
            search_cv_.wait(lock, [this] { return !running_ || have_pending_search_; });
            if (!running_) return;
            query = pending_query_;
            id = pending_id_;
            have_pending_search_ = false;
            for (const auto& root : roots_)
                if (root.info.online && root.shard) shards.push_back(root.shard);
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
        std::priority_queue<Candidate, std::vector<Candidate>, decltype(better)> top(better);
        std::vector<Candidate> in_order;
        if (!query.rank && query.sort == ResultSort::Index)
            in_order.reserve((std::min)(wanted, static_cast<size_t>(4096)));
        size_t total = 0;
        for (size_t shard_index = 0; shard_index < shards.size(); ++shard_index) {
            const auto& shard = shards[shard_index];
            for (size_t i = 0; i < shard->count; ++i) {
                if ((i & 4095u) == 0 && latest_search_id_.load() != id) break;
                const auto& record = shard->records[i];
                const std::wstring_view path(shard->pool + record.path_off, record.path_len);
                if (!StartsWithPath(path, query.path_prefix)) continue;
                const std::wstring_view name(path.data() + record.name_off, record.name_len);
                if (!MatchNetworkRecord(record, path, name, compiled, query.folders_only)) continue;
                Candidate candidate{shard.get(), &record, shard_index, i,
                    query.rank ? RankName(name.data(), static_cast<uint32_t>(name.size()),
                                          (record.flags & kRecordDirectory) != 0, compiled) : 0};
                ++total;
                if (!query.rank && query.sort == ResultSort::Index) {
                    if (total > query.offset && in_order.size() < query.limit)
                        in_order.push_back(candidate);
                } else if (wanted) {
                    if (top.size() < wanted) top.push(candidate);
                    else if (better(candidate, top.top())) {
                        top.pop();
                        top.push(candidate);
                    }
                }
            }
            if (latest_search_id_.load() != id) break;
        }
        if (latest_search_id_.load() != id) continue;
        result.total = total;
        std::vector<Candidate> selected;
        if (!query.rank && query.sort == ResultSort::Index) {
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

SearchResult MergeSearchResults(const Query& query, SearchResult local,
                                SearchResult network) {
    SearchResult result;
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
    const size_t begin = (std::min)(query.offset, result.hits.size());
    const size_t end = (std::min)(result.hits.size(), begin + query.limit);
    if (begin) result.hits.erase(result.hits.begin(), result.hits.begin() + begin);
    if (result.hits.size() > end - begin) result.hits.resize(end - begin);
    return result;
}

} // namespace pulse::index
