// zip_entry.cpp - see zip_entry.h.
#include "zip_entry.h"
#include <cstdint>
#include <map>
#include <type_traits>

namespace pulse::preview {
namespace {

struct archive;
struct archive_entry;

constexpr int kArchiveOk = 0;
constexpr int kArchiveEof = 1;
constexpr int kArchiveWarn = -20;

struct ZipApi {
    HMODULE module = nullptr;
    archive* (*read_new)() = nullptr;
    int (*support_format_zip_seekable)(archive*) = nullptr;
    int (*open_filename_w)(archive*, const wchar_t*, size_t) = nullptr;
    int (*next_header)(archive*, archive_entry**) = nullptr;
    int64_t (*read_data)(archive*, void*, size_t) = nullptr;  // la_ssize_t on x64
    int (*read_free)(archive*) = nullptr;
    const char* (*pathname_utf8)(archive_entry*) = nullptr;
    const wchar_t* (*pathname_w)(archive_entry*) = nullptr;
    int64_t (*entry_size)(archive_entry*) = nullptr;
    int (*size_is_set)(archive_entry*) = nullptr;
    bool ready = false;

    ZipApi() {
        module = LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
            return fn != nullptr;
        };
        ready = load(read_new, "archive_read_new") &&
            load(support_format_zip_seekable, "archive_read_support_format_zip_seekable") &&
            load(open_filename_w, "archive_read_open_filename_w") &&
            load(next_header, "archive_read_next_header") &&
            load(read_data, "archive_read_data") &&
            load(read_free, "archive_read_free") &&
            load(pathname_utf8, "archive_entry_pathname_utf8") &&
            load(pathname_w, "archive_entry_pathname_w") &&
            load(entry_size, "archive_entry_size") &&
            load(size_is_set, "archive_entry_size_is_set");
    }
};

void SetError(std::wstring* error, const wchar_t* text) {
    if (error) *error = text;
}

char AsciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool SameName(std::string_view a, std::string_view b) {
    if (!a.empty() && a.front() == '/') a.remove_prefix(1);
    if (!b.empty() && b.front() == '/') b.remove_prefix(1);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i] == '\\' ? '/' : a[i];
        char y = b[i] == '\\' ? '/' : b[i];
        if (AsciiLower(x) != AsciiLower(y)) return false;
    }
    return true;
}

std::string EntryName(const ZipApi& api, archive_entry* entry) {
    if (const char* utf8 = api.pathname_utf8(entry)) return utf8;
    const wchar_t* wide = api.pathname_w(entry);
    if (!wide) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

ZipApi& Api() {
    static ZipApi api;
    return api;
}

struct ZipReader {
    ZipApi& api = Api();
    archive* handle = nullptr;
    ZipReader() = default;
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;
    ~ZipReader() { if (handle) api.read_free(handle); }
    bool Open(const std::wstring& path) {
        if (!api.ready || !(handle = api.read_new())) return false;
        api.support_format_zip_seekable(handle);
        return api.open_filename_w(handle, path.c_str(), 64 * 1024) == kArchiveOk;
    }
};

bool ReadCurrent(ZipApi& api, archive* a, archive_entry* entry, size_t max_bytes, bool allow_prefix,
                 std::vector<unsigned char>& data, bool* truncated, std::wstring* error) {
    if (api.size_is_set(entry) && api.entry_size(entry) >= 0 &&
        static_cast<uint64_t>(api.entry_size(entry)) > max_bytes && !allow_prefix) {
        SetError(error, L"entry-too-large");
        return false;
    }
    unsigned char buffer[64 * 1024];
    for (;;) {
        const int64_t got = api.read_data(a, buffer, sizeof(buffer));
        if (got == 0) return true;
        if (got < 0) {
            data.clear();
            SetError(error, L"entry-read-failed");
            return false;
        }
        if (data.size() + static_cast<size_t>(got) > max_bytes) {
            if (allow_prefix) {
                data.insert(data.end(), buffer, buffer + (max_bytes - data.size()));
                if (truncated) *truncated = true;
                return true;
            }
            data.clear();
            SetError(error, L"entry-too-large");
            return false;
        }
        data.insert(data.end(), buffer, buffer + got);
    }
}

// Shared body: with allow_prefix a member larger than max_bytes yields its
// first max_bytes and sets *truncated instead of failing.
bool ReadMember(const std::wstring& path, std::string_view name, size_t max_bytes, bool allow_prefix,
                std::vector<unsigned char>& data, bool* truncated, std::wstring* error) {
    data.clear();
    if (truncated) *truncated = false;
    ZipReader reader;
    auto& api = reader.api;
    if (!reader.Open(path)) {
        SetError(error, api.ready ? L"archive-open-failed" : L"libarchive-unavailable");
        return false;
    }
    auto* a = reader.handle;
    for (;;) {
        archive_entry* entry = nullptr;
        const int r = api.next_header(a, &entry);
        if (r == kArchiveEof) break;
        if (r < kArchiveWarn || !entry) {
            SetError(error, L"entry-read-failed");
            return false;
        }
        if (!SameName(EntryName(api, entry), name)) continue;
        return ReadCurrent(api, a, entry, max_bytes, allow_prefix, data, truncated, error);
    }
    SetError(error, L"entry-not-found");
    return false;
}

} // namespace

bool VisitZipEntries(const std::wstring& path, const std::vector<std::string>& names, size_t max_bytes,
                     const ZipEntryVisitor& visit) {
    auto key = [](std::string name) {
        if (!name.empty() && name.front() == '/') name.erase(0, 1);
        for (char& c : name) c = c == '\\' ? '/' : AsciiLower(c);
        return name;
    };
    std::map<std::string, std::vector<size_t>> pending;
    for (size_t i = 0; i < names.size(); ++i) pending[key(names[i])].push_back(i);
    ZipReader reader;
    if (!reader.Open(path)) return false;
    std::vector<unsigned char> data;
    while (!pending.empty()) {
        archive_entry* entry = nullptr;
        const int r = reader.api.next_header(reader.handle, &entry);
        if (r == kArchiveEof || r < kArchiveWarn || !entry) return false;
        auto it = pending.find(key(EntryName(reader.api, entry)));
        if (it == pending.end()) continue;
        data.clear();
        if (!ReadCurrent(reader.api, reader.handle, entry, max_bytes, false, data, nullptr, nullptr))
            return false;
        for (size_t i : it->second)
            if (!visit(i, data)) return false;
        pending.erase(it);
    }
    return true;
}

bool ReadZipEntry(const std::wstring& path, std::string_view name, size_t max_bytes,
                  std::vector<unsigned char>& data, std::wstring* error) {
    return ReadMember(path, name, max_bytes, false, data, nullptr, error);
}

bool ReadZipEntryPrefix(const std::wstring& path, std::string_view name, size_t max_bytes,
                        std::vector<unsigned char>& data, bool* truncated, std::wstring* error) {
    return ReadMember(path, name, max_bytes, true, data, truncated, error);
}

} // namespace pulse::preview
