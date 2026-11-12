// dialog_record_apps.h — the program behind a dialog takeover record, for the
// settings list: records keep only the executable's name and a fingerprint of
// its full path (application_identity::ImageHash), never the path itself.
#pragma once
#include "../common/menu_icon.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse::app {

struct DialogRecordApp {
    std::wstring path;          // empty: not found on this machine right now
    std::wstring name;          // friendly name; empty when no path
    common::MenuIconRef icon;   // icon_edge pixels square; null when no path or icon
    bool verified = false;      // path hashes to the record's fingerprint
};

struct DialogRecordAppQuery {
    std::wstring basename;      // "WINWORD.EXE"
    uint64_t image_id = 0;      // ImageHash of the full image path
};

// Reads processes, the registry and files: worker threads only. A running
// process whose path hashes to the fingerprint wins; otherwise App Paths gives
// the program registered under that executable name (same name, unverified).
std::unordered_map<uint64_t, DialogRecordApp> ResolveDialogRecordApps(
    const std::vector<DialogRecordAppQuery>& queries, uint32_t icon_edge);

// Icon edge for the settings rows: 32 DIP at the system DPI (64 at most).
uint32_t DialogRecordIconEdge();

} // namespace pulse::app
