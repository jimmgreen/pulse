// host_routing.h - Which messages the hidden host window hands to a Pulse
// window (multi-window M2; plan §2.3).
//
// Services and background threads post process-level notifications to the
// host (ServiceWindow). Until the data/window split of M3, the host forwards
// each one unchanged to the main window with SendMessage, so the existing
// handlers run as before. Window-level messages (frame pump, quick preview,
// details probe, menus, toasts, the search box's own history refresh, the
// IShellWindows registration, the duplicate scan's volume list) still go to
// their window directly. A tab's live network search reports here too since
// M6f: the tab can move to another window, and its window can close (merge). Results of shared
// navigation services (M3e: UNC probe scheduler, BitLocker unlock wait,
// Explorer takeover) come here so they survive the window that started them;
// their handlers update every window. System
// broadcasts are not forwarded: every top-level window already receives them.
// The host handles TaskbarCreated itself because it owns the notification
// icon, and since M3c the broadcasts in ClassifySystemBroadcast once for all
// windows; Pulse windows only handle those when there is no host.
#pragma once
#include <windows.h>
#include <dbt.h>
#include <cwchar>
#include "app_messages.h"
#include "tray_controller.h"

namespace pulse::app {

inline constexpr UINT kHostRoutedMessages[] = {
    WM_WORKER_RESULT,          // PostWorkerResult
    WM_OPS_NOTIFY,             // OpsManager completion
    WM_INDEX_NOTIFY,           // IndexClient status
    WM_RECYCLE_SEARCH,         // current-user recycle search results
    WM_INDEX_SEARCH,           // IndexClient search results
    WM_NETWORK_INDEX_NOTIFY,   // NetworkAgentClient status
    WM_NETWORK_INDEX_SEARCH,   // NetworkAgentClient search results
    WM_CONTENT_SEARCH,         // content search agent
    WM_DUPLICATE_SCAN,         // duplicate scan agent
    WM_ASSOC_CHANGED,          // SHCNE_ASSOCCHANGED registration
    WM_SHELL_CACHE_INVALIDATE, // shell registry watch
    WM_SHELL_VERB_SEED,        // machine verb cache seed
    WM_CTX_LAYOUT_SEED,        // persistent context menu layout read (C4)
    WM_SHELL_VERBS,            // static verb reads (process-wide cache)
    WM_RECYCLE_INFO,           // Recycle Bin occupancy query
    WM_RECYCLE_SHELL_CHANGED,  // external Recycle Bin mutation
    WM_NETWORK_LOCATIONS,      // network locations read
    WM_LAN_DISCOVERY,          // LAN device discovery (network page)
    WM_UPDATE_RESULT,          // update check
    WM_UPDATE_DOWNLOADED,      // update download
    WM_CHANGE_TRACKING,        // change tracking client
    WM_TAG_ADS_WARNING,        // tag ADS write failures
    WM_TAG_ADS_DISCOVERED,     // tag ADS discovery
    WM_SETTINGS_TASK_RESULT,   // settings background tasks
    WM_NET_PROBE,              // UNC probe scheduler (shared queue)
    WM_DRIVE_EJECT_READY,
    WM_DRIVE_VERB,
    WM_BITLOCKER_UNLOCK,       // unlock wait (the prompt stays owned by the waiting window)
    WM_NETWORK_CONNECT,        // share logon (the credential prompt stays owned by the waiting window)
    WM_EXPLORER_TAKEOVER,      // Explorer window takeover (opens in the most recent window)
    WM_SHELLCTX_ITEMS,         // shell context menu rows (M4a: the menu is open in the active window)
    WM_NETWORK_LIVE_SEARCH,    // a tab's live network walk (M6f: the tab may have moved to another window)
    WM_DEVICES_CHANGED,        // phone list (DeviceRegistry, device thread)
    WM_UI_KICK,                // folder changes / folder sizes ready (shared AppCore queues)
    TrayController::kCallbackMessage,  // notification icon (owned by the host)
    WM_HOTKEY,                 // global search hotkey (registered on the host)
    WM_DOUBLE_CTRL,            // global search double Ctrl (posted to the host)
    WM_DIALOG_JUMP_CONTEXT,    // Open / Save dialog jump bar context request
};

constexpr bool IsHostRoutedMessage(UINT message) noexcept {
    for (const UINT routed : kHostRoutedMessages)
        if (routed == message) return true;
    return false;
}

// System broadcasts with a Pulse handler (M3c). The shared part (theme flag,
// drive list, places) runs once; the per-window part (accent, DWM chrome,
// repaint) runs for every window.
enum class SystemBroadcast { None, Theme, Volumes, Devices, QueryEndSession, EndSession };

// GUID_DEVINTERFACE_WPD: phones and cameras (MTP / PTP). DeviceRegistry
// registers for these; volume broadcasts reach every window anyway.
inline constexpr GUID kPortableDeviceInterfaceClass = {
    0x6AC27878, 0xA6FA, 0x4155, {0xBA, 0x85, 0xF9, 0x8F, 0x49, 0x1D, 0x4F, 0x33}};

inline SystemBroadcast ClassifySystemBroadcast(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_THEMECHANGED: return SystemBroadcast::Theme;
    case WM_SETTINGCHANGE: {
        const auto* area = reinterpret_cast<const wchar_t*>(lparam);
        if (wparam == SPI_SETHIGHCONTRAST || !area || wcscmp(area, L"ImmersiveColorSet") == 0 || wcscmp(area, L"HighContrast") == 0)
            return SystemBroadcast::Theme;
        return SystemBroadcast::None;
    }
    case WM_DEVICECHANGE: {
        // A USB stick, a mounted image, or any new volume changes the drive list.
        if (wparam != DBT_DEVICEARRIVAL && wparam != DBT_DEVICEREMOVECOMPLETE) return SystemBroadcast::None;
        const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(lparam);
        if (!header || header->dbch_devicetype == DBT_DEVTYP_VOLUME) return SystemBroadcast::Volumes;
        // A phone came or went (its storages are not volumes).
        if (header->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
            header->dbch_size >= sizeof(DEV_BROADCAST_DEVICEINTERFACE_W) &&
            IsEqualGUID(reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(header)->dbcc_classguid,
                        kPortableDeviceInterfaceClass))
            return SystemBroadcast::Devices;
        return SystemBroadcast::None;
    }
    case WM_QUERYENDSESSION: return SystemBroadcast::QueryEndSession;
    case WM_ENDSESSION: return SystemBroadcast::EndSession;
    default: return SystemBroadcast::None;
    }
}

}  // namespace pulse::app
