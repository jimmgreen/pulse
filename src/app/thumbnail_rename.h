#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include <unordered_map>

namespace pulse::app {
// Snapshot-only: never probe a file or Shell provider on the UI thread.
template<class Transfer>
void TransferRenamedThumbnails(const std::wstring& folder, const std::vector<fs::DirEntry>& before,
    const std::vector<fs::DirEntry>& after, const std::vector<fs::DirNotifyEvent>& events, Transfer transfer) {
    std::unordered_map<std::wstring, const fs::DirEntry*> old_rows, new_rows;
    for (const auto& event : events) {
        if (event.action != FILE_ACTION_RENAMED_NEW_NAME || event.old_name.empty()) continue;
        old_rows.emplace(event.old_name, nullptr); new_rows.emplace(event.name, nullptr);
    }
    if (old_rows.empty()) return;
    for (const auto& entry : before)
        if (auto it = old_rows.find(entry.name); it != old_rows.end()) it->second = &entry;
    for (const auto& entry : after)
        if (auto it = new_rows.find(entry.name); it != new_rows.end()) it->second = &entry;
    const auto path = [&](const fs::DirEntry& entry) {
        if (!entry.full_path.empty()) return entry.full_path;
        return folder + (folder.empty() || folder.back() == L'\\' || folder.back() == L'/' ? L"" : L"\\") + entry.name;
    };
    for (size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];
        if (event.action != FILE_ACTION_RENAMED_NEW_NAME || event.old_name.empty()) continue;
        // A coalesced batch may contain A -> B -> C, with only C in the snapshot.
        auto final_name = event.name;
        for (size_t j = i + 1; j < events.size(); ++j)
            if (events[j].action == FILE_ACTION_RENAMED_NEW_NAME && events[j].old_name == final_name)
                final_name = events[j].name;
        const auto* a = old_rows.at(event.old_name); const auto* b = new_rows.at(final_name);
        if (!a || !b || a->is_dir || b->is_dir || a->is_reparse || b->is_reparse ||
            a->attrs != b->attrs || a->size != b->size ||
            CompareFileTime(&a->mtime, &b->mtime) != 0 || CompareFileTime(&a->ctime, &b->ctime) != 0) continue;
        const uint64_t modified = (uint64_t(b->mtime.dwHighDateTime) << 32) | b->mtime.dwLowDateTime;
        transfer(path(*a), path(*b), modified, b->size);
    }
}
}
