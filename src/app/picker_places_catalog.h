#pragma once
#include "places.h"
#include "../common/picker_places_snapshot.h"

namespace pulse::app {
// Call on the catalog's owner thread inside ScopedAsyncSave.
inline bool ApplyPickerStarredManagement(PlacesCatalog& catalog, const picker_places::ManagePlacesRequest& request) {
    using Action = picker_places::ManagePlacesAction;
    if (!picker_places::ValidManagePlaces(request) || catalog.load_failed) return false;
    if (request.action == Action::Remove) {
        for (const auto& path : request.paths)
            if (catalog.IsStarred(path)) catalog.ToggleStarred(path);
        return true;
    }
    if (request.action == Action::SetGroup) {
        if (!request.group.empty() && !catalog.FindStarredGroup(request.group)) return false;
        for (const auto& path : request.paths) {
            const auto* item = catalog.FindStarred(path);
            if (!item || item->kind != PlaceItemKind::Folder) return false;
        }
        for (const auto& path : request.paths) catalog.SetStarredItemGroup(path, request.group);
        return true;
    }
    if (request.action == Action::CreateGroup) {
        const auto* item = catalog.FindStarred(request.paths[0]);
        if (!item || item->kind != PlaceItemKind::Folder) return false;
        const auto name = picker_places::TrimGroupName(request.name);
        const auto* current = catalog.FindStarredGroup(item->group);
        // A timeout retry for this folder/name must not manufacture another group.
        if (current && current->name == name) return true;
        return !catalog.CreateStarredGroup(name, request.paths[0]).empty();
    }
    if (request.action == Action::RenameGroup) {
        const auto* group = catalog.FindStarredGroup(request.group);
        if (!group) return false;
        const auto name = picker_places::TrimGroupName(request.name);
        return group->name == name || catalog.RenameStarredGroup(request.group, name);
    }
    if (request.action == Action::DeleteGroup)
        return !catalog.FindStarredGroup(request.group) || catalog.UngroupStarred(request.group);
    return false;
}
}
