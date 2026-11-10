#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../ui/ui_view_model.h"
#include <vector>
#include <optional>
#include <unordered_map>

namespace pulse::app {

enum class NotifyPatch { Applied, NeedFullEnum };

struct PreparedDirNotifyBatch {
    std::vector<fs::DirNotifyEvent> events;
    std::unordered_map<std::wstring, std::optional<fs::DirEntry>> metadata;
    bool need_full_enum = false;
};

// Performs drive classification and filesystem probes; call from an I/O worker.
PreparedDirNotifyBatch PrepareDirNotifyBatch(const std::wstring& folder,
    const std::vector<fs::DirNotifyEvent>& events);
// Pure memory application; maintains rename holds and active sort order and never probes disk.
NotifyPatch ApplyPreparedDirNotifyBatch(std::vector<fs::DirEntry>& entries,
    const PreparedDirNotifyBatch& prepared, ui::SortColumn col, ui::SortDirection sort_dir);

bool FillDirEntry(const std::wstring& dir, const std::wstring& name, fs::DirEntry& out);

// Renames keep their row within a group. New rows and modified sort keys
// move to their comparator position (see entry_order_hold.h).
NotifyPatch ApplyDirNotify(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                           const fs::DirNotifyEvent& event,
                           ui::SortColumn col, ui::SortDirection sort_dir);

// Same result as calling ApplyDirNotify for each event in order, in
// O(entries + events log events) instead of O(entries * events).
NotifyPatch ApplyDirNotifyBatch(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                                const std::vector<fs::DirNotifyEvent>& events,
                                ui::SortColumn col, ui::SortDirection sort_dir);

} // namespace pulse::app
