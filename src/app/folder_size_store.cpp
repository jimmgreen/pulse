#include "folder_size_store.h"
#include "../fs/fs_enum.h"
#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include <windows.h>
#include <cwctype>
#include <iterator>
#include <algorithm>
#include <vector>

namespace pulse::app::folder_size {
std::wstring Key(std::wstring path) {
    path = fs::NormalizePath(std::move(path));
    while (path.size() > 7 && path.back() == L'\\') path.pop_back();
    for (auto& character : path) character = static_cast<wchar_t>(std::towlower(character));
    return path;
}
bool Within(const std::wstring& path, const std::wstring& parent) {
    return path.starts_with(parent) && (path.size() == parent.size() ||
        (!parent.empty() && parent.back() == L'\\') || path[parent.size()] == L'\\');
}
uint64_t NowUtcMs() {
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    const auto ticks = (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return ticks / 10000 - 11644473600000ull;
}
Store::Entry* Store::Ensure(const std::wstring& key, const std::set<std::wstring>& protected_paths) {
    if (auto found = entries.find(key); found != entries.end()) { Touch(found->second); return &found->second; }
    if (entries.size() >= kCacheLimit) {
        auto victim = lru_.begin();
        while (victim != lru_.end() && protected_paths.contains(*victim)) ++victim;
        if (victim == lru_.end()) return nullptr;
        entries.erase(*victim); lru_.erase(victim);
    }
    lru_.push_back(key);
    auto& entry = entries[key];
    entry.position = std::prev(lru_.end());
    entry.revision = ++next_revision_;
    return &entry;
}
void Store::Touch(Entry& entry) { lru_.splice(lru_.end(), lru_, entry.position); }
void Store::MarkStale(Entry& entry, uint64_t now, uint64_t delay) {
    entry.revision = ++next_revision_; ++entry.request_epoch;
    entry.completed = 0;
    // Keep failure backoff across notifications. Explicit Calculate clears it.
    entry.not_before = (std::max)(entry.not_before, now + delay);
    entry.next_index_at = 0;
    const bool manual = entry.work.manual && entry.work.Running();
    entry.work = {};
    if (manual) { entry.work.activity = FolderSizeActivity::Queued; entry.work.manual = true; }
    entry.value.verified = false;
    if (entry.value.has_value && entry.value.source != FolderSizeSource::Index)
        entry.value.state = FolderSizeState::Cached;
}
namespace {
bool Known(const Store::Entry& entry) {
    return entry.value.has_value || entry.completed != 0 || entry.work.Running();
}
}
bool Store::InvalidateAncestors(const std::wstring& path, uint64_t now, uint64_t delay, bool known_only) {
    bool changed = false;
    std::wstring ancestor = path;
    for (;;) {
        if (auto found = entries.find(ancestor); found != entries.end() && (!known_only || Known(found->second))) {
            MarkStale(found->second, now, delay); changed = true;
        }
        if (ancestor.size() <= 7) break;
        const auto slash = ancestor.find_last_of(L'\\');
        if (slash == std::wstring::npos) break;
        ancestor.resize(slash == 6 ? 7 : slash);
    }
    return changed;
}
bool Store::Invalidate(const std::wstring& path, uint64_t now, uint64_t delay, bool known_only) {
    bool changed = InvalidateAncestors(path, now, delay, known_only);
    const auto prefix = path.back() == L'\\' ? path : path + L'\\';
    for (auto found = entries.lower_bound(prefix); found != entries.end() && found->first.starts_with(prefix); ++found) {
        if (found->first == path || (known_only && !Known(found->second))) continue;
        MarkStale(found->second, now, delay); changed = true;
    }
    return changed;
}
SavedValues Store::Snapshot() const {
    SavedValues values;
    values.reserve(entries.size());
    for (const auto& [path, entry] : entries) if (entry.value.has_value) values.emplace_back(path, entry.value);
    return values;
}
namespace {
bool Number(const std::wstring& text, uint64_t& value) {
    if (text.empty() || text.front() == L'-') return false;
    wchar_t* end = nullptr;
    value = wcstoull(text.c_str(), &end, 10);
    return end == text.c_str() + text.size();
}
}
SavedValues ReadValues(const std::wstring& file) {
    SavedValues values;
    std::wstring text;
    if (file.empty() || !ReadUtf8File(file, text)) return values;
    for (const auto& record : json::ExtractStringArray(text, L"folders")) {
        const auto first = record.find(L'\t');
        if (first == std::wstring::npos || first + 3 >= record.size() || record[first + 2] != L'\t') continue;
        FolderSizeValue value;
        if (!Number(record.substr(0, first), value.bytes)) continue;
        value.has_value = true; value.partial = record[first + 1] == L'1';
        value.state = FolderSizeState::Cached;
        size_t path_start = first + 3;
        // v2 stores provenance and scan timestamp; old records remain unknown-origin cache values.
        if (path_start + 2 < record.size() && record[path_start + 1] == L'\t' &&
            (record[path_start] == L'S' || record[path_start] == L'I' || record[path_start] == L'U')) {
            value.source = record[path_start] == L'S' ? FolderSizeSource::Scan :
                record[path_start] == L'I' ? FolderSizeSource::Index : FolderSizeSource::Unknown;
            const auto next = record.find(L'\t', path_start + 2);
            if (next == std::wstring::npos || !Number(record.substr(path_start + 2, next - path_start - 2), value.verified_at)) continue;
            path_start = next + 1;
        }
        // v3 keeps terminal omission diagnostics, never worker progress.
        if (record.compare(path_start, 3, L"!3\t") == 0) {
            const auto skip_end = record.find(L'\t', path_start + 3);
            if (skip_end == std::wstring::npos || !Number(record.substr(path_start + 3, skip_end - path_start - 3), value.skipped)) continue;
            const auto issue_end = record.find(L'\t', skip_end + 1);
            uint64_t issues = 0;
            if (issue_end == std::wstring::npos || !Number(record.substr(skip_end + 1, issue_end - skip_end - 1), issues) || issues > 15) continue;
            value.issues = static_cast<uint32_t>(issues); path_start = issue_end + 1;
        }
        const auto path = Key(record.substr(path_start));
        if (path.empty() || fs::IsVirtualPath(path)) continue;
        values.emplace_back(path, value);
        if (values.size() == kCacheLimit) break;
    }
    return values;
}
namespace {
// Like WriteUtf8FileAtomic, but for a cache that can always be rebuilt: the
// copy is hidden, so a folder showing Pulse's data folder does not count the
// short-lived .tmp (or the cache) among its items, and it skips write-through
// and the flush, which only protect against losing it in a power cut.
bool WriteCacheFile(const std::wstring& path, const std::wstring& text) {
    std::vector<uint8_t> bytes;
    if (text.size() > kMaxUtf8FileBytes || !EncodeUtf8Bytes(text, bytes) || bytes.size() > kMaxUtf8FileBytes)
        return false;
    const std::wstring temp = path + L".tmp";
    constexpr DWORD kAttributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    // An existing hidden file is replaced only when the new one is hidden too.
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        kAttributes | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        const DWORD remaining = static_cast<DWORD>((std::min)(bytes.size() - done, static_cast<size_t>(UINT32_MAX)));
        if (!WriteFile(file, bytes.data() + done, remaining, &written, nullptr) || written == 0) break;
        done += written;
    }
    CloseHandle(file);
    // The renamed copy keeps its attributes, so the cache itself is hidden too.
    if (done != bytes.size() || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}
} // namespace
bool WriteValues(const std::wstring& file, const SavedValues& values) {
    if (file.empty()) return true;
    std::wstring text = L"{\"version\":3,\"folders\":[";
    bool first = true;
    const auto cache_key = Key(file);
    for (const auto& [path, value] : values) {
        // Persisting the size of our own ancestor creates a write/size feedback loop.
        // Keep these totals live, but recompute them instead of storing them.
        if (Within(cache_key, Key(path))) continue;
        if (!value.has_value) continue;
        if (!first) text += L',';
        first = false;
        const wchar_t source = value.source == FolderSizeSource::Scan ? L'S' :
            value.source == FolderSizeSource::Index ? L'I' : L'U';
        text += L'"';
        json::Escape(std::to_wstring(value.bytes) + L"\t" + (value.partial ? L"1\t" : L"0\t") +
            source + L"\t" + std::to_wstring(value.verified_at) + L"\t!3\t" +
            std::to_wstring(value.skipped) + L"\t" + std::to_wstring(value.issues) + L"\t" + path, text);
        text += L'"';
    }
    text += L"]}";
    std::wstring existing;
    if (ReadUtf8File(file, existing) && existing == text) return true;
    return WriteCacheFile(file, text);
}
} // namespace pulse::app::folder_size