// ai_archive_tools.cpp - System32 archiveint.dll inspect/extract + folder diff.
#include "ai_archive_tools.h"

#include "ai_tools_fs.h"
#include "../common/json_value.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace pulse::app::ai {
namespace {

struct archive;
struct archive_entry;

constexpr int kArchiveEof = 1;
constexpr int kArchiveOk = 0;
constexpr int kArchiveWarn = -20;
constexpr size_t kMaxExtractFiles = 10000;
constexpr uint64_t kMaxExtractBytes = 4ull * 1024ull * 1024ull * 1024ull;

struct ReaderApi {
    HMODULE module = nullptr;
    archive* (*read_new)() = nullptr;
    int (*support_filter_all)(archive*) = nullptr;
    int (*support_format_all)(archive*) = nullptr;
    int (*open_filename_w)(archive*, const wchar_t*, size_t) = nullptr;
    int (*next_header)(archive*, archive_entry**) = nullptr;
    int64_t (*read_data)(archive*, void*, size_t) = nullptr;
    int (*data_skip)(archive*) = nullptr;
    int (*read_free)(archive*) = nullptr;
    const char* (*format_name)(archive*) = nullptr;
    const char* (*error_string)(archive*) = nullptr;
    const wchar_t* (*pathname_w)(archive_entry*) = nullptr;
    const char* (*pathname_utf8)(archive_entry*) = nullptr;
    int64_t (*entry_size)(archive_entry*) = nullptr;
    int (*size_is_set)(archive_entry*) = nullptr;
    int64_t (*entry_mtime)(archive_entry*) = nullptr;
    unsigned (*filetype)(archive_entry*) = nullptr;
    int (*is_encrypted)(archive_entry*) = nullptr;
    bool ready = false;

    ReaderApi() {
        module = LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
            return fn != nullptr;
        };
        ready = load(read_new, "archive_read_new") &&
                load(support_filter_all, "archive_read_support_filter_all") &&
                load(support_format_all, "archive_read_support_format_all") &&
                load(open_filename_w, "archive_read_open_filename_w") &&
                load(next_header, "archive_read_next_header") &&
                load(read_data, "archive_read_data") &&
                load(data_skip, "archive_read_data_skip") &&
                load(read_free, "archive_read_free") &&
                load(pathname_w, "archive_entry_pathname_w") &&
                load(pathname_utf8, "archive_entry_pathname_utf8") &&
                load(entry_size, "archive_entry_size") &&
                load(size_is_set, "archive_entry_size_is_set") &&
                load(entry_mtime, "archive_entry_mtime") &&
                load(filetype, "archive_entry_filetype") &&
                load(is_encrypted, "archive_entry_is_encrypted");
        load(format_name, "archive_format_name");
        load(error_string, "archive_error_string");
    }
};

const ReaderApi& Api() {
    static const ReaderApi api;
    return api;
}

std::wstring LowerCase(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool IsReservedDeviceName(std::wstring_view segment) {
    // Strip trailing dots/spaces and extension before checking Windows reserved names.
    size_t end = segment.size();
    while (end > 0 && (segment[end - 1] == L' ' || segment[end - 1] == L'.')) --end;
    std::wstring_view stem = segment.substr(0, end);
    if (const size_t dot = stem.find(L'.'); dot != std::wstring_view::npos) stem = stem.substr(0, dot);
    std::wstring u(stem);
    for (wchar_t& c : u) c = static_cast<wchar_t>(std::towupper(c));
    if (u == L"CON" || u == L"PRN" || u == L"AUX" || u == L"NUL") return true;
    if (u.size() == 4 && (u.rfind(L"COM", 0) == 0 || u.rfind(L"LPT", 0) == 0) && u[3] >= L'1' && u[3] <= L'9')
        return true;
    return false;
}

std::wstring EntryPath(const ReaderApi& api, archive_entry* e) {
    if (const wchar_t* w = api.pathname_w(e); w && *w) return w;
    if (const char* u = api.pathname_utf8(e); u && *u) return json::Wide(u);
    return {};
}

std::wstring NormalizeSlashPath(std::wstring p) {
    for (wchar_t& c : p)
        if (c == L'\\') c = L'/';
    while (!p.empty() && p.front() == L'/') p.erase(0, 1);
    while (!p.empty() && p.back() == L'/') p.pop_back();
    return p;
}

void EnsureDirectories(const std::wstring& dir) {
    if (dir.empty()) return;
    std::wstring clean = dir;
    while (clean.size() > 3 && clean.back() == L'\\') clean.pop_back();
    size_t pos = 0;
    if (clean.size() >= 2 && clean[1] == L':') pos = 3;
    else if (clean.rfind(L"\\\\", 0) == 0) {
        const size_t s1 = clean.find(L'\\', 2);
        const size_t s2 = s1 == std::wstring::npos ? std::wstring::npos : clean.find(L'\\', s1 + 1);
        pos = s2 == std::wstring::npos ? clean.size() : s2 + 1;
    }
    while (pos <= clean.size()) {
        const size_t next = clean.find(L'\\', pos);
        const std::wstring sub = next == std::wstring::npos ? clean : clean.substr(0, next);
        if (!sub.empty()) CreateDirectoryW(Win32Path(sub).c_str(), nullptr);
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
}

std::wstring UniqueFilePath(const std::wstring& desired) {
    if (GetFileAttributesW(Win32Path(desired).c_str()) == INVALID_FILE_ATTRIBUTES) return desired;
    const size_t slash = desired.find_last_of(L'\\');
    const size_t dot = desired.rfind(L'.');
    const bool has_ext = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
    const std::wstring stem = has_ext ? desired.substr(0, dot) : desired;
    const std::wstring ext = has_ext ? desired.substr(dot) : std::wstring();
    for (int n = 2; n < 10000; ++n) {
        std::wstring cand = stem + L" (" + std::to_wstring(n) + L")" + ext;
        if (GetFileAttributesW(Win32Path(cand).c_str()) == INVALID_FILE_ATTRIBUTES) return cand;
    }
    return desired;
}

struct DirSnapshotItem {
    std::wstring name;
    bool is_dir = false;
    uint64_t size = 0;
    uint64_t mtime_ticks = 0;
};

bool EnumerateTopLevel(const std::wstring& dir, std::vector<DirSnapshotItem>& out, std::string& err_msg,
                       const std::atomic<bool>& cancelled) {
    out.clear();
    std::wstring pattern = Win32Path(dir);
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        err_msg = "Could not list folder (Win32 " + std::to_string(e) + ").";
        return false;
    }
    do {
        if (cancelled.load()) {
            FindClose(find);
            err_msg = "Cancelled.";
            return false;
        }
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        DirSnapshotItem item;
        item.name = fd.cFileName;
        item.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        item.size = item.is_dir ? 0 : (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        item.mtime_ticks = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                           fd.ftLastWriteTime.dwLowDateTime;
        out.push_back(std::move(item));
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return true;
}

} // namespace

bool SanitizeArchiveRelativePath(std::wstring_view raw_path, std::wstring& out_rel_win_path) {
    out_rel_win_path.clear();
    if (raw_path.empty()) return false;
    // Reject absolute / UNC / drive-letter paths outright (Zip-Slip defense).
    if (raw_path.front() == L'/' || raw_path.front() == L'\\') return false;
    if (raw_path.size() >= 2 && raw_path[1] == L':') return false;

    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i < raw_path.size()) {
        while (i < raw_path.size() && (raw_path[i] == L'/' || raw_path[i] == L'\\')) ++i;
        if (i >= raw_path.size()) break;
        size_t j = i;
        while (j < raw_path.size() && raw_path[j] != L'/' && raw_path[j] != L'\\') ++j;
        std::wstring seg(raw_path.substr(i, j - i));
        i = j;
        if (seg == L".") continue;
        if (seg == L"..") return false;
        // Reject illegal Windows characters and control chars.
        for (wchar_t ch : seg) {
            if (ch < 0x20 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'|' || ch == L'?' ||
                ch == L'*') {
                return false;
            }
        }
        // Strip trailing dots/spaces Windows silently collapses; reject empty.
        while (!seg.empty() && (seg.back() == L' ' || seg.back() == L'.')) seg.pop_back();
        if (seg.empty() || IsReservedDeviceName(seg)) return false;
        parts.push_back(std::move(seg));
    }
    if (parts.empty()) return false;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) out_rel_win_path.push_back(L'\\');
        out_rel_win_path += parts[k];
    }
    return true;
}

ArchiveInspectResult InspectArchive(const std::wstring& archive_path, size_t max_entries,
                                    const std::atomic<bool>& cancelled) {
    ArchiveInspectResult out;
    const ReaderApi& api = Api();
    if (!api.ready) {
        out.error_code = "unavailable";
        out.error_message = "System archiveint.dll is not available on this Windows version.";
        return out;
    }
    archive* a = api.read_new();
    if (!a) {
        out.error_code = "init_failed";
        out.error_message = "Could not initialize archive reader.";
        return out;
    }
    struct Owner { const ReaderApi& api; archive* a; ~Owner() { api.read_free(a); } } owner{api, a};
    api.support_filter_all(a);
    api.support_format_all(a);
    const std::wstring wp = Win32Path(archive_path);
    if (api.open_filename_w(a, wp.c_str(), 64 * 1024) != kArchiveOk) {
        out.error_code = "open_failed";
        const char* es = api.error_string ? api.error_string(a) : nullptr;
        out.error_message = es && *es ? std::string(es) : "Could not open archive.";
        return out;
    }

    const size_t cap = std::clamp<size_t>(max_entries, 10, 500);
    for (;;) {
        if (cancelled.load()) {
            out.error_code = "cancelled";
            out.error_message = "Cancelled.";
            return out;
        }
        archive_entry* e = nullptr;
        const int r = api.next_header(a, &e);
        if (r == kArchiveEof) break;
        if (r < kArchiveWarn || !e) {
            if (out.total_entries == 0) {
                out.error_code = "read_failed";
                const char* es = api.error_string ? api.error_string(a) : nullptr;
                out.error_message = es && *es ? std::string(es) : "Archive appears corrupt or unsupported.";
                return out;
            }
            out.truncated = true;
            break;
        }
        if (out.format.empty() && api.format_name) {
            if (const char* fn = api.format_name(a)) out.format = fn;
        }
        const bool is_dir = (api.filetype(e) & 0170000u) == 0040000u;
        const uint64_t sz =
            api.size_is_set(e) ? static_cast<uint64_t>((std::max<int64_t>)(0, api.entry_size(e))) : 0;
        const bool enc = api.is_encrypted(e) != 0;
        if (enc) out.has_encrypted = true;
        ++out.total_entries;
        if (is_dir) ++out.dir_count;
        else {
            ++out.file_count;
            out.total_uncompressed += sz;
        }
        if (out.entries.size() < cap) {
            ArchiveEntryInfo info;
            info.path = NormalizeSlashPath(EntryPath(api, e));
            info.is_dir = is_dir;
            info.size = sz;
            info.encrypted = enc;
            info.mtime = api.entry_mtime(e);
            out.entries.push_back(std::move(info));
        } else {
            out.truncated = true;
        }
        api.data_skip(a);
    }
    out.ok = true;
    return out;
}

ArchiveExtractResult ExtractArchive(const std::wstring& archive_path, const std::wstring& dest_folder,
                                    const std::vector<std::wstring>& selected_entries,
                                    const std::atomic<bool>& cancelled) {
    ArchiveExtractResult out;
    const ReaderApi& api = Api();
    if (!api.ready) {
        out.error_code = "unavailable";
        out.error_message = "System archiveint.dll is not available on this Windows version.";
        return out;
    }
    if (dest_folder.empty()) {
        out.error_code = "invalid_dest";
        out.error_message = "Destination folder is required.";
        return out;
    }

    std::unordered_set<std::wstring> filter;
    for (const std::wstring& sel : selected_entries) {
        const std::wstring norm = LowerCase(NormalizeSlashPath(sel));
        if (!norm.empty()) filter.insert(norm);
    }

    archive* a = api.read_new();
    if (!a) {
        out.error_code = "init_failed";
        out.error_message = "Could not initialize archive reader.";
        return out;
    }
    struct Owner { const ReaderApi& api; archive* a; ~Owner() { api.read_free(a); } } owner{api, a};
    api.support_filter_all(a);
    api.support_format_all(a);
    const std::wstring wp = Win32Path(archive_path);
    if (api.open_filename_w(a, wp.c_str(), 64 * 1024) != kArchiveOk) {
        out.error_code = "open_failed";
        const char* es = api.error_string ? api.error_string(a) : nullptr;
        out.error_message = es && *es ? std::string(es) : "Could not open archive.";
        return out;
    }

    std::wstring dest = dest_folder;
    while (dest.size() > 3 && (dest.back() == L'\\' || dest.back() == L'/')) dest.pop_back();
    EnsureDirectories(dest);

    std::unordered_set<std::wstring> seen_top;
    std::vector<uint8_t> buf(64 * 1024);

    for (;;) {
        if (cancelled.load()) {
            out.error_code = "cancelled";
            out.error_message = "Extraction cancelled.";
            return out;
        }
        archive_entry* e = nullptr;
        const int r = api.next_header(a, &e);
        if (r == kArchiveEof) break;
        if (r < kArchiveWarn || !e) {
            out.error_code = "read_failed";
            const char* es = api.error_string ? api.error_string(a) : nullptr;
            out.error_message = es && *es ? std::string(es) : "Failed while reading archive entry.";
            return out;
        }
        const std::wstring raw = EntryPath(api, e);
        if (!filter.empty()) {
            const std::wstring norm = LowerCase(NormalizeSlashPath(raw));
            if (filter.count(norm) == 0) {
                api.data_skip(a);
                continue;
            }
        }
        std::wstring rel;
        if (!SanitizeArchiveRelativePath(raw, rel)) {
            ++out.skipped_unsafe;
            api.data_skip(a);
            continue;
        }
        const unsigned ftype = api.filetype(e) & 0170000u;
        // Skip symlinks/special devices for safety.
        if (ftype != 0 && ftype != 0100000u && ftype != 0040000u) {
            ++out.skipped_unsafe;
            api.data_skip(a);
            continue;
        }
        const bool is_dir = (ftype == 0040000u) || (!raw.empty() && (raw.back() == L'/' || raw.back() == L'\\'));
        const std::wstring target_base = (dest.back() == L'\\' ? dest : dest + L"\\") + rel;

        const size_t first_sep = rel.find(L'\\');
        const std::wstring top = first_sep == std::wstring::npos ? rel : rel.substr(0, first_sep);

        if (is_dir) {
            EnsureDirectories(target_base);
            ++out.extracted_dirs;
            if (seen_top.insert(LowerCase(top)).second) out.top_level_created.push_back(top);
            api.data_skip(a);
            continue;
        }

        if (out.extracted_files >= kMaxExtractFiles) {
            out.error_code = "too_many_files";
            out.error_message = "Archive exceeds the 10,000-file extraction safety limit.";
            return out;
        }

        if (const size_t parent_slash = target_base.find_last_of(L'\\'); parent_slash != std::wstring::npos) {
            EnsureDirectories(target_base.substr(0, parent_slash));
        }
        const std::wstring final_path = UniqueFilePath(target_base);
        HANDLE file = CreateFileW(Win32Path(final_path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            out.error_code = "write_failed";
            out.error_message = "Could not create extracted file on disk.";
            return out;
        }

        bool write_ok = true;
        for (;;) {
            if (cancelled.load()) {
                write_ok = false;
                out.error_code = "cancelled";
                out.error_message = "Extraction cancelled.";
                break;
            }
            const int64_t got = api.read_data(a, buf.data(), buf.size());
            if (got < 0) {
                write_ok = false;
                out.error_code = api.is_encrypted(e) ? "encrypted" : "decompress_failed";
                const char* es = api.error_string ? api.error_string(a) : nullptr;
                out.error_message = es && *es ? std::string(es) : "Failed to decompress entry.";
                break;
            }
            if (got == 0) break;
            out.extracted_bytes += static_cast<uint64_t>(got);
            if (out.extracted_bytes > kMaxExtractBytes) {
                write_ok = false;
                out.error_code = "size_limit";
                out.error_message = "Archive exceeds the 4 GiB extraction safety limit.";
                break;
            }
            DWORD written = 0;
            if (!WriteFile(file, buf.data(), static_cast<DWORD>(got), &written, nullptr) ||
                written != static_cast<DWORD>(got)) {
                write_ok = false;
                out.error_code = "disk_write_failed";
                out.error_message = "Failed writing extracted bytes to disk.";
                break;
            }
        }
        if (write_ok) {
            const int64_t mtime = api.entry_mtime(e);
            if (mtime > 0) {
                ULARGE_INTEGER t{};
                t.QuadPart = static_cast<ULONGLONG>(mtime) * 10000000ull + 116444736000000000ull;
                FILETIME ft{t.LowPart, t.HighPart};
                SetFileTime(file, nullptr, nullptr, &ft);
            }
        }
        CloseHandle(file);
        if (!write_ok) {
            DeleteFileW(Win32Path(final_path).c_str());
            return out;
        }
        ++out.extracted_files;
        // Record actual created top-level name (in case collision auto-renamed top-level file).
        std::wstring actual_top = top;
        if (first_sep == std::wstring::npos) {
            const size_t s = final_path.find_last_of(L'\\');
            if (s != std::wstring::npos && s + 1 < final_path.size()) actual_top = final_path.substr(s + 1);
        }
        if (seen_top.insert(LowerCase(actual_top)).second) out.top_level_created.push_back(actual_top);
    }

    out.ok = true;
    return out;
}

FolderCompareSummary CompareFolderPaths(const std::wstring& left_dir, const std::wstring& right_dir,
                                        const std::atomic<bool>& cancelled) {
    FolderCompareSummary out;
    out.left_path = left_dir;
    out.right_path = right_dir;

    std::vector<DirSnapshotItem> left_items, right_items;
    std::string err;
    if (!EnumerateTopLevel(left_dir, left_items, err, cancelled)) {
        out.error_code = cancelled.load() ? "cancelled" : "left_unreadable";
        out.error_message = "Left folder: " + err;
        return out;
    }
    if (!EnumerateTopLevel(right_dir, right_items, err, cancelled)) {
        out.error_code = cancelled.load() ? "cancelled" : "right_unreadable";
        out.error_message = "Right folder: " + err;
        return out;
    }

    out.left_total = left_items.size();
    out.right_total = right_items.size();

    std::unordered_map<std::wstring, size_t> right_index;
    right_index.reserve(right_items.size());
    for (size_t j = 0; j < right_items.size(); ++j) {
        right_index.emplace(LowerCase(right_items[j].name), j);
    }
    std::vector<bool> right_matched(right_items.size(), false);

    constexpr uint64_t kTolerance = 20000000ull; // 2 seconds, matching app::CompareFolderTabs
    for (const DirSnapshotItem& l : left_items) {
        const auto it = right_index.find(LowerCase(l.name));
        if (it == right_index.end()) {
            out.only_left.push_back({l.name, l.is_dir, l.size, 0});
            continue;
        }
        const size_t j = it->second;
        right_matched[j] = true;
        const DirSnapshotItem& r = right_items[j];
        if (l.is_dir != r.is_dir) {
            out.different_size.push_back({l.name, l.is_dir, l.size, r.size});
        } else if (l.is_dir) {
            ++out.same_count;
        } else if (l.mtime_ticks > r.mtime_ticks + kTolerance) {
            out.newer_left.push_back({l.name, false, l.size, r.size});
        } else if (r.mtime_ticks > l.mtime_ticks + kTolerance) {
            out.newer_right.push_back({l.name, false, l.size, r.size});
        } else if (l.size != r.size) {
            out.different_size.push_back({l.name, false, l.size, r.size});
        } else {
            ++out.same_count;
        }
    }
    for (size_t j = 0; j < right_items.size(); ++j) {
        if (!right_matched[j]) {
            const DirSnapshotItem& r = right_items[j];
            out.only_right.push_back({r.name, r.is_dir, 0, r.size});
        }
    }
    out.ok = true;
    return out;
}

} // namespace pulse::app::ai
