// app_state.h — Window process state, shot request, and WM_APP message ids.
#pragma once
#include "tray_compare_metadata.h"
#include "../common/path_utils.h"

#include "../ui/ui_compositor.h"
#include "../ui/notification_toast.h"
#include "../ui/ui_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/quick_preview_window.h"
#include "list_type_ahead.h"
#include "preview_selection_follow.h"
#include "../ui/text_diff_window.h"
#include "../ui/bloom_accent_picker.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_watch.h"
#include "app_model.h"
#include "live_network_search.h"
#include "tray_compare_metadata.h"
#include "frame_pump.h"
#include <deque>
#include <atomic>
#include <memory>
#include <mutex>
#include "content_results_ui.h"
#include "app_worker.h"
#include "places.h"
#include "app_network_locations.h"
#include "details_meta.h"
#include "folder_sizes.h"
#include "context_menu_prefs.h"
#include "context_menu_controller.h"
#include "shell_verbs.h"
#include "app_prefs.h"
#include "explorer_window_takeover.h"
#include "shell_window_registry.h"
#include "saved_search.h"
#include "search_history.h"
#include "settings_controller.h"
#include "sidebar_scrollbar_fade.h"
#include "single_instance_coordinator.h"
#include "tray_controller.h"
#include "global_search_hotkey.h"
#include "global_search_window.h"
#include "tab_controller.h"
#include "update_checker.h"
#include "update_installer.h"
#include "session.h"
#include "duplicate_scan.h"
#include "../index/index_client.h"
#include "app_change_tracking.h"
#include "unc_probe_scheduler.h"
#include "../index/network_agent_client.h"
#include "../index/content_search_client.h"
#include "../ops/ops_manager.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
constexpr UINT kTimerUi = 1;
// Scrolls the list while a marquee is dragged past its edge.
constexpr UINT_PTR kTimerMarqueeScroll = 0x4D51;
constexpr UINT WM_NETWORK_LOCATIONS = WM_APP + 73;
constexpr UINT WM_ASSOC_CHANGED = WM_APP + 74;  // SHCNE_ASSOCCHANGED: default programs changed
constexpr UINT WM_BITLOCKER_UNLOCK = WM_APP + 75;  // app::BitLockerUnlockResult* (bitlocker_unlock.h)

enum class OmnibarMode { Path, Mixed, Command, Project };

struct TrayDeckEntry {
    int batch = -1;
    int sub = -1;
    const app::TrayItem* item = nullptr;
};

struct TagAdsDiscovery {
    std::wstring path;
    std::vector<app::TagAdsRecord> records;
    std::vector<std::wstring> legacy_names;
};

struct ShotRequest {
    bool active = false;
    std::wstring path;
    std::wstring output;
    bool force_dark = false;
    int width = 0;
    int height = 0;
    ui::ViewMode view_mode = ui::ViewMode::Details;
    std::wstring language;
    bool update_available = false;
    std::wstring update_state;
    std::chrono::steady_clock::time_point start;
};

struct Timing {
    double first_frame_ms = 0.0;
    double sort_done_ms = 0.0;
    double enum_ms = 0.0;
    double sort_ms = 0.0;
    // Frame breakdown used by the performance overlay.  Draw includes the
    // UI model build and D2D EndDraw; present includes swap-chain pacing.
    double draw_ms = 0.0;
    double present_ms = 0.0;
    bool first_frame_recorded = false;
};

// Byte-by-byte comparison of the two staged files, run on a worker thread.
struct TrayCompareJob {
    std::wstring a, b;
    std::atomic<int> state{1}; // 1 running, 2 identical, 3 different, 4 unreadable
};

// Text-or-binary probe (first 8 KB of both files) for the tray compare "view diff" link.
struct TrayTextProbe {
    std::wstring a, b;
    std::atomic<int> state{0}; // 0 running, 1 both text, 2 binary / unreadable
};

struct SidebarRefreshLoad;
struct AppState {
    HWND hwnd = nullptr;
    ui::Compositor compositor;
    ui::NotificationToast notification_toast;
    ui::MainRenderer renderer;
    // Vsync-paced frames while highlight glides / list shifts run (frame_pump.h).
    app::FramePump framePump;
    ui::QuickPreviewWindow quickPreview;
    // View row the quick preview was anchored to when Delete ran from inside
    // it; SyncQuickPreview re-anchors there once the listing drops the entry
    // so the preview steps to the neighbouring file instead of closing.
    int quickPreviewAnchorView = -1;
    app::PreviewSelectionFollow quickPreviewSelection;
    app::ListTypeAhead listTypeAhead;
    const app::Tab* listTypeAheadTab = nullptr;
    std::wstring listTypeAheadFolder;

    app::WindowTabs window_tabs;
    app::Pane* pane = nullptr;          // focused leaf of the current layout tab
    app::Pane* targetPane = nullptr;    // Ctrl+Alt+D marked destination

    fs::SnapshotStore store;
    app::WorkerPool worker;
    fs::DirWatchSet watches;
    struct DirNotifyBatch {
        std::wstring path;
        bool overflow = false;
        std::vector<fs::DirNotifyEvent> events;
    };
    std::mutex notify_mu;
    std::vector<DirNotifyBatch> notify_queue;
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
    fs::RecycleBinInfo recycle_info;
    // SHQueryRecycleBin and $I files lag IFileOperation; retry occupancy/list
    // after recycle mutations and ignore occupancy that still matches the
    // pre-mutation count while an optimistic update is showing.
    ULONGLONG recycle_refresh_at = 0;
    int recycle_refresh_left = 0;
    uint64_t recycle_ignore_items = 0;
    bool recycle_info_guard = false;
    uint32_t sidebarCollapsedMask = 0;
    ULONGLONG sidebarPeekArmAt = 0;     // rail hover start; 0 = not armed
    uint32_t sidebarHiddenMask = 0;  // Section menu: hidden groups are not drawn.
    // Section display order (logical SidebarSectionId values). Header drags
    // rewrite it; the masks above are indexed by id, so they survive a reorder.
    std::vector<int> sidebarOrder = app::DefaultSidebarOrder();
    // Built-in quick-access links switched off in the section menu.
    uint32_t sidebarQuickAccessHiddenMask = 0;
    bool starredExpanded = true;
    float sidebarScroll = 0.0f;
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
    std::wstring notified_update_version;
    int settingsReleaseExpanded = 0;   // Settings > About release-note row that is open, -1 none
    std::wstring whatsNewVersion;      // set once after an upgrade; toast shown at whatsNewAt
    ULONGLONG whatsNewAt = 0;
    app::TabController tabs;
    app::TrayController tray_controller;
    bool hidden_to_tray = false;       // the close button hid the window (tray_reveal.h)
    GlobalSearchHotkey globalSearchHotkey;
    GlobalSearchWindow globalSearchWindow;
    app::SingleInstanceCoordinator single_instance;
    ui::BloomAccentPicker bloom_accent;
    uint64_t tagAdsResultRevision = 0;
    std::unordered_set<std::wstring> tagAdsDiscoveryQueued;
    std::unordered_set<std::wstring> tagAdsDiscoveryChecked;
    index::IndexClient index;
    app::FolderSizes folderSizes;
    ChangeTrackingUi changes;
    index::NetworkAgentClient networkIndex;
    index::ContentSearchClient contentSearch;
    std::shared_ptr<ContentSelectionAction> contentSelectionAction;
    index::ContentSearchClient duplicateSearch;
    app::DuplicateScanSession duplicateScan;
    uint64_t dup_view_epoch = 0;
    std::wstring dup_view_language;
    std::vector<ui::DuplicateGroupView> dup_view_cache;
    std::vector<index::VolumeInfo> dup_volume_cache;
    ULONGLONG dup_volume_cache_tick = 0;
    bool dup_volume_cache_pending = false;
    app::SavedSearchStore savedSearches;
    struct PendingIndexSearch {
        index::Query query;
        index::SearchResult local;
        index::SearchResult network;
        bool local_ready = false;
        bool network_ready = false;
        bool network_snapshot = false; // Captured for this request, not global root configuration.
        bool network_expected = true;  // Network roots or a live walk exist; else its failure is ignored.
        std::wstring live_network_root;  // #74: scope walked live instead of the network index
    };
    std::unordered_map<uint32_t, PendingIndexSearch> pendingIndexSearches;
    LiveNetworkSearchSessions liveNetworkSearches;
    std::vector<index::Hit> paletteHits;
    size_t paletteTotal = 0;
    std::wstring paletteQuery;
    std::wstring paletteIssuedNeedle;
    std::wstring paletteIssuedPrefix;
    bool paletteIssuedFolders = false;
    uint32_t paletteSearchId = 0;
    // Live match count for the open advanced-search dialog.
    uint32_t advancedCountId = 0;
    HWND advancedCountHwnd = nullptr;
    // Session autosave: last written JSON, so unchanged state is never rewritten.
    std::wstring sessionSavedJson;
    // Shutdown messages can arrive while WM_CREATE is rolling back.
    bool updateSessionPrepared = false;
    bool restoreUpdateSession = false;
    std::wstring prefsSavedJson;
    ULONGLONG sessionAutosaveCheck = 0;
    uint32_t nextIndexReq = 1;
    bool paletteSearching = false;
    app::UncProbeScheduler probe_scheduler;
    std::wstring probeUnc;
    std::vector<std::wstring> probeQueue;

    bool darkMode = false;
    unsigned settingsExpanded = ui::kSettingsDefaultExpandedMask;
    ui::ThemeMode themeOverride = ui::ThemeMode::Auto;
    bool safeMode = false;
    bool startupComplete = false; // WM_CREATE committed loaded/restored state; rollback must not save.
    bool isolatedTest = false;
    bool isolatedTestPersist = false; // selftest builds: test instance saves into PULSE_TEST_DATA_DIR
    bool contentIndexObserver = false;
    D2D1_COLOR_F accentColor;
    float scale = 1.0f;
    bool maximized = false;
    bool backdropActive = true;

    bool showFps = false;
    bool forceStatusPerformance = false; // --fps for this process only
    double lastFrameMs = 0.0;
    double lastFps = 0.0;
    std::chrono::steady_clock::time_point lastFrameTime;
    std::deque<std::chrono::steady_clock::time_point> fpsWindow; // recent Present times
    double processCpuPercent = 0.0;
    double workingSetMb = 0.0;
    ULONGLONG processSampleTick = 0;
    uint64_t lastProcessTime100ns = 0;

    int hoverRow = -1;
    int hoverPaneIndex = -1;
    ULONGLONG ctxHoverSince = 0;
    std::wstring ctxHoverPrefetched;
    int dropPaneIndex = -1;
    bool dropHeader = false;            // drop on pane title bar → open folder
    bool scrollbarHovered = false;
    bool scrollbarDragging = false;
    bool scrollbarHorizontal = false;
    bool scrollbarSidebar = false;
    int scrollbarDragStartX = 0;
    int scrollbarDragStartY = 0;
    float scrollbarGrabOffset = 0.0f;
    float scrollbarDragStartScroll = 0.0f;
    float scrollbarHoverWidth = 0.0f;
    // Sidebar scrollbar shows while scrolling or hovered, then fades (#44).
    bool sidebarScrollbarHot = false;
    float sidebarScrollSeen = 0.0f;
    app::ScrollbarFade sidebarScrollbarFade;

    bool splitterDragging = false;
    int splitterDragIndex = -1;
    D2D1_RECT_F splitterParentBounds{};
    app::SplitOrientation splitterOrientation = app::SplitOrientation::Vertical;
    std::vector<app::SplitContainer*> splitterNodes;

    bool columnResizing = false;
    int columnResizeIndex = -1;
    int columnResizePane = -1;

    // Column view divider drag and click bookkeeping (app_column_view.cpp).
    bool stripResizing = false;
    int stripResizePane = -1;
    int stripResizeColumn = -1;
    int stripResizeStartX = 0;
    float stripResizeStartDip = 0.0f;
    ULONGLONG stripClickTick = 0;
    bool stripHScrolling = false;        // ancestor scrollbar thumb drag
    float stripHScrollStartDip = 0.0f;   // scroll_from_right_dip at press
    float stripHScrollRatio = 0.0f;      // strip px per thumb px
    float stripHScrollMaxDip = 0.0f;

    bool tabDragPending = false;
    bool tabDragging = false;
    int tabDragIndex = -1;               // window_tabs.items index of the dragged tab
    POINT tabDragStartPt{};
    // Double-click close (pulse::HandleTabDoubleClick): the tab the last press
    // landed on (strip tab or vertical tab row, never a close button) and when.
    const void* tabPressTab = nullptr;
    LONG tabPressTime = 0;
    int tabDragLastX = 0;
    float tabDragPressLeft = 0.0f;       // rest-slot left at press (for grab-offset follow)
    float tabDragFloatLeft = 0.0f;       // dragged tab left (px, cursor-delta driven)
    float tabFlowLeft = 0.0f;            // tab strip rest range / slot geometry (px)
    float tabFlowRight = 0.0f;
    float tabSlotW = 0.0f;
    float tabPitch = 0.0f;
    std::vector<int> tabOrder;           // display position -> window_tabs.items index
    int tabDragRunPos = 0;               // run start within tabOrder (group drags)
    int tabDragRunLen = 1;               // >1: the whole group run moves as a block
    bool tabDragFromChip = false;        // drag started on the group chip
    int tabDragGroupId = 0;              // chip drag: app::TabGroup::id
    float tabDragSlots = 1.0f;           // visual width of the drag block in slots
    float tabDragBlockW = 0.0f;          // >0: collapsed chip drag block (chip+gap, px)
    struct TabTrack { float start = 0.0f; int durationMs = 150; std::chrono::steady_clock::time_point t0; };
    std::unordered_map<const app::LayoutTab*, TabTrack> tabTracks;
    std::unordered_map<const app::LayoutTab*, float> tabOffsets;
    // Chip slide channel (px, key = app::TabGroup::id) for collapsed-group
    // drags: chips are px-positioned, so slot-unit tab offsets can't move them.
    std::unordered_map<int, TabTrack> chipTracks;
    std::unordered_map<int, float> chipOffsets;

    // Smooth scroll animation.
    bool scrollAnimating = false;
    float scrollTargetY = 0.0f;
    float scrollTargetX = 0.0f;
    bool scrollHorizontal = false;
    std::chrono::steady_clock::time_point scrollLastUpdateTime;
    // Keep wheel input visually attached to the content. A longer response
    // reads as lag when a frame is already close to the 16.7 ms budget.
    static constexpr double kScrollResponseMs = 28.0;

    ShotRequest shot;
    std::wstring open_path;   // folder to open as a new tab after session restore
    Timing timing;
    std::wstring session_path;
    std::vector<app::LayoutTabSnapshot> session_layout_tabs;
    std::vector<app::GroupSessionSnapshot> session_tab_groups;
    int session_active_layout_tab = 0;
    std::unordered_map<std::wstring, std::wstring> gitRoots;

    // 1B-2 GUI verification: render the context menu to a PNG, no interaction.
    bool menushot = false;
    std::wstring menushot_out;
    // Menushot variant: pretend the active tab is a search results view.
    bool menushot_search = false;
    // GUI verification for the color picker popup (QFluent replica).
    bool colorpickshot = false;
    bool colorpickshot_dark = true;
    std::wstring colorpickshot_out;
    // Smoke test: open the interactive color picker dialog on launch.
    bool colorpickdialog = false;
    // GUI verification for the tray card deck: stage a few files pre-shot.
    bool shot_tray = false;
    int shot_tray_count = 4;
    // GUI verification for tab colors: three tabs, red/yellow/blue strips.
    bool shot_tab_colors = false;
    // GUI verification for pinned-tab spacing and the adjacent new-tab button.
    bool shot_pinned_tab = false;
    // GUI verification for the in-place tag rename field frame.
    bool shot_tag_rename = false;
    // GUI verification for the tag drag-reorder: stage a mid-drag frame.
    bool shot_tag_drag = false;
    // GUI verification for a rail hover hint (staged after the message pump).
    bool shot_tooltip = false;
    // GUI verification for the details panel: open it with a pre-selection.
    bool shot_details = false;
    int shot_group = 0;   // --shot-group N: group the shot folder (1 name .. 6 location)
    bool shot_details_multi = false;
    float shot_scale_override = 0.0f;
    bool shot_high_contrast = false;

    std::mutex resultMutex;
    std::queue<app::WorkResult> results;

    HWND hwndAddressEdit = nullptr;
    bool addressEditing = false;
    bool addressSearching = false;
    bool searchHistoryOpen = false;
    bool searchScopePending = false;
    bool sidebarResizing = false;
    bool addressSearchComposing = false;
    ULONGLONG addressLiveDue = 0;
    ULONGLONG addressHistoryDue = 0;
    std::wstring addressLiveContext;
    std::wstring addressHistoryPath;
    bool addressSearchCurrent = false;
    bool addressSearchContent = false;
    bool searchOptionsPending = false;
    ULONGLONG contentStatusTick = 0;
    std::wstring contentStatusText;
    bool pinyinReadyLast = false;
    std::wstring addressSearchRoot;
    // Query shown when the search box was opened; the first Esc returns to it.
    std::wstring addressSearchEntryText;
    // Middle-click tabs open after the opener, in click order (browser style).
    const void* backgroundTabOpener = nullptr;
    size_t backgroundTabRun = 0;
    // Attention pulse on a tab reused instead of opening a duplicate (key is
    // only compared, never dereferenced).
    const app::LayoutTab* tabFlashKey = nullptr;
    ULONGLONG tabFlashAt = 0;
    uint64_t tagGroupsRevision = 0;  // tag catalog revision last published for grouping
    float addressSearchAnimation = 0.0f;
    float addressScopeAnimation = 0.0f;
    ULONGLONG addressAnimationTick = 0;
    bool addressIgnoreKillFocus = false;
    HWND hwndFilterEdit = nullptr;
    bool filterEditing = false;
    bool filterIgnoreKillFocus = false;
    bool filterFocusPending = false;
    bool filterSelectMode = false;
    std::wstring filterSelectRestore;
    std::wstring filterCue;

    // Stage 1B-1: ops layer (queue + undo), inline rename overlay.
    ops::OpsManager ops;
    std::wstring pending_undo_json; // loaded from session, imported in WM_CREATE
    uint64_t opsCompleted = 0;
    std::unique_ptr<ui::FileOperationWindow> operationWindow;
    uint64_t operationUiTaskId = 0;
    uint64_t operationDismissedTaskId = 0;
    uint64_t conflictUiToken = 0;
    uint64_t lockPromptTaskId = 0;   // failed op whose "file in use" prompt was shown
    bool operationAutoShown = false;
    bool operationPinnedByUser = false;
    std::chrono::steady_clock::time_point operationStartedAt{};
    std::chrono::steady_clock::time_point operationFinishedAt{};
    HWND hwndRenameEdit = nullptr;
    int renameIndex = -1;
    bool renameIgnoreKillFocus = false;
    bool renameClickCandidate = false;
    app::Pane* renameClickPane = nullptr;
    app::Tab* renameClickTab = nullptr;
    int renameClickIndex = -1;
    ULONGLONG renameClickDue = 0;
    ULONGLONG renameClickStarted = 0;
    std::wstring renameClickPath;
    HWND hwndTagRenameEdit = nullptr;
    app::TagId tagRenameId;
    bool tagRenameIgnoreKillFocus = false;
    HFONT editFont = nullptr;
    HBRUSH editBrush = nullptr;

    // Stage 1B-2: Fluent menus, OLE drag & drop, breadcrumb segments.
    std::unique_ptr<ui::FluentMenu> menu;
    ui::WindowDropTarget* dropTarget = nullptr; // ref held by RegisterDragDrop
    int breadcrumbHover = -1;
    int hoverRegion = 0;
    int hoverControlIndex = -1;
    int hoverSubIndex = -1;
    ULONGLONG hoverSince = 0;
    POINT hoverPoint{};
    std::wstring tooltipText;
    std::wstring hoverPath;
    // Name of the hovered sidebar row or section: the collapsed rail shows icons
    // only, so its tooltips read this.
    std::wstring hoverLabel;

    // Drag-over feedback state (rendered via WindowViewModel).
    int dropRow = -1;
    int dropBreadcrumb = -1;
    int dropSidebar = -1;
    bool dropTray = false;
    bool dropQuickAccess = false;  // folders over the Quick access header: pin
    std::wstring dropBadge;
    float dropBadgeX = 0.0f;
    float dropBadgeY = 0.0f;
    bool dropBadgeMove = false;
    bool trayDragOut = false;          // tray contents are being dragged out (not a tray target)
    ULONGLONG trayProbeAt = 0;         // last staging-tray existence probe (UI timer, 2 s)
    bool trayCompare = false;          // two-file compare table replaces the card stack
    std::unique_ptr<app::TrayCompareMetadata> trayMetadata;
    std::shared_ptr<TrayCompareJob> trayCmpJob; // on-demand byte comparison
    std::shared_ptr<TrayTextProbe> trayTextProbe; // text/binary probe for "view diff"
    std::unique_ptr<ui::TextDiffWindow> textDiff; // stand-alone text compare window
    // Panes as shell windows for "open file location" (shell_window_sync.h).
    std::unique_ptr<app::ShellWindowRegistry> shell_windows;
    std::vector<app::ShellWindowEntry> shell_windows_published;
    std::unique_ptr<app::ExplorerWindowTakeover> explorer_takeover;   // experimental (B站 #1 2b)
    // One-time teaching bubbles (tips_seen bits in appPrefs).
    int teachTip = -1;                 // visible tip, -1 = none
    int teachCandidate = -1;           // trigger currently holding
    ULONGLONG teachCandidateSince = 0;
    ULONGLONG teachShownAt = 0;
    bool teachShownThisSession = false; // at most one tip per launch
    std::wstring dropDestDir;                 // resolved drop destination ("" = none/tray)
    std::wstring dropLaunchProgram;           // program row that opens the dropped items

    // Spring-loaded folder enter during drag-over (ui.md §7.8).
    int springRow = -1;
    ULONGLONG springStart = 0;
    bool springEntered = false;

    // Drag-out bookkeeping.
    bool dragPending = false;
    POINT dragStartPt{};
    int clickCollapseIndex = -1;
    bool clickToggleOnRelease = false;

    // Tag drag-reorder gesture (sidebar 标签组), QFluentKit TabBar model:
    // the dragged tag follows cursor deltas 1:1, swaps with a sibling when its
    // edge crosses the sibling's center (sibling slides 150 ms InOutQuad), and
    // on release slides home with a distance-proportional duration.
    bool tagDragPending = false;
    bool tagDragActive = false;
    POINT tagDragStartPt{};
    int tagDragTag = -1;                 // places.tags index of the dragged tag
    std::wstring tagDragPath;            // pulse:tag:<index> at press time
    int tagDragLastY = 0;
    float tagDragGrabDy = 0.0f;          // cursor y minus dragged slot top at activation
    float tagDragFloatTop = 0.0f;        // dragged slot top (px, cursor-delta driven)
    float tagDragY = 0.0f;               // dragged slot center for the renderer
    float tagFlowTop = 0.0f;             // tag flow range / slot geometry (px)
    float tagFlowBottom = 0.0f;
    float tagSlotH = 0.0f;
    float tagPitch = 0.0f;
    std::vector<int> tagOrder;           // display position -> places.tags index
    struct TagTrack { float start = 0.0f; int durationMs = 150; std::chrono::steady_clock::time_point t0; };
    std::unordered_map<std::wstring, TagTrack> tagTracks; // label -> slide track (slot units)
    std::unordered_map<std::wstring, float> tagOffsets;   // label -> current offset (slot units)
    // Insertion indicator line at the tentative gap's top edge; chases the
    // target slot with the same 180ms easeOutCubic as the sibling slides.
    bool tagGapVisible = false;
    float tagGapLineY = 0.0f;            // current animated position (px)
    float tagGapFrom = 0.0f;
    float tagGapTo = 0.0f;
    std::chrono::steady_clock::time_point tagGapT0{};

    bool starDragPending = false;
    bool starDragActive = false;
    POINT starDragStartPt{};
    std::wstring starDragPath;
    size_t starDragTarget = 0;

    // Sidebar section header drag: reorders the sections. The dragged section
    // stays in place (drawn raised) while an insertion line shows the new slot.
    bool groupDragPending = false;
    bool groupDragActive = false;
    POINT groupDragStartPt{};
    int groupDragId = -1;                // SidebarSectionId being dragged
    int groupDragToIndex = -1;           // target slot in sidebarOrder
    float groupGapLineY = 0.0f;          // insertion indicator (px, 0 = hidden)
    bool groupGapVisible = false;
    // Set when the press came from a header-less section's own row (starred root,
    // OneDrive account): a plain click navigates there instead of folding.
    std::wstring groupDragPath;
    // The press came from a navigable header's title (#80): a plain click opens
    // groupDragPath even when it is empty (This PC).
    bool groupDragNavigate = false;

    // Quick-access pin drag: pinned folders reorder inside the section.
    bool pinDragPending = false;
    bool pinDragActive = false;
    POINT pinDragStartPt{};
    std::wstring pinDragPath;
    int pinDragRun = -1;                 // slot.run of the dragged row (highlight)
    int pinDragToIndex = -1;             // target position among the pins
    float pinGapLineY = 0.0f;
    bool pinGapVisible = false;

    // Staging tray card stack: per-card animation state keyed by item path.
    // TickTrayDeck advances it on the 16 ms UI timer; the renderer receives
    // the current pose through WindowViewModel::tray_deck (TrayStackGeometry
    // maps it to pixels for both drawing and hit testing).
    enum class TrayMotion : uint8_t { None, ThrowOut, ThrowBack, Spring };
    enum class TrayExit : uint8_t { Fade, Tumble, Return };
    struct TrayCardAnim {
        std::wstring name;      // cached so exiting ghosts can still draw
        std::wstring folder;
        DWORD attrs = 0;
        bool is_dir = false;
        bool missing = false;
        bool cut = false;
        uint64_t size = 0;
        bool ghost = false;     // left the stack window, playing its exit
        TrayExit exit = TrayExit::Fade;
        ULONGLONG exit_started = 0;
        ULONGLONG exit_delay = 0;   // ms, staggered clear
        float exit_opacity = 1.0f;
        float depth = 0.0f;     // eases toward the window index (0 = top)
        float hover = 0.0f;     // 0..1 top-card lift + close badge
        float appear = 1.0f;    // 0 = dropping in, eases to 1
        float opacity = 1.0f;
        float fly = 0.0f;       // card widths
        float dx = 0.0f;        // DIPs
        float dy = 0.0f;
        float angle = 0.0f;     // degrees
        float shrink = 1.0f;
        TrayMotion motion = TrayMotion::None;
        ULONGLONG motion_started = 0;
        float from_fly = 0.0f, from_dx = 0.0f, from_dy = 0.0f, from_angle = 0.0f;
        float to_fly = 0.0f, to_dy = 0.0f, to_angle = 0.0f;
    };
    struct TrayPuff {
        ULONGLONG start = 0;
        float angle = 0.0f;
        float dist = 0.0f;
        float size = 1.0f;
    };
    struct TrayExitHint {
        TrayExit exit = TrayExit::Tumble;
        ULONGLONG delay = 0;
    };
    // Press-and-hold on the top card: follow the pointer, fling to the back.
    struct TrayDrag {
        bool pending = false;   // button down on the top card, not moved yet
        bool active = false;    // moved past the slop: card follows the pointer
        std::wstring path;
        int x0 = 0, y0 = 0;
        int last_x = 0;
        ULONGLONG last_t = 0;
        float vx = 0.0f;        // DIP/ms, smoothed
        float dx = 0.0f;        // DIPs
        float dy = 0.0f;
    };
    std::unordered_map<std::wstring, TrayCardAnim> trayCards;
    std::vector<TrayPuff> trayPuffs;
    std::unordered_map<std::wstring, TrayExitHint> trayExitHints; // consumed by the next tick
    TrayDrag trayDrag;
    std::wstring trayRaisePath; // stepping back: this card drops in on top
    float trayOpen = 0.0f;      // eased drag-over highlight
    float traySpread = 0.0f;    // eased stack fan-out while hovered
    ULONGLONG trayLastTick = 0;
    int trayDeckOffset = 0;     // cyclic index of the top card (newest-first order)
    int trayWheelAccum = 0;     // sub-notch wheel delta accumulator
    size_t trayDeckLastTotal = 0; // collect detection (stack resets to newest)

    // Right details panel (view-menu toggle, session-persisted).
    bool showDetailsPanel = false;
    float detailsPanelWidth = 340.0f;
    bool detailsPanelResizing = false;
    float detailsScroll = 0.0f;
    bool detailsPreviewOnly = false;
    // Preview generation on/off: off skips the preview child process entirely,
    // so no decode work happens for the selection at all.
    bool detailsPreviewEnabled = true;
    bool detailsPreviewPanning = false;
    float detailsPreviewExpansion = 0.0f;
    float detailsPreviewExpansionFrom = 0.0f;
    ULONGLONG detailsPreviewFoldStart = 0;
    // Collapsible sections: bit 0基本信息 1属性 2标签 3安全 4其他; 安全/其他 default collapsed.
    uint32_t detailsCollapsedMask = (1u << 1) | (1u << 3) | (1u << 4);
    // Per-selection probe cache (file times + star state), keyed by path.
    std::wstring detailsSelPath;
    bool detailsSelValid = false;
    FILETIME detailsCreated{}, detailsModified{}, detailsAccessed{};
    bool detailsStarred = false;
    std::wstring detailsTypeName; // shell type name (SHGFI_TYPENAME)
    // Async security/volume meta for the 安全/其他 sections (WM_DETAILS_META).
    std::wstring detailsMetaPath;
    std::wstring detailsOwner, detailsPermissions;
    std::wstring detailsDrive, detailsFileSystem, detailsFreeSpace;
    // Folder size walk (background; cancelled and restarted on selection change).
    std::mutex detailsSizeMutex;
    std::condition_variable detailsSizeCv;
    std::thread detailsSizeThread;
    std::atomic<uint64_t> detailsSizeGeneration{0};
    bool detailsSizeStop = false;
    std::wstring detailsSizeRequested; // path being walked ("" = idle)
    struct DetailsSizeResult {
        std::wstring path;
        uint64_t size = 0, files = 0, dirs = 0;
        bool done = false;
    } detailsSize;

    // Rubber-band (marquee) selection in the file list.
    bool marqueePending = false;
    bool marqueeActive = false;
    bool marqueeAdditive = false;
    app::Pane* blankClickPane = nullptr;
    app::Tab* blankClickTab = nullptr;
    uint64_t blankClickGeneration = 0;
    app::Pane* blankDoublePane = nullptr;
    app::Tab* blankDoubleTab = nullptr;
    uint64_t blankDoubleGeneration = 0;
    DWORD blankDoubleTime = 0;
    POINT blankDoublePoint{};
    bool blankDoublePending = false;
    POINT marqueeStart{};
    POINT marqueeCur{};
    std::unordered_set<int> marqueeBase;
    // Tab scroll offset when the marquee started. marqueeStart is in that
    // moment's pane pixels, so scrolling carries the start with the content.
    float marqueeOriginScrollX = 0.0f;
    float marqueeOriginScrollY = 0.0f;
    // Edge auto-scroll is on. Frames follow the display clock (framePump);
    // kTimerMarqueeScroll only keeps them coming when the pump is unavailable.
    bool marqueeAutoScroll = false;
    std::chrono::steady_clock::time_point marqueeScrollLast{};
    // Pointer moved since the band was last applied; Render applies it once
    // per frame instead of once per mouse message.
    bool marqueeSelectionDirty = false;

    // Cut state mirrored into list rows (ui.md §5.2 rule 6: 55% opacity).
    std::vector<std::wstring> cutPaths;
    DWORD pendingCutClipboardSequence = 0;
    // Clipboard value CollectPathsToTray wrote for tray batch trayClipboardBatchId.
    DWORD trayClipboardSequence = 0;
    uint64_t trayClipboardBatchId = 0;
    std::vector<std::wstring> pendingCutClipboardPaths;
    std::vector<std::wstring> completedCutClipboardPaths;

    // After a "new folder/file" op lands, select it and start renaming.

};

// WM_SHELL_VERBS heap payload (posted by the registry enumeration thread).
struct ShellVerbsResult {
    std::wstring ext;
    std::vector<app::StaticVerb> verbs;
    uint32_t generation = 0;  // ContextMenuController::cache_generation() at request time
};

// WM_SHELL_VERB_SEED heap payload: the machine-wide verb cache, parsed on the
// seeding thread. Stale generations are dropped (a registry change re-seeds).
struct ShellVerbSeed {
    uint32_t generation = 0;
    std::unordered_map<std::wstring, std::vector<app::StaticVerb>> machine;
};

struct ShellCtxItemsPayload {
    std::vector<ops::ShellMenuItem> items;
    bool partial = false;
    std::vector<std::wstring> slow_clsids;
};


// WM_DETAILS_META heap payload (posted by the details-panel probe thread).
// File times + SHGFI_TYPENAME + security/volume must stay off the UI thread —
// BuildVm / DBLCLK hit-test used to block on SHGetFileInfo before OpenWith.
struct DetailsMetaResult {
    std::wstring path;
    bool attrs_valid = false;
    FILETIME created{};
    FILETIME modified{};
    FILETIME accessed{};
    std::wstring type_name;
    app::DetailsMeta meta;
};

inline AppState* GetAppState(HWND hwnd) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

inline std::wstring ClipboardPath(const std::wstring& p) {
    return path::StripExtendedPathPrefix(p);
}

} // namespace pulse
