// app_messages.h - Pulse's private window messages (WM_APP range).
// Light on purpose, so routing tables and tests can use the ids without the
// whole AppState.
#pragma once
#include <windows.h>

namespace pulse {

constexpr UINT WM_WORKER_RESULT = WM_APP + 1;
constexpr UINT WM_OPS_NOTIFY = WM_APP + 2;
constexpr UINT WM_INDEX_NOTIFY = WM_APP + 40;
constexpr UINT WM_INDEX_SEARCH = WM_APP + 42;
constexpr UINT WM_NET_PROBE = WM_APP + 41;
constexpr UINT WM_TAG_ADS_WARNING = WM_APP + 43;
constexpr UINT WM_TAG_ADS_DISCOVERED = WM_APP + 44;
// Explorer context-menu integration (优化.md §7). lParam owns a heap payload.
constexpr UINT WM_SHELLCTX_ITEMS = WM_APP + 45;  // ShellCtxItemsPayload*
constexpr UINT WM_SHELL_VERBS = WM_APP + 46;     // ShellVerbsResult*
constexpr UINT WM_DETAILS_META = WM_APP + 47;    // DetailsMetaResult*
constexpr UINT WM_SETTINGS_TASK_RESULT = WM_APP + 48;
constexpr UINT WM_SHELL_CACHE_INVALIDATE = WM_APP + 49;
constexpr UINT WM_NETWORK_INDEX_NOTIFY = WM_APP + 51;
constexpr UINT WM_NETWORK_INDEX_SEARCH = WM_APP + 52;
constexpr UINT WM_CONTENT_SEARCH = WM_APP + 53;
constexpr UINT WM_CONTENT_SELECTION = WM_APP + 64;
constexpr UINT WM_DUPLICATE_SCAN = WM_APP + 58;
constexpr UINT WM_QUICK_PREVIEW_NAVIGATE = WM_APP + 54;
constexpr UINT WM_QUICK_PREVIEW_OPEN = WM_APP + 55;
constexpr UINT WM_QUICK_PREVIEW_COMMAND = WM_APP + 65;  // lParam: immutable command token
constexpr UINT WM_SHOW_RELEASE_NOTES = WM_APP + 66;  // "updated" toast clicked
constexpr UINT WM_UPDATE_RESULT = WM_APP + 56;
constexpr UINT WM_RECYCLE_INFO = WM_APP + 57;
constexpr UINT WM_DUP_VOLUMES = WM_APP + 59;
constexpr UINT WM_UPDATE_DOWNLOADED = WM_APP + 60;
constexpr UINT WM_UPDATE_INSTALL = WM_APP + 61;
constexpr UINT WM_SEARCH_HISTORY = WM_APP + 62;
constexpr UINT WM_CHANGE_TRACKING = WM_APP + 63;
constexpr UINT WM_FRAME_PUMP = WM_APP + 67;  // app::FramePump: one display frame of motion
constexpr UINT WM_SHELL_SELECT = WM_APP + 68;  // app::ShellSelectRequest* (shell_window_sync.h)
constexpr UINT WM_EXPLORER_TAKEOVER = WM_APP + 69;  // app::ExplorerTakeoverRequest* (shell_window_sync.h)
constexpr UINT WM_NETWORK_LIVE_SEARCH = WM_APP + 71;  // shared_ptr<LiveNetworkSearch>* (#74 live walk progressed)
constexpr UINT WM_SHELL_VERB_SEED = WM_APP + 70;
constexpr UINT WM_EXIT_PULSE = WM_APP + 72;  // palette "Exit Pulse" (#57)  // ShellVerbSeed* (machine verb cache read off the UI thread)
constexpr UINT WM_NETWORK_LOCATIONS = WM_APP + 73;
constexpr UINT WM_ASSOC_CHANGED = WM_APP + 74;  // SHCNE_ASSOCCHANGED: default programs changed
constexpr UINT WM_BITLOCKER_UNLOCK = WM_APP + 75;  // app::BitLockerUnlockResult* (bitlocker_unlock.h)
constexpr UINT WM_RELEASE_WINDOW_STATES = WM_APP + 76;  // host: free retired window states (window_factory.h)
constexpr UINT WM_ARRANGE_WINDOWS = WM_APP + 129;
constexpr UINT WM_MERGE_ALL_WINDOWS = WM_APP + 130;
constexpr UINT WM_NEW_WINDOW = WM_APP + 77;  // open another window at this window's folder (M4a)
constexpr UINT WM_REOPEN_CLOSED_WINDOW = WM_APP + 78;  // reopen the last closed window (M4c toast, Ctrl+Shift+T)
constexpr UINT WM_OPEN_SESSION_WINDOWS = WM_APP + 79;  // first window shown: open the session's other windows (M4d)
constexpr UINT WM_MOVE_TAB_TO_NEW_WINDOW = WM_APP + 80;  // Ctrl+Shift+M: the active tab moves to a new window (M4e)
constexpr UINT WM_BEGIN_TAB_DETACH = WM_APP + 81;  // a tab dragged off the strip: start the system move loop (M6a)
constexpr UINT WM_UNDO_WINDOW_MERGE = WM_APP + 82;  // the merge toast was clicked: open the merged window again (M6c)
constexpr UINT WM_MERGE_WINDOW = WM_APP + 83;  // released on a tab strip after a title-bar drag; wParam target hwnd, lParam index (M6c)
constexpr UINT WM_CONVERT_EXTERNAL_OPEN = WM_APP + 84;  // the open toast was clicked: tab <-> new window (M7b)
constexpr UINT WM_ADOPT_OPEN_POLICY = WM_APP + 85;  // the "make it the default" toast was clicked (M7b)
constexpr UINT WM_REOPEN_CLOSED_TABS = WM_APP + 86;  // the "N tabs closed" toast was clicked (M7f)
constexpr UINT WM_NETWORK_CONNECT = WM_APP + 88;  // app::NetworkConnectResult* (network_connect.h)
constexpr UINT WM_CTX_LAYOUT_SEED = WM_APP + 87;  // lParam CtxLayoutSeed*: persistent menu layout read (C4)
constexpr UINT WM_OPEN_EXTERNAL = WM_APP + 89;  // host: wake only; SingleInstanceCoordinator owns queued requests
constexpr UINT WM_DRIVE_EJECT_READY = WM_APP + 133; // shared_ptr<DriveEjectTask>*
constexpr UINT WM_DRIVE_VERB = WM_APP + 90;  // app::DriveVerbResult* (drive_verbs.h): eject / disconnect finished
constexpr UINT WM_DETAILS_TREE_UNDO = WM_APP + 91;  // the "expanded N folders" snack: undo it
constexpr UINT WM_UNDO_DELETE = WM_APP + 92;  // the "deleted N items" snack: undo the delete
constexpr UINT WM_ZIP_COMPRESS = WM_APP + 93;  // 压缩: wParam 0 still running (lParam job id), 1 done (ZipDone*)
constexpr UINT WM_ZIP_CANCEL = WM_APP + 94;  // the 压缩 snack: cancel
constexpr UINT WM_DEFERRED_STARTUP = WM_APP + 95;  // posted by the first WM_CREATE: build what the first frame does not need
constexpr UINT WM_REGISTER_DROP_TARGET = WM_APP + 96;  // posted by every WM_CREATE: RegisterDragDrop once the first frame is up
constexpr UINT WM_FAVORITE_UNDO = WM_APP + 97;  // the 已移出收藏夹 snack (收藏栏 drag-out): put it back
constexpr UINT WM_TAG_DROP_UNDO = WM_APP + 98;  // the 已给 N 项加上「…」 snack (files dropped on a tag): undo
constexpr UINT WM_TAG_RULE_HIT = WM_APP + 99;  // 自动规则: files appeared in a watched folder (tag_rules.cpp)
constexpr UINT WM_TRAY_UNDO = WM_APP + 100;      // the tray release snack: undo the copy / move
constexpr UINT WM_TRAY_OPEN_DEST = WM_APP + 101;  // the tray release snack: open the target folder
constexpr UINT WM_TRAY_RESTORE = WM_APP + 102;   // the 已清空暂存区 snack: put the items back
constexpr UINT WM_TRAY_TAKE_CLIP = WM_APP + 103;  // stage the Ctrl+C / Ctrl+X offer (剪贴板 N 项 · 收入)
constexpr UINT WM_TAG_RULE_ACCEPT = WM_APP + 104;  // the 自动规则 snack's 加上
constexpr UINT WM_GIT_RESULT = WM_APP + 105;  // in-app Git: lParam owns a finished job (git_ui.cpp)
constexpr UINT WM_CLOUD_PIN_DONE = WM_APP + 106;  // 始终保留 / 释放空间 finished: wParam CloudPinAction, lParam failed count
constexpr UINT WM_CLOUD_DOWNLOAD = WM_APP + 107;  // 下载并预览: wParam app::CloudDownloadEvent, lParam job id
constexpr UINT WM_DEVICES_CHANGED = WM_APP + 108;  // DeviceRegistry: the phone list changed (device_registry.h)
constexpr UINT WM_AI_UI_TASK = WM_APP + 109;  // host: lParam std::shared_ptr<UiTask>*, an AI tool's UI step (ai_tools_ui.h)
constexpr UINT WM_AI_STATUS = WM_APP + 110;   // host: what AI clients do changed (ai_status.h)
constexpr UINT WM_AI_SNACK_UNDO = WM_APP + 111;  // the AI 新建 snack: undo that change (ai_settings_ui.h)
constexpr UINT WM_AI_SNACK_OPEN = WM_APP + 112;  // the AI 新建 snack: show the new file
constexpr UINT WM_LAN_DISCOVERY = WM_APP + 113;  // host: a LAN device look finished (app_network_page.h)
constexpr UINT WM_NETWORK_MAP_DONE = WM_APP + 114;  // window: 连接服务器's drive mapping finished (NetworkMapResult*)
constexpr UINT WM_UI_KICK = WM_APP + 115;  // a background producer has results: run the kTimerUi tick now
constexpr UINT WM_COMMAND_PALETTE = WM_APP + 116;  // the address editor got ">": open the command palette
constexpr UINT WM_RECYCLE_REFUSED = WM_APP + 117;  // window: a recycle left items the bin would not keep (AppState::recycleRefused)
constexpr UINT WM_DOUBLE_CTRL = WM_APP + 118;  // two Ctrl taps anywhere: toggle the quick search popup
constexpr UINT WM_DIALOG_JUMP_CONTEXT = WM_APP + 119;  // the dialog jump bar wants the open folders
constexpr UINT WM_DEVICE_WATCH_READY = WM_APP + 120;  // worker: wParam ticket, lParam PIDLIST_ABSOLUTE of the phone folder to watch (app_runtime.h)
constexpr UINT WM_DEVICE_FOLDER_CHANGED = WM_APP + 121;  // SHChangeNotify on the watched phone folder (SHCNRF_NewDelivery)
// The failed pack download card's buttons (pack_failure_ui.h).
constexpr UINT WM_PACK_RETRY = WM_APP + 122;
constexpr UINT WM_PACK_SOURCES = WM_APP + 123;
constexpr UINT WM_PACK_CHECK_UPDATE = WM_APP + 124;
constexpr UINT WM_PACK_DETAILS = WM_APP + 125;
constexpr UINT WM_PACK_OPEN_FOLDER = WM_APP + 126;
constexpr UINT WM_SOURCE_PROBE_DONE = WM_APP + 127;  // a 测速 probe finished (download_source_probe.h)

constexpr UINT WM_WINDOW_ARRANGE_UNDO = WM_APP + 128;
constexpr UINT WM_DIALOG_MANAGEMENT_RESULT = WM_APP + 131;

}  // namespace pulse

constexpr UINT WM_REOPEN_WORKSPACE_PANE = WM_APP + 132;

constexpr UINT WM_RECYCLE_SEARCH = WM_APP + 134;
constexpr UINT WM_DETAILS_TREE_RENAME = WM_APP + 135;

constexpr UINT WM_RECYCLE_SHELL_CHANGED = WM_APP + 136;

constexpr UINT WM_DETAILS_PREVIEW_NAVIGATE = WM_APP + 137;
constexpr UINT WM_DETAILS_PREVIEW_OPEN = WM_APP + 138;
constexpr UINT WM_DETAILS_PREVIEW_COMMAND = WM_APP + 139;
constexpr UINT WM_DRAG_RETURN = WM_APP + 143;
constexpr UINT WM_HISTORY_REDO = WM_APP + 144;
