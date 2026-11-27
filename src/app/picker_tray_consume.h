#pragma once
#include "app_model.h"
#include "../common/picker_tray_consume.h"

namespace pulse::app {
// Owner thread only. Preflight the entire identity set before changing any record.
// Snippet backing files must stay referenced until a separate lifetime policy exists.
inline picker_places::TrayConsumeResult ApplyPickerTrayConsume(StagingTray& tray,
    const picker_places::TrayConsumeRequest& request, DWORD publisher_pid, HWND source) {
    if (!picker_places::ValidConsumeTray(request) || request.publisher_pid != publisher_pid || request.source != source) return {};
    DWORD source_pid = 0;
    if (!GetWindowThreadProcessId(source, &source_pid) || source_pid != publisher_pid) return {};
    const auto* shelf = tray.FindShelf(request.shelf_id);
    if (!shelf) return {};
    std::vector<picker_places::TrayConsumeItem> eligible;
    uint32_t skipped = 0;
    for (const auto& selected : request.items) {
        const TrayItem* found = nullptr;
        for (const auto& batch : shelf->batches) if (batch.id == selected.batch_id)
            for (const auto& item : batch.items) if (item.id == selected.item_id) found = &item;
        if (!found || found->path != selected.path || found->inflight_task) return {};
        if (IsTraySnippet(*found)) ++skipped;
        else eligible.push_back(selected);
    }
    for (const auto& item : eligible)
        tray.RemoveReferenceByIdentity(request.shelf_id, item.batch_id, item.item_id, item.path);
    return {true, static_cast<uint32_t>(eligible.size()), skipped};
}
}
