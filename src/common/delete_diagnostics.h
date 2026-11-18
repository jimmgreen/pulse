#pragma once
#include "path_utils.h"
#include "preview_diagnostics.h"
#include "runtime_log.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pulse::diagnostics::deletion {
inline constexpr size_t kItemLimit = 64, kHistoryLimit = 128;
inline constexpr uint64_t kLifetimeMs = 120000;
inline std::wstring Key(std::wstring_view path) {
    auto key = path::FoldedKey(path::StripExtendedPathPrefix(path));
    std::replace(key.begin(), key.end(), L'/', L'\\');
    while (key.size() > 3 && key.back() == L'\\') key.pop_back();
    return key;
}
inline uint64_t Id(std::wstring_view key) { return preview::DiagnosticPathId(key); }
inline std::wstring Parent(const std::wstring& key) {
    const auto slash = key.find_last_of(L'\\');
    if (slash == std::wstring::npos) return {};
    return key.substr(0, slash == 2 && key[1] == L':' ? 3 : slash);
}
struct Record {
    uint64_t task = 0, item = 0, at = 0, path_id = 0;
    std::wstring parent, name;
};
class History {
public:
    void Add(uint64_t task, uint64_t item, const std::wstring& source, uint64_t now) {
        const auto key = Key(source);
        const auto parent = Parent(key);
        if (parent.empty()) return;
        std::lock_guard lock(mutex_);
        Prune(now);
        if (records_.size() == kHistoryLimit) records_.erase(records_.begin());
        records_.push_back({task, item, now, Id(key), parent, key.substr(key.find_last_of(L'\\') + 1)});
    }
    std::vector<Record> ForDirectory(std::wstring_view directory, uint64_t now) {
        const auto key = Key(directory);
        std::lock_guard lock(mutex_);
        Prune(now);
        std::vector<Record> found;
        for (const auto& record : records_) if (record.parent == key) found.push_back(record);
        return found;
    }
private:
    void Prune(uint64_t now) {
        std::erase_if(records_, [now](const Record& record) { return now - record.at > kLifetimeMs; });
    }
    std::mutex mutex_;
    std::vector<Record> records_;
};
inline History history;
inline unsigned SourceState(DWORD attributes, DWORD error) {
    if (attributes != INVALID_FILE_ATTRIBUTES) return 1;
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? 0u : 2u;
}

// Called on the operation worker only. Attribute errors other than missing
// are unknown, never evidence that the source was removed. No retries or I/O
// are performed by the UI-side snapshot observer below.
inline void Item(uint64_t task, size_t index, const std::wstring& source,
                 HRESULT hr, bool confirmed, bool cancelled, bool mutated) {
    if (!runtime::Enabled() || index >= kItemLimit) return;
    const auto key = Key(source);
    const auto parent = Parent(key);
    const auto started = GetTickCount64();
    const DWORD attrs = GetFileAttributesW(source.c_str());
    const DWORD error = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const unsigned state = SourceState(attrs, error);
    runtime::Event("file_delete_item", {{"task", task}, {"item", index}, {"path_id", Id(key)},
        {"directory_id", Id(parent)}, {"hresult", static_cast<uint32_t>(hr)}, {"confirmed", confirmed},
        {"cancelled", cancelled}, {"mutated", mutated}, {"source_state", state}, {"probe_error", error},
        {"attributes", attrs}, {"probe_ms", GetTickCount64() - started}});
    if (confirmed) history.Add(task, index, source, GetTickCount64());
}

// stage: 0 = worker enumeration (possibly later superseded), 1 = accepted UI
// snapshot. Match names only within the same normalized parent directory.
template<class Entries>
void Snapshot(std::wstring_view directory, const Entries& entries, uint64_t generation,
              unsigned stage, uint64_t window = 0) {
    if (!runtime::Enabled()) return;
    const auto now = GetTickCount64();
    const auto records = history.ForDirectory(directory, now);
    if (records.empty()) return;
    std::unordered_map<std::wstring, bool> names;
    for (const auto& record : records) names.emplace(record.name, false);
    for (const auto& entry : entries) {
        const auto found = names.find(path::FoldedKey(entry.name));
        if (found != names.end()) found->second = true;
    }
    for (const auto& record : records) {
        runtime::Event("file_delete_snapshot", {{"task", record.task}, {"item", record.item},
            {"path_id", record.path_id}, {"directory_id", Id(record.parent)}, {"generation", generation},
            {"stage", stage}, {"window", window}, {"present", names.at(record.name)},
            {"entries", entries.size()}, {"since_delete_ms", now - record.at}});
    }
}
} // namespace pulse::diagnostics::deletion
