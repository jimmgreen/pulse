#pragma once
#include "app_internal.h"
#include "app_runtime.h"
#include "global_search_controller.h"
#include "picker_places_catalog.h"
#include "picker_tray_consume.h"
#include "quick_access.h"
#include "../common/picker_places_snapshot.h"

namespace pulse {
inline LRESULT HandlePickerConsumeTray(AppState& s, const COPYDATASTRUCT* data) {
    picker_places::TrayConsumeRequest request;
    if (!picker_places::DecodeConsumeTray(data, request)) return 0;
    if (request.source != s.core.host.Hwnd()) return 0;
    const auto result = app::ApplyPickerTrayConsume(s.tray, request, GetCurrentProcessId(), s.core.host.Hwnd());
    if (result.removed) {
        std::unordered_set<uint64_t> retained, consumed;
        if (const auto* shelf = s.tray.FindShelf(request.shelf_id))
            for (const auto& batch : shelf->batches)
                for (const auto& item : batch.items) retained.insert(item.id);
        for (const auto& item : request.items) if (!retained.contains(item.item_id)) consumed.insert(item.item_id);
        app::ForEachWindowSharingCore(s, [&](AppState& window) {
            auto& checked = window.trayView.checked;
            checked.erase(std::remove_if(checked.begin(), checked.end(), [&](uint64_t id) {
                return consumed.contains(id);
            }), checked.end());
            InvalidateRect(window.hwnd, nullptr, FALSE);
        });
        // Existing shelf/session timers persist the shared model and refresh the shelf.
        // Publication queues immutable metadata; this dispatch performs no file I/O.
        PublishPickerPlaces(s, true);
    }
    return picker_places::EncodeConsumeTrayResult(result);
}
inline LRESULT HandlePickerStageTray(AppState& s, const COPYDATASTRUCT* data) {
    picker_places::TrayStageRequest request;
    if (!picker_places::DecodeStageTray(data, request)) return 0;
    if (request.publisher_pid != GetCurrentProcessId() || request.source != s.core.host.Hwnd()) return 0;
    std::vector<app::TrayItem> items;
    items.reserve(request.paths.size());
    for (const auto& path : request.paths) {
        app::TrayItem item;
        item.path = path;
        item.content_kind = 0;
        item.owned_snippet = false;
        items.push_back(std::move(item));
    }
    const size_t added = s.tray.CollectRichToShelf(request.shelf_id, items, false, nullptr, true);
    if (added > 0) {
        app::ForEachWindowSharingCore(s, [](AppState& window) {
            InvalidateRect(window.hwnd, nullptr, FALSE);
        });
        PublishPickerPlaces(s, true);
    }
    return static_cast<LRESULT>(added);
}
inline bool HandlePickerManagePlaces(AppState& s, const COPYDATASTRUCT* data) {
    picker_places::ManagePlacesRequest request;
    if (!picker_places::DecodeManagePlaces(data, request) || s.places.load_failed) return false;
    using Action = picker_places::ManagePlacesAction;
    bool accepted = false;
    {
        app::PlacesCatalog::ScopedAsyncSave save(s.places);
        if (request.action <= Action::DeleteGroup) {
            accepted = app::ApplyPickerStarredManagement(s.places, request);
        } else {
            const auto& root = request.paths[0];
            const int index = s.places.FindWorkspace(root);
            switch (request.action) {
            case Action::PinWorkspace:
                if (index >= 0) accepted = true;
                else {
                    const int active = s.places.active_workspace;
                    accepted = s.places.PinWorkspace(root, picker_places::TrimGroupName(request.name), 0, {root}) >= 0;
                    s.places.active_workspace = active;
                }
                break;
            case Action::UnpinWorkspace:
                accepted = index < 0 || s.places.UnpinWorkspace(index);
                break;
            case Action::RenameWorkspace:
                if (index >= 0) {
                    s.places.workspaces[static_cast<size_t>(index)].name = picker_places::TrimGroupName(request.name);
                    accepted = s.places.Save();
                }
                break;
            case Action::MoveWorkspaceUp:
            case Action::MoveWorkspaceDown:
                if (index >= 0) {
                    const int target = index + (request.action == Action::MoveWorkspaceUp ? -1 : 1);
                    if (target >= 0 && target < static_cast<int>(s.places.workspaces.size())) MoveWorkspace(s, index, target);
                    accepted = true;
                }
                break;
            default: break;
            }
        }
    }
    if (accepted) {
        RefreshStarredViews(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        PublishPickerPlaces(s, true);
    }
    return accepted;
}
inline bool HandlePickerAddStarred(AppState& s, const COPYDATASTRUCT* data) {
    if (data && data->dwData == picker_places::kManagePlacesMessage) return HandlePickerManagePlaces(s, data);
    if (s.places.load_failed) return false;
    std::vector<picker_places::AddStarredRequest> requests;
    if (data && data->dwData == picker_places::kAddStarredBatchMessage) {
        if (!picker_places::DecodeAddStarredBatch(data, requests)) return false;
    } else {
        picker_places::AddStarredRequest request;
        if (!picker_places::DecodeAddStarred(data, request)) return false;
        requests.push_back(std::move(request));
    }
    for (const auto& request : requests) {
        if (!request.group.empty() && !s.places.FindStarredGroup(request.group)) return false;
        const auto* existing = s.places.FindStarred(request.path);
        if (existing && !request.group.empty() && existing->kind == app::PlaceItemKind::File) return false;
    }
    {
        app::PlacesCatalog::ScopedAsyncSave save(s.places);
        for (const auto& request : requests) {
            const auto* existing = s.places.FindStarred(request.path);
            // Retries add idempotently; never toggle an existing favorite off.
            if (!existing && !ToggleStarred(s, request.path, static_cast<app::PlaceItemKind>(request.kind))) return false;
            if (!request.group.empty() && s.places.SetStarredItemGroup(request.path, request.group)) {
                RefreshStarredViews(s);
                InvalidateRect(s.hwnd, nullptr, FALSE);
            }
        }
    }
    PublishPickerPlaces(s, true);
    return true;
}
}
