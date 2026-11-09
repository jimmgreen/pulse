// selection_caps.h — what the selection in a tab can do, by what it is.
//
// The toolbar file commands (cut / copy / rename / delete), the selection chip,
// the item menu and the keyboard shortcuts all read this, so a drive, a share or
// a Recycle Bin item never gets the file commands that make no sense for it
// (copying C:\ to the clipboard, deleting \\server\share, ...).
// Header-only: app_model.cpp (compiled into several tests) uses it.
#pragma once
#include "app_model.h"
#include "places.h"
#include "app_network_locations.h"
#include "../fs/fs_enum.h"
#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::app {

enum class SelectionKind : uint8_t {
    None,           // nothing selected
    Files,          // ordinary files and folders
    Drives,         // This PC drive rows
    Launchers,      // This PC program icons (网盘程序入口): open only
    NetworkShares,  // the share list of \\server
    NetworkPlaces,  // pulse:networks (pinned and system network locations)
    Recycle,        // Recycle Bin items
    Device,         // a phone's files and storages: Explorer's verbs via the shell host
};

struct SelectionCaps {
    SelectionKind kind = SelectionKind::None;
    bool cut = false;
    bool copy = false;
    bool del = false;     // networks view: removes the selected pins
    bool rename = false;  // one item; a drive's label
    bool tags = false;    // only real file / folder selections
    // Drives: what the selected drives allow (any of them).
    bool any_removable = false;  // DRIVE_REMOVABLE / DRIVE_CDROM (fixed USB disks: see DriveEjectable)
    bool any_remote = false;     // mapped network drive
    bool any_locked = false;     // BitLocker volume not unlocked yet
    bool all_locked = false;
    // Networks view: some selected rows are Pulse's own pins.
    bool any_network_pin = false;
};

// "\\server" (no share): its listing is the server's shares.
inline bool IsUncServerRoot(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) path.remove_prefix(8);
    else if (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\")) path.remove_prefix(2);
    else return false;
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    return !path.empty() && path.find_first_of(L"\\/") == std::wstring_view::npos;
}

// "\\server\share" exactly (a share, not a folder inside it).
inline bool IsUncShareRoot(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) path.remove_prefix(8);
    else if (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\")) path.remove_prefix(2);
    else return false;
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    const size_t slash = path.find_first_of(L"\\/");
    return slash != std::wstring_view::npos && slash > 0 && slash + 1 < path.size() &&
        path.find_first_of(L"\\/", slash + 1) == std::wstring_view::npos;
}

enum class RecycleSelection { None, All, Mixed };

inline RecycleSelection RecycleSelectionOf(const Tab& tab) {
    bool recycle = false;
    bool ordinary = false;
    for (int index : tab.SelectedIndices()) {
        if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) continue;
        if (tab.EntryAt(static_cast<size_t>(index)).recycle_path.empty()) ordinary = true;
        else recycle = true;
    }
    return recycle ? (ordinary ? RecycleSelection::Mixed : RecycleSelection::All)
                   : RecycleSelection::None;
}

inline SelectionCaps ComputeSelectionCaps(const Tab& tab, const PlacesCatalog* places) {
    SelectionCaps caps;
    const int count = tab.SelectedCount();
    if (count <= 0) return caps;
    const auto recycle = RecycleSelectionOf(tab);
    if (recycle != RecycleSelection::None) {
        caps.kind = SelectionKind::Recycle;
        caps.del = recycle == RecycleSelection::All;
        return caps;
    }
    std::wstring kind;
    std::wstring rest;
    if (ParsePulsePath(tab.current_path, &kind, &rest) && kind == L"device") {
        // Phone files: what Explorer offers for them, run by the shell host
        // on their shell ids. A device root lists its storages: copy only.
        caps.kind = SelectionKind::Device;
        caps.copy = true;
        if (rest.find(L'\\') == std::wstring::npos) return caps;
        caps.cut = true;
        bool read_only = false;
        for (int index : tab.SelectedIndices()) {
            if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) continue;
            if (tab.EntryAt(static_cast<size_t>(index)).attrs & FILE_ATTRIBUTE_READONLY) {
                read_only = true;
                break;
            }
        }
        caps.cut = !read_only;  // a cut deletes the source afterwards
        caps.del = !read_only;
        caps.rename = count == 1 && !read_only;
        return caps;
    }
    if (kind == L"recycle") {
        caps.kind = SelectionKind::Recycle;
        caps.del = true;  // permanent delete, confirmed
        return caps;
    }
    if (tab.current_path == kNetworkLocationsPath) {
        caps.kind = SelectionKind::NetworkPlaces;
        if (places) {
            for (int index : tab.SelectedIndices()) {
                if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) continue;
                if (places->FindNetwork(tab.EntryAt(static_cast<size_t>(index)).full_path) >= 0) {
                    caps.any_network_pin = true;
                    break;
                }
            }
        }
        caps.del = caps.any_network_pin;  // Del unpins, like removing a shortcut
        return caps;
    }
    if (IsUncServerRoot(tab.current_path)) {
        caps.kind = SelectionKind::NetworkShares;
        return caps;
    }
    caps.kind = SelectionKind::Files;
    // Only This PC (empty path) lists drives; ordinary folders skip the scan so
    // a huge selection costs nothing per frame.
    if (tab.current_path.empty() && tab.snapshot) {
        bool drives = false;
        bool launchers = false;
        bool all_locked = true;
        bool rename = count == 1;
        for (int index : tab.SelectedIndices()) {
            if (index < 0 || static_cast<size_t>(index) >= tab.EntryCount()) continue;
            const fs::DirEntry e = tab.EntryAt(static_cast<size_t>(index));
            if (e.pc_launcher || e.pc_device) {  // programs and phones: open only
                launchers = true;
                continue;
            }
            if (!e.drive_type) continue;
            drives = true;
            caps.any_removable = caps.any_removable || e.drive_type == DRIVE_REMOVABLE ||
                e.drive_type == DRIVE_CDROM;
            caps.any_remote = caps.any_remote || e.drive_type == DRIVE_REMOTE;
            caps.any_locked = caps.any_locked || e.drive_locked;
            all_locked = all_locked && e.drive_locked;
            // A disc has no writable label; a locked volume cannot be read.
            if (e.drive_locked || e.drive_type == DRIVE_CDROM) rename = false;
        }
        if (launchers) {
            caps.kind = SelectionKind::Launchers;  // no file commands at all
            return caps;
        }
        if (drives) {
            caps.kind = SelectionKind::Drives;
            caps.all_locked = all_locked;
            caps.rename = rename;
            return caps;
        }
    }
    caps.cut = caps.copy = caps.del = true;
    caps.rename = count == 1;
    caps.tags = true;
    return caps;
}

} // namespace pulse::app
