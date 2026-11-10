// app_core.h - Process-wide state shared by every Pulse window.
//
// Multi-window step M1: services, data stores and process settings live in
// AppCore; AppState keeps the per-window UI state and reaches these members
// through `AppState::core` (plus same-named reference aliases while call sites
// migrate). One AppCore outlives every AppState that uses it.
#pragma once
#include "recycle_refresh_state.h"
namespace pulse::app { class RecycleShellWatch; class ExtensionStoreController; }
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_watch.h"
#include "../index/content_search_client.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"
#include "../ops/ops_manager.h"
#include "../ui/FluentTokens.h"
#include "../ui/render_caches.h"
#include "app_change_tracking.h"
#include "app_model.h"
#include "app_prefs.h"
#include "app_worker.h"
#include "closed_tabs.h"
#include "closed_windows.h"
#include "context_menu_controller.h"
#include "context_menu_prefs.h"
#include "dir_notify_queue.h"
#include "dir_notify_async.h"
#include "duplicate_scan.h"
#include "explorer_window_takeover.h"
#include "win_e_hotkey.h"
#include "external_open.h"
#include "external_open_notice.h"
#include "folder_sizes.h"
#include "dialog_jump.h"
#include "double_ctrl.h"
#include "global_search_hotkey.h"
#include "global_search_window.h"
#include "jump_history.h"
#include "host_window.h"
#include "window_registry.h"
#include "live_network_search.h"
#include "places.h"
#include "saved_search.h"
#include "search_history.h"
#include "settings_controller.h"
#include "shell_window_plan.h"
#include "shell_window_registry.h"
#include "single_instance_coordinator.h"
#include "tray_controller.h"
#include "unc_probe_scheduler.h"
#include "update_checker.h"
#include "update_installer.h"
#include <windows.h>
#include <d2d1.h>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse {

struct SidebarRefreshLoad;
struct NetworkLocationLoad;
struct NetworkPageLoad;

struct AppState;
struct DialogManagementUiState;
struct DriveEjectTask;

struct AppCore {
    AppCore() = default;
    AppCore(const AppCore&) = delete;
    AppCore& operator=(const AppCore&) = delete;

    fs::SnapshotStore store;
    app::WorkerPool worker;
    // Shared by all settings windows; destroyed after the live window loop ends.
    std::shared_ptr<app::ExtensionStoreController> extension_store;
    fs::DirWatchSet watches;
    std::shared_ptr<DriveEjectTask> drive_eject;
    struct DirNotifyBatch {
        std::wstring path;
        bool overflow = false;
        std::vector<fs::DirNotifyEvent> events;
    };
    std::mutex notify_mu;
    std::vector<DirNotifyBatch> notify_queue;
    // Taken from notify_queue on the UI thread (DrainDirNotifies); holds the
    // batches of folders that were patched moments ago.
    app::DirNotifyPacer<DirNotifyBatch> notify_pacer;
    app::DirNotifyAsync notify_async;
    uint64_t notify_async_round = 0;
    struct CoalescedSizePatch {
        std::wstring path;
        std::wstring name;
        ULONGLONG due = 0;
    };
    std::vector<CoalescedSizePatch> size_patches;
    ULONGLONG last_unc_poll = 0;

    app::SidebarModel sidebar;
    std::shared_ptr<SidebarRefreshLoad> sidebarRefresh;
    std::shared_ptr<NetworkLocationLoad> networkLocations;
    // The network page's 最近访问 and LAN discovery cache (app_network_page.cpp).
    std::shared_ptr<NetworkPageLoad> networkPage;
    fs::RecycleBinInfo recycle_info;
    app::RecycleRefreshGate recycle_refresh_gate;
    std::shared_ptr<app::RecycleShellWatch> recycle_shell_watch;
    // SHQueryRecycleBin and $I files lag IFileOperation; retry occupancy/list
    // after recycle mutations and ignore occupancy that still matches the
    // pre-mutation count while an optimistic update is showing.
    ULONGLONG recycle_refresh_at = 0;
    int recycle_refresh_left = 0;
    uint64_t recycle_ignore_items = 0;
    bool recycle_info_guard = false;

    app::StagingTray tray;
    app::PlacesCatalog places;
    app::ContextMenuPrefs ctxMenuPrefs;
    // Explorer COM/static menu session, caches, and delayed refresh state.
    app::ContextMenuController context_menu;
    app::AppPrefs appPrefs;
    app::SearchHistory searchHistory;
    app::SearchHistoryWriter searchHistoryWriter;
    app::SettingsController settings;
    app::UpdateChecker update_checker;
    app::UpdateResult update_result;
    bool update_result_ready = false;
    app::UpdateInstaller update_installer;
    DWORD update_install_error = ERROR_SUCCESS;
    ULONGLONG next_update_progress_paint = 0;
    ULONGLONG next_update_check = GetTickCount64() + 15000;
    // Background preview pack updates (TickUpdates): half a minute after
    // start, then every update check interval.
    ULONGLONG next_pack_update_check = GetTickCount64() + 30000;
    std::wstring notified_update_version;

    std::wstring whatsNewVersion;      // set once after an upgrade; toast shown at whatsNewAt
    ULONGLONG whatsNewAt = 0;

    app::TrayController tray_controller;

    GlobalSearchHotkey globalSearchHotkey;
    DoubleCtrlWatcher doubleCtrl;          // Ctrl Ctrl summons the quick search popup
    // Frequent places, shared by quick search and the dialog jump bar.
    std::shared_ptr<app::JumpHistory> jumpHistory = std::make_shared<app::JumpHistory>();
    app::DialogJump dialogJump;            // jump bar under other programs' Open / Save dialogs
    GlobalSearchWindow globalSearchWindow;
    app::SingleInstanceCoordinator single_instance;

    uint64_t tagAdsResultRevision = 0;
    std::unordered_set<std::wstring> tagAdsDiscoveryQueued;
    std::unordered_set<std::wstring> tagAdsDiscoveryChecked;
    index::IndexClient index;
    app::FolderSizes folderSizes;
    ChangeTrackingUi changes;
    index::NetworkAgentClient networkIndex;
    index::ContentSearchClient contentSearch;

    index::ContentSearchClient duplicateSearch;
    app::DuplicateScanSession duplicateScan;

    std::vector<index::VolumeInfo> dup_volume_cache;
    ULONGLONG dup_volume_cache_tick = 0;
    bool dup_volume_cache_pending = false;
    app::SavedSearchStore savedSearches;
    struct PendingIndexSearch {
        index::Query query;
        index::SearchResult local;
        index::SearchResult network;
        index::SearchResult recycle;
        std::shared_ptr<std::atomic<bool>> recycle_cancel;
        bool recycle_ready = false;
        bool local_ready = false;
        bool network_ready = false;
        bool network_snapshot = false; // Captured for this request, not global root configuration.
        bool network_expected = true;  // Network roots or a live walk exist; else its failure is ignored.
        std::wstring live_network_root;  // #74: scope walked live instead of the network index
    };
    std::unordered_map<uint32_t, PendingIndexSearch> pendingIndexSearches;
    LiveNetworkSearchSessions liveNetworkSearches;

    // Session autosave: last written JSON, so unchanged state is never rewritten.
    std::wstring sessionSavedJson;
    std::wstring sessionSavedLayoutJson;
    std::shared_ptr<const std::wstring> sessionSavedUndo;
    // Shutdown messages can arrive while WM_CREATE is rolling back.
    bool updateSessionPrepared = false;
    bool restoreUpdateSession = false;
    std::wstring prefsSavedJson;
    ULONGLONG sessionAutosaveCheck = 0;
    // Last input, paint, window change or dirty UI tick in any window, and the
    // last autosave pass that serialized the session (TickSessionAutosave).
    ULONGLONG sessionActivity = 0;
    ULONGLONG sessionAutosaveFull = 0;
    uint32_t nextIndexReq = 1;

    app::UncProbeScheduler probe_scheduler;
    std::wstring probeUnc;
    std::vector<std::wstring> probeQueue;

    bool darkMode = false;

    ui::ThemeMode themeOverride = ui::ThemeMode::Auto;
    bool safeMode = false;
    bool startupComplete = false; // WM_CREATE committed loaded/restored state; rollback must not save.
    bool isolatedTest = false;
    bool isolatedTestPersist = false; // selftest builds: test instance saves into PULSE_TEST_DATA_DIR
    bool contentIndexObserver = false;
    D2D1_COLOR_F accentColor;

    std::wstring session_path;

    std::unordered_map<std::wstring, std::wstring> gitRoots;

    std::mutex resultMutex;
    std::queue<app::WorkResult> results;

    // Stage 1B-1: ops layer (queue + undo), inline rename overlay.
    ops::OpsManager ops;

    // Panes as shell windows for "open file location" (shell_window_sync.h).
    std::unique_ptr<app::ShellWindowRegistry> shell_windows;
    std::vector<app::ShellWindowEntry> shell_windows_published;
    std::unique_ptr<app::ExplorerWindowTakeover> explorer_takeover;   // experimental (B站 #1 2b)
    std::unique_ptr<app::WinEHotkey> win_e_hotkey;

    // Cut state mirrored into list rows (ui.md §5.2 rule 6: 55% opacity).
    std::vector<std::wstring> cutPaths;
    DWORD pendingCutClipboardSequence = 0;
    // Clipboard value CollectPathsToTray wrote for tray batch trayClipboardBatchId.
    DWORD trayClipboardSequence = 0;
    uint64_t trayClipboardBatchId = 0;
    std::vector<std::wstring> pendingCutClipboardPaths;
    std::vector<std::wstring> completedCutClipboardPaths;

    // Icon and thumbnail caches every window's renderer draws from.
    ui::RenderCaches render_caches;

    // Every Pulse window of the process, most recently activated first.
    app::BasicWindowRegistry<AppState> windows;
    // The first window's WM_CREATE set up the process (services, saved state,
    // tray, ops); later windows only set up themselves (M4a).
    bool started = false;
    // Windows closed while others stayed open, for the restore toast and
    // Ctrl+Shift+T (M4c); ExitPulse sets `exiting` so its closes are not kept.
    app::ClosedWindowStack closed_windows;
    // Tabs closed by hand (M7f), and the number of the last close of a tab
    // or window, which orders the two for Ctrl+Shift+T.
    app::ClosedTabStack closed_tabs;
    uint64_t close_seq = 0;
    bool exiting = false;
    // Session v8 (M4d): the windows of the last session other than the focused
    // one (which the first window restores), most recent first. The first
    // window opens them once it has been shown (WM_OPEN_SESSION_WINDOWS); until
    // then session saves keep them, so a launch into the tray loses nothing.
    std::vector<app::WindowSessionSnapshot> pending_session_windows;
    // Folders opened from outside Pulse (M7a, open_routing.h): the batch the
    // last ones formed, its window and, while it is an existing window, the
    // tab the batch's first folder opened in there.
    app::ExternalOpenNotice open_failure_notice;
    uint64_t open_wait_tick = 0;
    app::ExternalOpenBatch open_batch;
    HWND open_batch_window = nullptr;
    const void* open_batch_tab = nullptr;  // app::LayoutTab*, compared only
    // The toast after the last such open (M7b): what it offers, the window it
    // shows in, the tab it is about and, for "make it a tab", where the tabs
    // go back to. The streak of conversions, and the default the "make it
    // the default" toast proposes.
    app::OpenConversion open_convert = app::OpenConversion::None;
    HWND open_convert_window = nullptr;
    const void* open_convert_tab = nullptr;  // app::LayoutTab*, compared only
    HWND open_convert_into = nullptr;
    app::OpenConversionStreak open_convert_streak;
    app::ExternalOpenPolicy open_suggested = app::ExternalOpenPolicy::Smart;

    // Hidden process window (host_window.h): single-instance endpoint today,
    // tray/hotkey/service callbacks as M2 continues. Destroyed before the
    // state its handler reads, after management has revoked its worker wake.
    app::HostWindow host;
    std::shared_ptr<DialogManagementUiState> dialogManagement;
};

// Owns the AppCore of a stand-alone AppState. A base class, so the core exists
// before AppState's members bind their references to it.
struct AppCoreOwner {
    explicit AppCoreOwner(std::unique_ptr<AppCore> core = nullptr) noexcept : owned_core(std::move(core)) {}
    std::unique_ptr<AppCore> owned_core;
};

}  // namespace pulse
