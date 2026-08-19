// app_main.cpp — Pulse UI process entry point, window, input, shot mode.
#include "../ui/ui_compositor.h"
#include "../ui/ui_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_watch.h"
#include "../fs/fs_net_cache.h"
#include "app_model.h"
#include "app_worker.h"
#include "session.h"
#include "context_menu.h"
#include "shell_verbs.h"
#include "places.h"
#include "details_meta.h"
#include "context_menu_prefs.h"
#include "app_prefs.h"
#ifdef PULSE_WITH_SELFTEST
#include "selftest_1b2.h"
#endif
#include "resource.h"
#include "../index/index_client.h"
#include "../ops/ops_manager.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include "../common/text_format.h"
#include <windows.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <prsht.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <process.h>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "psapi.lib")

using namespace pulse;

static std::wstring ClipboardPath(const std::wstring& p);

constexpr UINT WM_WORKER_RESULT = WM_APP + 1;
constexpr UINT WM_OPS_NOTIFY = WM_APP + 2;
constexpr UINT WM_INDEX_NOTIFY = WM_APP + 40;
constexpr UINT WM_INDEX_SEARCH = WM_APP + 42;
constexpr UINT WM_NET_PROBE = WM_APP + 41;
constexpr UINT WM_TAG_ADS_WARNING = WM_APP + 43;
constexpr UINT WM_TAG_ADS_DISCOVERED = WM_APP + 44;
// Explorer context-menu integration (优化.md §7). lParam owns a heap payload.
constexpr UINT WM_SHELLCTX_ITEMS = WM_APP + 45;  // std::vector<ops::ShellMenuItem>*
constexpr UINT WM_SHELL_VERBS = WM_APP + 46;     // ShellVerbsResult*
constexpr UINT WM_DETAILS_META = WM_APP + 47;    // DetailsMetaResult*
constexpr UINT WM_TRAYICON = WM_APP + 50;
constexpr UINT kTimerUi = 1;

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
    std::chrono::steady_clock::time_point start;
};

struct Timing {
    double first_frame_ms = 0.0;
    double sort_done_ms = 0.0;
    double enum_ms = 0.0;
    double sort_ms = 0.0;
    bool first_frame_recorded = false;
};

struct AppState {
    HWND hwnd = nullptr;
    ui::Compositor compositor;
    ui::MainRenderer renderer;

    std::vector<std::unique_ptr<app::Pane>> panes;
    app::Pane* pane = nullptr;          // focused leaf (non-owning)
    app::Pane* targetPane = nullptr;    // Ctrl+D marked destination
    std::unique_ptr<app::SplitContainer> root;
    app::LayoutPreset layout = app::LayoutPreset::Single;

    fs::SnapshotStore store;
    app::WorkerPool worker;
    std::unique_ptr<fs::DirWatch> watcher;
    std::atomic<bool> watchDirty{false};
    std::atomic<ULONGLONG> watchLastChange{0};

    app::SidebarModel sidebar;
    uint32_t sidebarCollapsedMask = 0;
    app::StagingTray tray;
    app::PlacesCatalog places;
    app::ContextMenuPrefs ctxMenuPrefs;
    app::AppPrefs appPrefs;
    int settingsPage = 0;
    float settingsScroll = 0.0f;
    bool trayIconAdded = false;
    std::unordered_set<std::wstring> tagFallbackVolumes;
    std::unordered_set<std::wstring> tagAdsDiscoveryQueued;
    std::unordered_set<std::wstring> tagAdsDiscoveryChecked;
    const void* tagAdsLastSnapshot = nullptr;
    std::wstring tagAdsLastViewPath;
    std::wstring tagAdsLastFilter;
    int tagAdsLastFirstRow = -1;
    int tagAdsLastLastRow = -1;
    index::IndexClient index;
    std::vector<index::Hit> paletteHits;
    size_t paletteTotal = 0;
    std::wstring paletteQuery;
    std::wstring paletteIssuedNeedle;
    std::wstring paletteIssuedPrefix;
    bool paletteIssuedFolders = false;
    uint32_t paletteSearchId = 0;
    uint32_t nextIndexReq = 1;
    bool paletteSearching = false;
    bool probeBusy = false;
    std::wstring probeUnc;
    std::vector<std::wstring> probeQueue;

    bool darkMode = false;
    ui::ThemeMode themeOverride = ui::ThemeMode::Auto;
    D2D1_COLOR_F accentColor;
    float scale = 1.0f;
    bool maximized = false;
    bool backdropActive = true;

    bool showFps = true;
    double lastFrameMs = 0.0;
    double lastFps = 0.0;
    std::chrono::steady_clock::time_point lastFrameTime;
    double processCpuPercent = 0.0;
    double workingSetMb = 0.0;
    ULONGLONG processSampleTick = 0;
    uint64_t lastProcessTime100ns = 0;

    int hoverRow = -1;
    int hoverPaneIndex = -1;
    int dropPaneIndex = -1;
    bool scrollbarHovered = false;
    bool scrollbarDragging = false;
    bool scrollbarHorizontal = false;
    int scrollbarDragStartX = 0;
    int scrollbarDragStartY = 0;
    float scrollbarDragStartScroll = 0.0f;
    float scrollbarHoverWidth = 0.0f;

    bool splitterDragging = false;
    int splitterDragIndex = -1;
    D2D1_RECT_F splitterParentBounds{};
    app::SplitOrientation splitterOrientation = app::SplitOrientation::Vertical;
    std::vector<app::SplitContainer*> splitterNodes;

    bool columnResizing = false;
    int columnResizeIndex = -1;
    int columnResizePane = -1;

    bool tabDragPending = false;
    bool tabDragging = false;
    int tabDragIndex = -1;               // pane.tabs index of the dragged tab
    POINT tabDragStartPt{};
    int tabDragLastX = 0;
    float tabDragPressLeft = 0.0f;       // rest-slot left at press (for grab-offset follow)
    float tabDragFloatLeft = 0.0f;       // dragged tab left (px, cursor-delta driven)
    float tabFlowLeft = 0.0f;            // tab strip rest range / slot geometry (px)
    float tabFlowRight = 0.0f;
    float tabSlotW = 0.0f;
    float tabPitch = 0.0f;
    std::vector<int> tabOrder;           // display position -> pane.tabs index
    int tabDragRunPos = 0;               // run start within tabOrder (group drags)
    int tabDragRunLen = 1;               // >1: the whole group run moves as a block
    bool tabDragFromChip = false;        // drag started on the group chip
    int tabDragGroupId = 0;              // chip drag: app::TabGroup::id
    float tabDragSlots = 1.0f;           // visual width of the drag block in slots
    float tabDragBlockW = 0.0f;          // >0: collapsed chip drag block (chip+gap, px)
    struct TabTrack { float start = 0.0f; int durationMs = 150; std::chrono::steady_clock::time_point t0; };
    std::unordered_map<const app::Tab*, TabTrack> tabTracks;
    std::unordered_map<const app::Tab*, float> tabOffsets;
    // Chip slide channel (px, key = app::TabGroup::id) for collapsed-group
    // drags: chips are px-positioned, so slot-unit tab offsets can't move them.
    std::unordered_map<int, TabTrack> chipTracks;
    std::unordered_map<int, float> chipOffsets;

    // Smooth scroll animation.
    bool scrollAnimating = false;
    float scrollTargetY = 0.0f;
    std::chrono::steady_clock::time_point scrollLastUpdateTime;
    static constexpr double kScrollResponseMs = 48.0;

    ShotRequest shot;
    Timing timing;
    std::wstring session_path;
    std::vector<std::wstring> session_pane_paths;
    std::vector<ui::ViewMode> session_pane_views;
    std::vector<std::array<float, 3>> session_pane_columns;
    std::vector<app::PaneSessionSnapshot> session_pane_tabs;
    int session_layout = 0;
    int session_focused = 0;
    int session_target = -1;
    std::vector<std::wstring> recentPaths;
    std::unordered_map<std::wstring, std::wstring> gitRoots;

    // 1B-2 GUI verification: render the context menu to a PNG, no interaction.
    bool menushot = false;
    std::wstring menushot_out;
    // GUI verification for the tray fan deck: stage a few files pre-shot.
    bool shot_tray = false;
    int shot_tray_count = 4;
    // GUI verification for tab colors: three tabs, red/yellow/blue strips.
    bool shot_tab_colors = false;
    // GUI verification for the details panel: open it with a pre-selection.
    bool shot_details = false;
    bool shot_details_multi = false;
    float shot_scale_override = 0.0f;
    bool shot_high_contrast = false;

    std::mutex resultMutex;
    std::queue<app::WorkResult> results;

    HWND hwndAddressEdit = nullptr;
    bool addressEditing = false;
    bool addressIgnoreKillFocus = false;
    HWND hwndFilterEdit = nullptr;
    bool filterEditing = false;
    bool filterIgnoreKillFocus = false;
    bool filterFocusPending = false;

    // Stage 1B-1: ops layer (queue + undo), inline rename overlay.
    ops::OpsManager ops;
    std::wstring pending_undo_json; // loaded from session, imported in WM_CREATE
    uint64_t opsCompleted = 0;
    std::unique_ptr<ui::FileOperationWindow> operationWindow;
    uint64_t operationUiTaskId = 0;
    uint64_t operationDismissedTaskId = 0;
    uint64_t conflictUiToken = 0;
    bool operationAutoShown = false;
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

    // Drag-over feedback state (rendered via WindowViewModel).
    int dropRow = -1;
    int dropBreadcrumb = -1;
    int dropSidebar = -1;
    bool dropTray = false;
    std::wstring dropBadge;
    float dropBadgeX = 0.0f;
    float dropBadgeY = 0.0f;
    std::wstring dropDestDir;                 // resolved drop destination ("" = none/tray)

    // Spring-loaded folder enter during drag-over (ui.md §7.8).
    int springRow = -1;
    ULONGLONG springStart = 0;
    bool springEntered = false;

    // Drag-out bookkeeping.
    bool dragPending = false;
    POINT dragStartPt{};
    int clickCollapseIndex = -1;

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

    // Staging tray fan deck: eased per-card poses keyed by item path. The
    // tick below smooths them toward layout targets; the renderer receives
    // the current values through WindowViewModel::tray_deck.
    struct TrayCardAnim {
        std::wstring name;      // cached so exiting ghosts can still draw
        DWORD attrs = 0;
        bool is_dir = false;
        bool missing = false;
        bool ghost = false;     // removed from the tray, fading out
        float slot = 0.0f;      // fan slot (0 = center); eases toward layout
        float hover = 0.0f;     // 0..1 raise + straighten
        float appear = 0.0f;    // 0 = just collected, eases to 1
        float opacity = 1.0f;   // ghosts ease to 0, then the entry is dropped
    };
    std::unordered_map<std::wstring, TrayCardAnim> trayCards;
    float trayOpen = 0.0f;      // eased drag-over fan spread
    int trayDeckOffset = 0;     // window start into the newest-first item list
    int trayWheelAccum = 0;     // sub-notch wheel delta accumulator
    size_t trayDeckLastTotal = 0; // collect detection (window resets to newest)

    // Right details panel (view-menu toggle, session-persisted).
    bool showDetailsPanel = false;
    float detailsPanelWidth = 340.0f;
    bool detailsPanelResizing = false;
    float detailsScroll = 0.0f;
    float detailsPreviewScroll = 0.0f;
    // Cover-mode preview pan (DIPs) + drag state.
    float detailsPreviewPanX = 0.0f;
    float detailsPreviewPanY = 0.0f;
    bool detailsPreviewPanning = false;
    POINT detailsPreviewPanLast{};
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
    POINT marqueeStart{};
    POINT marqueeCur{};
    std::unordered_set<int> marqueeBase;

    // Cut state mirrored into list rows (ui.md §5.2 rule 6: 55% opacity).
    std::vector<std::wstring> cutPaths;

    // After a "new folder/file" op lands, select it and start renaming.
    std::wstring pendingRenameName;

    // Explorer context-menu session (优化.md §7): wait a beat for the COM
    // query (prefetched on WM_RBUTTONDOWN) so the first painted frame is the
    // merged menu; late items still append at the bottom.
    uint32_t ctxToken = 0;                        // active session (0 = none)
    std::vector<std::wstring> ctxPaths;           // parsing paths of the session
    bool ctxBackground = false;
    bool ctxMenuOpen = false;                     // right-click menu is tracking
    bool ctxComReady = false;                     // RSP_CTX_ITEMS arrived for ctxToken
    std::vector<ops::ShellMenuItem> ctxComItems;  // COM items for ctxToken
    std::vector<app::StaticVerb> ctxStaticVerbs;  // bound to CmdShellStaticBase+i
    std::wstring ctxStaticExt;                    // extension the statics are for
    std::vector<ui::FluentMenuItem> ctxBaseItems; // menu without the shell section
    std::unordered_map<std::wstring, std::vector<app::StaticVerb>> shellVerbCache;
    std::unordered_map<std::wstring, std::vector<ops::ShellMenuItem>> shellComCache;
    std::unordered_set<std::wstring> shellVerbPending;
    ULONGLONG ctxQueryAt = 0;                     // GetTickCount64 when the COM query started
    ULONGLONG shellRefreshAt = 0;                 // delayed list refresh after a shell verb
    ULONGLONG shellRefreshAgainAt = 0;
};

// WM_SHELL_VERBS heap payload (posted by the registry enumeration thread).
struct ShellVerbsResult {
    std::wstring ext;
    std::vector<app::StaticVerb> verbs;
};

// WM_DETAILS_META heap payload (posted by the security/volume fetch thread).
struct DetailsMetaResult {
    std::wstring path;
    app::DetailsMeta meta;
};

static AppState* GetAppState(HWND hwnd) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// Security/volume facts must never be fetched on the UI thread; results
// arrive as WM_DETAILS_META and are applied only if still current.
static void PrefetchDetailsMeta(HWND hwnd, const std::wstring& path) {
    std::thread([hwnd, path] {
        auto* result = new DetailsMetaResult{ path, app::FetchDetailsMeta(path) };
        if (!PostMessageW(hwnd, WM_DETAILS_META, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

static void NewTab(AppState& s, const std::wstring& path);
static void OpenSettingsTab(AppState& s, int page);
static void EnsureTrayIcon(AppState& s, bool show);
static bool IsSettingsTab(const app::Tab* tab);
static void ApplyAppWindowChrome(AppState& s);

static void PostWorkerResult(AppState& s, app::WorkResult res) {
    {
        std::lock_guard<std::mutex> lock(s.resultMutex);
        s.results.push(std::move(res));
    }
    PostMessageW(s.hwnd, WM_WORKER_RESULT, 0, 0);
}

static app::Tab* ActiveTab(AppState& s) {
    return s.pane ? s.pane->ActiveTab() : nullptr;
}

static D2D1_RECT_F FocusedPaneRect(const AppState& s) {
    D2D1_RECT_F content = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    if (!s.root || !s.pane) return content;
    std::vector<std::pair<app::Pane*, D2D1_RECT_F>> laid;
    app::LayoutSplitTree(*s.root, content, 4.0f * s.scale, laid);
    for (const auto& item : laid) {
        if (item.first == s.pane) return item.second;
    }
    return laid.empty() ? content : laid.front().second;
}

static app::Pane* PaneAtSlot(AppState& s, int index) {
    if (!s.root) return s.pane;
    std::vector<app::Pane*> visible;
    s.root->CollectPanes(visible);
    if (index >= 0 && index < static_cast<int>(visible.size())) return visible[static_cast<size_t>(index)];
    return s.pane;
}

// Defined further below; the details-panel fill in BuildVm needs it.
static std::wstring EntryFullPath(const app::Tab& tab, int index);

static int SlotIndexOf(const AppState& s, const app::Pane* pane) {
    if (!s.root || !pane) return -1;
    std::vector<app::Pane*> visible;
    s.root->CollectPanes(visible);
    for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
        if (visible[static_cast<size_t>(i)] == pane) return i;
    }
    return -1;
}

static D2D1_RECT_F ListRect(const AppState& s) {
    D2D1_RECT_F pane = FocusedPaneRect(s);
    float extra = 0.0f;
    const app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr;
    if (tab && !tab->banner_message.empty()) extra = 36.0f * s.scale;
    const ui::ViewMode mode = tab ? tab->view_mode : ui::ViewMode::Details;
    pane.top += s.renderer.PaneHeaderHeight() + extra +
                (ui::ShowsColumnHeader(mode) ? s.renderer.ColumnHeaderHeight() : 0.0f);
    return pane;
}

static void StartLoadingPath(AppState& s, app::Tab& tab, const std::wstring& path);
static void NavigateTo(AppState& s, const std::wstring& path);
static void FocusPane(AppState& s, app::Pane* p);
static void ApplyLayoutPreset(AppState& s, app::LayoutPreset preset);
static void HideFilterEditor(AppState& s, bool commit);
static void ShowFilterEditor(AppState& s);
enum class OmnibarMode { Path, Command, Project };
static void ShowOmnibar(AppState& s, OmnibarMode mode);
static void OpenWorkspace(AppState& s, int index);
static void LoadVirtualView(AppState& s, app::Tab& tab, const std::wstring& path);
static void ShowSplitDropdown(AppState& s);
static void ShowViewDropdown(AppState& s, int pane_index);
static void SetViewMode(AppState& s, ui::ViewMode mode);
static void TransferToTarget(AppState& s, bool move);
static void HideRenameOverlay(AppState& s, bool commit);
static void HideAddressEditor(AppState& s, bool navigate);
static void ShowAddressEditor(AppState& s);
static std::wstring FormatAddressPath(const std::wstring& text);
static std::vector<std::wstring> CollectPanePaths(const AppState& s);
static std::vector<ui::ViewMode> CollectPaneViews(const AppState& s);
static std::wstring PinCandidate(AppState& s);
static bool IsUncPath(const std::wstring& p);

static bool ScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
                              D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& maxScroll) {
    track = ListRect(s);
    const float viewH = std::max(0.0f, track.bottom - track.top);
    maxScroll = s.renderer.MaxScrollForPane(pane, FocusedPaneRect(s));
    if (maxScroll <= 0.0f || viewH <= 0.0f) return false;
    const float totalH = viewH + maxScroll;
    const float thumbH = std::max(24.0f * s.scale, viewH * (viewH / totalH));
    const float travel = std::max(1.0f, viewH - thumbH);
    const float thumbY = track.top + std::clamp(pane.scroll_y / maxScroll, 0.0f, 1.0f) * travel;
    track.left = track.right - 14.0f * s.scale;
    thumb = D2D1::RectF(track.left, thumbY, track.right, thumbY + thumbH);
    return true;
}

static bool HorizontalScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
                                         D2D1_RECT_F& track, D2D1_RECT_F& thumb,
                                         float& maxScroll) {
    track = ListRect(s);
    const float viewW = std::max(0.0f, track.right - track.left);
    maxScroll = s.renderer.MaxScrollXForPane(pane, FocusedPaneRect(s));
    if (maxScroll <= 0.0f || viewW <= 0.0f) return false;
    const float totalW = viewW + maxScroll;
    const float thumbW = std::max(28.0f * s.scale, viewW * (viewW / totalW));
    const float travel = std::max(1.0f, viewW - thumbW);
    const float thumbX = track.left + std::clamp(pane.scroll_x / maxScroll, 0.0f, 1.0f) * travel;
    track.top = track.bottom - 12.0f * s.scale;
    thumb = D2D1::RectF(thumbX, track.top, thumbX + thumbW, track.bottom);
    return true;
}

static void RememberPath(AppState& s, const std::wstring& path) {
    if (path.empty() || fs::IsVirtualPath(path)) return;
    std::wstring n = fs::NormalizePath(path);
    if (n.empty()) return;
    s.recentPaths.erase(std::remove(s.recentPaths.begin(), s.recentPaths.end(), n), s.recentPaths.end());
    s.recentPaths.insert(s.recentPaths.begin(), n);
    if (s.recentPaths.size() > 16) s.recentPaths.resize(16);
    s.places.RecordVisit(n);
}

static void FillPaneSlots(AppState& s, ui::WindowViewModel& vm) {
    D2D1_RECT_F content = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    std::vector<std::pair<app::Pane*, D2D1_RECT_F>> laid;
    std::vector<app::SplitterLayout> splitters;
    const float gap = 8.0f * s.scale;
    if (s.root) app::LayoutSplitTree(*s.root, content, gap, laid, &splitters);
    vm.pane_slots.clear();
    vm.splitters.clear();
    s.splitterNodes.clear();
    vm.filter_editing = s.filterEditing;
    if (app::Tab* tab = ActiveTab(s)) {
        std::wstring kind, rest;
        if (app::ParsePulsePath(tab->current_path, &kind, &rest) && kind == L"settings") {
            vm.settings_open = true;
            vm.settings_page = (rest == L"context") ? 1 : 0;
            vm.settings_scroll = s.settingsScroll;
            vm.settings_launch_on_startup = s.appPrefs.launch_on_startup;
            vm.settings_keep_running = s.appPrefs.keep_running_on_close;
            vm.settings_row_height = s.appPrefs.row_height;
            static constexpr ipc::CtxMenuGroup kGroups[] = {
                ipc::CtxMenuGroup::Software, ipc::CtxMenuGroup::OpenWith,
                ipc::CtxMenuGroup::Share, ipc::CtxMenuGroup::System, ipc::CtxMenuGroup::Print
            };
            for (int g = 0; g < 5; ++g)
                vm.settings_group_on[g] = s.ctxMenuPrefs.GroupEnabled(kGroups[g]);
            vm.settings_items.clear();
            vm.settings_items.reserve(s.ctxMenuPrefs.seen.size());
            for (const auto& seen : s.ctxMenuPrefs.seen) {
                ui::SettingsRowView row;
                row.key = seen.key;
                row.text = seen.text;
                row.group = static_cast<int>(ipc::GroupOf(seen.category));
                row.on = s.ctxMenuPrefs.ItemEnabled(seen.key, seen.category, seen.from_com);
                vm.settings_items.push_back(std::move(row));
            }
            if (vm.status.status_text.empty()) vm.status.status_text = L"设置";
        }
    }
    for (const auto& sp : splitters) {
        ui::SplitterView view;
        view.hit_rect = sp.hit_rect;
        view.parent_bounds = sp.parent_bounds;
        view.vertical = (sp.orientation == app::SplitOrientation::Vertical);
        vm.splitters.push_back(view);
        s.splitterNodes.push_back(sp.node);
    }
    for (size_t i = 0; i < laid.size(); ++i) {
        ui::PaneSlotView slot;
        slot.rect = laid[i].second;
        app::Pane* p = laid[i].first;
        slot.focused = (p == s.pane);
        slot.target = (p && p == s.targetPane);
        if (p) {
            // BuildWindowViewModel already populated the focused pane. Reuse
            // it instead of rebuilding all snapshot-derived data twice.
            if (slot.focused) slot.pane = vm.pane;
            else app::FillPaneViewModel(slot.pane, *p, &s.places);
            if (app::Tab* tab = p->ActiveTab()) {
                for (const auto& cutPath : s.cutPaths) {
                    if (fs::ParentPath(cutPath) != tab->current_path) continue;
                    const size_t slash = cutPath.find_last_of(L"\\/");
                    slot.pane.cut_names.insert(slash == std::wstring::npos
                        ? cutPath : cutPath.substr(slash + 1));
                }
            }
            if (static_cast<int>(i) == s.hoverPaneIndex) slot.pane.hover_index = s.hoverRow;
            if (static_cast<int>(i) == s.dropPaneIndex) slot.pane.drop_target_index = s.dropRow;
            if (slot.focused) {
                slot.pane.rename_index = s.renameIndex;
                if (s.marqueeActive) {
                    slot.pane.marquee_active = true;
                    slot.pane.marquee_rect = D2D1::RectF(
                        static_cast<float>(std::min(s.marqueeStart.x, s.marqueeCur.x)),
                        static_cast<float>(std::min(s.marqueeStart.y, s.marqueeCur.y)),
                        static_cast<float>(std::max(s.marqueeStart.x, s.marqueeCur.x)),
                        static_cast<float>(std::max(s.marqueeStart.y, s.marqueeCur.y)));
                }
            }
        }
        vm.pane_slots.push_back(std::move(slot));
    }
}

static bool UpdateSplitterDrag(AppState& s, int mx, int my) {
    if (!s.splitterDragging) return false;
    if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
        s.splitterDragging = false;
        s.splitterDragIndex = -1;
        return true;
    }
    if (s.splitterDragIndex < 0 ||
        s.splitterDragIndex >= static_cast<int>(s.splitterNodes.size())) {
        return true;
    }
    app::SplitContainer* node = s.splitterNodes[static_cast<size_t>(s.splitterDragIndex)];
    if (!node) return true;
    app::ApplySplitRatio(*node, s.splitterParentBounds, 8.0f * s.scale,
                         static_cast<float>(mx), static_cast<float>(my));
    s.hoverRegion = static_cast<int>(ui::HitTestResult::Splitter);
    s.hoverControlIndex = s.splitterDragIndex;
    return true;
}

// ---------------------------------------------------------------------------
// Staging tray fan deck: display entries (newest batch first) + eased poses.
// ---------------------------------------------------------------------------
static constexpr size_t kTrayDeckCap = 5;

struct TrayDeckEntry {
    int batch = -1;
    int sub = -1;
    const app::TrayItem* item = nullptr;
};

static std::vector<TrayDeckEntry> TrayDeckEntries(const app::StagingTray& tray, size_t offset,
                                                  size_t cap = kTrayDeckCap) {
    std::vector<TrayDeckEntry> out;
    const auto& batches = tray.batches();
    size_t skipped = 0;
    for (int b = static_cast<int>(batches.size()) - 1; b >= 0 && out.size() < cap; --b) {
        const auto& items = batches[static_cast<size_t>(b)].items;
        for (int k = 0; k < static_cast<int>(items.size()) && out.size() < cap; ++k) {
            if (skipped < offset) { ++skipped; continue; }
            out.push_back({ b, k, &items[static_cast<size_t>(k)] });
        }
    }
    return out;
}

static int TrayItemTotalCount(const app::StagingTray& tray) {
    int n = 0;
    for (const auto& b : tray.batches()) n += static_cast<int>(b.items.size());
    return n;
}

// Extended-length prefixes leak into tooltips otherwise: \\?\C:\x -> C:\x.
static std::wstring TrayDisplayPath(const std::wstring& path) {
    if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\" + path.substr(7);
    if (path.compare(0, 4, L"\\\\?\\") == 0) return path.substr(4);
    return path;
}

static std::wstring TrayItemName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Display index of the tray card under the cursor. Hovering a card's × badge
// (TrayItemRemove) still counts as hovering that card, so the raise pose and
// the badge stay put instead of oscillating.
static int TrayDeckHoverIndex(const AppState& s) {
    if (s.hoverRegion == static_cast<int>(ui::HitTestResult::TrayCard))
        return s.hoverControlIndex;
    if (s.hoverRegion == static_cast<int>(ui::HitTestResult::TrayItemRemove)) {
        const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset));
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            if (entries[static_cast<size_t>(i)].batch == s.hoverControlIndex &&
                entries[static_cast<size_t>(i)].sub == s.hoverSubIndex)
                return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Details panel helpers: star shortcuts, byte/time formatting, size walk.
// ---------------------------------------------------------------------------
// "星标常用文件" is a path index in places.json, not copies or .lnk shortcuts.
static void RefreshStarredViews(AppState& s) {
    for (auto& pane : s.panes) {
        if (!pane) continue;
        for (auto& owned : pane->tabs) {
            app::Tab* tab = owned.get();
            if (!tab) continue;
            std::wstring kind;
            if (app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"starred")
                LoadVirtualView(s, *tab, tab->current_path);
        }
    }
}

static bool ToggleStarred(AppState& s, const std::wstring& target) {
    if (target.empty() || fs::IsVirtualPath(target)) return false;
    const bool on = s.places.ToggleStarred(target);
    s.detailsStarred = on;
    RefreshStarredViews(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return on;
}

static void StopDetailsSizeWalk(AppState& s) {
    s.detailsSizeGeneration.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        s.detailsSizeRequested.clear();
        s.detailsSize = AppState::DetailsSizeResult{};
    }
    s.detailsSizeCv.notify_one();
}

static void StartDetailsSizeWalk(AppState& s, const std::wstring& path) {
    const uint64_t generation =
        s.detailsSizeGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        s.detailsSize = AppState::DetailsSizeResult{};
        s.detailsSize.path = path;
        s.detailsSizeRequested = path;
    }
    if (!s.detailsSizeThread.joinable()) {
        AppState* sp = &s;
        s.detailsSizeThread = std::thread([sp] {
            uint64_t processed = 0;
            for (;;) {
                std::wstring request;
                uint64_t requestGeneration = 0;
                {
                    std::unique_lock<std::mutex> lock(sp->detailsSizeMutex);
                    sp->detailsSizeCv.wait(lock, [&] {
                        return sp->detailsSizeStop ||
                            (!sp->detailsSizeRequested.empty() &&
                             sp->detailsSizeGeneration.load(std::memory_order_relaxed) != processed);
                    });
                    if (sp->detailsSizeStop) return;
                    request = sp->detailsSizeRequested;
                    requestGeneration =
                        sp->detailsSizeGeneration.load(std::memory_order_relaxed);
                    processed = requestGeneration;
                }
                uint64_t size = 0, files = 0, dirs = 0;
                std::vector<std::wstring> stack{request};
                while (!stack.empty() &&
                       sp->detailsSizeGeneration.load(std::memory_order_relaxed) ==
                           requestGeneration) {
                    std::wstring dir = std::move(stack.back());
                    stack.pop_back();
                    WIN32_FIND_DATAW fd{};
                    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                                FindExSearchNameMatch, nullptr,
                                                FIND_FIRST_EX_LARGE_FETCH);
                    if (h == INVALID_HANDLE_VALUE) continue;
                    do {
                        if (sp->detailsSizeGeneration.load(std::memory_order_relaxed) !=
                            requestGeneration) break;
                        if (fd.cFileName[0] == L'.' &&
                            (fd.cFileName[1] == L'\0' ||
                             (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                            continue;
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            ++dirs;
                            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
                                stack.push_back(dir + L"\\" + fd.cFileName);
                        } else {
                            ++files;
                            size += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) |
                                    fd.nFileSizeLow;
                        }
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
                if (sp->detailsSizeGeneration.load(std::memory_order_relaxed) !=
                    requestGeneration) continue;
                {
                    std::lock_guard<std::mutex> lock(sp->detailsSizeMutex);
                    if (sp->detailsSizeRequested != request) continue;
                    sp->detailsSize = {request, size, files, dirs, true};
                }
                if (sp->hwnd) InvalidateRect(sp->hwnd, nullptr, FALSE);
            }
        });
    }
    (void)generation;
    s.detailsSizeCv.notify_one();
}

static void ShutdownDetailsSizeWalk(AppState& s) {
    s.detailsSizeGeneration.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        s.detailsSizeStop = true;
        s.detailsSizeRequested.clear();
    }
    s.detailsSizeCv.notify_one();
    if (s.detailsSizeThread.joinable()) s.detailsSizeThread.join();
}

static std::wstring DetailsAttributeText(DWORD attrs) {
    struct AttributeName { DWORD bit; const wchar_t* name; };
    constexpr AttributeName values[] = {
        { FILE_ATTRIBUTE_READONLY, L"只读" },
        { FILE_ATTRIBUTE_HIDDEN, L"隐藏" },
        { FILE_ATTRIBUTE_SYSTEM, L"系统" },
        { FILE_ATTRIBUTE_COMPRESSED, L"压缩" },
        { FILE_ATTRIBUTE_ENCRYPTED, L"加密" },
        { FILE_ATTRIBUTE_REPARSE_POINT, L"重解析点" },
    };
    std::wstring result;
    for (const auto& value : values) {
        if (!(attrs & value.bit)) continue;
        if (!result.empty()) result += L"、";
        result += value.name;
    }
    return result.empty() ? L"普通" : result;
}

// Exponential smoothing toward layout targets; runs on the 16 ms UI timer.
// Returns true while anything is still moving (caller invalidates).
static bool TickTrayDeck(AppState& s) {
    bool dirty = false;
    auto ease = [&dirty](float& cur, float target, float k) {
        const float next = cur + (target - cur) * k;
        if (std::abs(next - cur) > 0.0015f) { cur = next; dirty = true; }
        else if (cur != target) { cur = target; dirty = true; }
    };

    ease(s.trayOpen, s.dropTray ? 1.0f : 0.0f, 0.20f);

    // Window into the newest-first list; a fresh collect always jumps back
    // to the newest items.
    const int total = TrayItemTotalCount(s.tray);
    const int max_offset = std::max(0, total - static_cast<int>(kTrayDeckCap));
    const int clamped_offset = std::clamp(s.trayDeckOffset, 0, max_offset);
    if (clamped_offset != s.trayDeckOffset) { s.trayDeckOffset = clamped_offset; dirty = true; }
    const bool grew = total > static_cast<int>(s.trayDeckLastTotal);
    if (grew && s.trayDeckOffset != 0) { s.trayDeckOffset = 0; dirty = true; }
    s.trayDeckLastTotal = static_cast<size_t>(total);

    const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset));
    const int n = static_cast<int>(entries.size());
    const int hovered = TrayDeckHoverIndex(s);

    std::unordered_set<std::wstring> live;
    live.reserve(entries.size() * 2);
    for (int i = 0; i < n; ++i) {
        const app::TrayItem& item = *entries[static_cast<size_t>(i)].item;
        live.insert(item.path);
        const float target_slot = static_cast<float>(i) - (n - 1) * 0.5f;
        auto [it, inserted] = s.trayCards.try_emplace(item.path);
        AppState::TrayCardAnim& a = it->second;
        if (inserted) {
            // Collected items fan out from the center; items revealed by
            // wheel-scrolling fade in directly at their slot.
            a.slot = grew ? 0.0f : target_slot;
        }
        if (a.name.empty()) a.name = TrayItemName(item.path);
        a.attrs = item.attrs;
        a.is_dir = item.is_dir;
        a.missing = !item.exists;
        a.ghost = false;
        ease(a.slot, target_slot, 0.22f);
        ease(a.appear, 1.0f, 0.16f);
        ease(a.hover, i == hovered ? 1.0f : 0.0f, 0.28f);
        ease(a.opacity, 1.0f, 0.25f);
    }
    // Removed/scrolled-out items become ghosts: frozen slot, fade + sink,
    // then dropped. Cap ghosts so fast wheeling can't pile them up.
    size_t ghosts = 0;
    for (auto it = s.trayCards.begin(); it != s.trayCards.end();) {
        if (live.count(it->first)) { ++it; continue; }
        AppState::TrayCardAnim& a = it->second;
        if (!a.ghost) { a.ghost = true; a.hover = 0.0f; dirty = true; }
        ease(a.opacity, 0.0f, 0.22f);
        ++ghosts;
        if (a.opacity <= 0.02f || ghosts > 6) { it = s.trayCards.erase(it); dirty = true; }
        else ++it;
    }
    return dirty;
}

// View-model wrapper: builds the base VM and layers ops-layer status on top.
static ui::WindowViewModel BuildVm(AppState& s) {
    if (!s.pane) return {};
    ui::WindowViewModel vm = app::BuildWindowViewModel(*s.pane, s.sidebar,
        s.pane->focused, s.maximized, s.darkMode, &s.places, s.sidebarCollapsedMask);
    ops::OpStatus st = s.ops.Status();
    if (st.active || !st.last_error.empty() || !st.summary.empty()) {
        vm.status.task_text = st.last_error.empty() ? st.summary
            : st.summary + L" — " + st.last_error;
        vm.status.task_progress = st.active ? st.percent : -1.0f;
    }
    {
        std::wstring idx = s.index.Status();
        app::Tab* active = ActiveTab(s);
        std::wstring virtual_kind;
        std::wstring virtual_rest;
        const bool search_visible = active &&
            app::ParsePulsePath(active->current_path, &virtual_kind, &virtual_rest) &&
            virtual_kind == L"search";
        if (!idx.empty() && (s.paletteSearching || search_visible)) {
            if (!vm.status.status_text.empty()) vm.status.status_text += L"  ·  ";
            vm.status.status_text += idx;
        }
    }
    if (s.showFps) {
        wchar_t perf[256];
        swprintf_s(perf,
            L"DEV  %.2f ms  %.0f FPS  \u00B7  \u679A\u4E3E %.1f ms  \u00B7  \u6392\u5E8F %.1f ms  \u00B7  CPU %.1f%%  \u00B7  %.1f MB",
            s.lastFrameMs, s.lastFps, s.timing.enum_ms, s.timing.sort_ms,
            s.processCpuPercent, s.workingSetMb);
        vm.status.performance_text = perf;
        wchar_t compactPerf[96];
        swprintf_s(compactPerf, L"DEV  %.1f ms  \u00B7  %.0f FPS  \u00B7  %.0f MB",
            s.lastFrameMs, s.lastFps, s.workingSetMb);
        vm.status.performance_compact_text = compactPerf;
    }
    // 1B-2 overlays: cut rows, drag feedback, breadcrumb hover.
    if (!s.cutPaths.empty()) {
        app::Tab* tab = ActiveTab(s);
        if (tab && !tab->current_path.empty()) {
            for (const auto& cutPath : s.cutPaths) {
                if (fs::ParentPath(cutPath) != tab->current_path) continue;
                const size_t slash = cutPath.find_last_of(L"\\/");
                vm.pane.cut_names.insert(slash == std::wstring::npos
                    ? cutPath : cutPath.substr(slash + 1));
            }
        }
    }
    vm.pane.drop_target_index = s.dropRow;
    vm.pane.rename_index = s.renameIndex;
    vm.address_editing = s.addressEditing;
    vm.splitter_pressed = s.splitterDragging;
    vm.details_resize_pressed = s.detailsPanelResizing;
    if (s.marqueeActive) {
        vm.pane.marquee_active = true;
        vm.pane.marquee_rect = D2D1::RectF(
            static_cast<float>(std::min(s.marqueeStart.x, s.marqueeCur.x)),
            static_cast<float>(std::min(s.marqueeStart.y, s.marqueeCur.y)),
            static_cast<float>(std::max(s.marqueeStart.x, s.marqueeCur.x)),
            static_cast<float>(std::max(s.marqueeStart.y, s.marqueeCur.y)));
    }
    vm.breadcrumb_hover = s.breadcrumbHover;
    vm.breadcrumb_drop = s.dropBreadcrumb;
    vm.sidebar_drop_index = s.dropSidebar;
    // Sidebar tag reorder: emit the tags group in the tentative drag order and
    // feed each tag its current slide offset (slot units).
    int tagsGroup = -1;
    for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
        if (!vm.sidebar[g].items.empty() && vm.sidebar[g].items[0].is_tag) { tagsGroup = g; break; }
    }
    if (tagsGroup >= 0) {
        auto& items = vm.sidebar[tagsGroup].items;
        if (!s.tagRenameId.empty()) {
            const std::wstring editing_path = app::MakeTagPath(s.tagRenameId);
            for (auto& item : items) item.editing = item.path == editing_path;
        }
        if (s.tagDragActive && s.tagOrder.size() == items.size()) {
            std::vector<ui::SidebarItem> reordered;
            reordered.reserve(items.size());
            for (int idx : s.tagOrder) reordered.push_back(std::move(items[idx]));
            items = std::move(reordered);
            for (int pos = 0; pos < static_cast<int>(s.tagOrder.size()); ++pos) {
                if (s.tagOrder[pos] == s.tagDragTag) { vm.tag_drag_item = pos; break; }
            }
            vm.tag_drag_group = tagsGroup;
            vm.tag_drag_y = s.tagDragY;
        }
        for (auto& it : items) {
            const auto off = s.tagOffsets.find(it.label);
            if (off != s.tagOffsets.end()) it.y_offset = off->second;
        }
    }
    // Title-bar tabs: emit the tentative drag order and slide offsets.
    if (s.pane && !vm.tabs.empty()) {
        const bool dragging = s.tabDragging && s.tabOrder.size() == vm.tabs.size();
        if (dragging) {
            std::vector<ui::TabView> reordered;
            reordered.reserve(vm.tabs.size());
            for (int idx : s.tabOrder) {
                if (idx >= 0 && idx < static_cast<int>(vm.tabs.size()))
                    reordered.push_back(std::move(vm.tabs[static_cast<size_t>(idx)]));
            }
            if (reordered.size() == vm.tabs.size()) vm.tabs = std::move(reordered);
            for (int pos = 0; pos < static_cast<int>(s.tabOrder.size()); ++pos) {
                if (s.tabOrder[pos] == s.tabDragIndex) { vm.tab_drag_index = pos; break; }
            }
            vm.tab_drag_x = s.tabDragFloatLeft;
            vm.tab_drag_count = s.tabDragRunLen;
            vm.tab_drag_chip = s.tabDragFromChip;
            if (s.tabDragRunLen > 1) vm.tab_drag_index = s.tabDragRunPos;
            const int origActive = static_cast<int>(s.pane->active_tab);
            for (int pos = 0; pos < static_cast<int>(s.tabOrder.size()); ++pos) {
                const bool isActive = s.tabOrder[pos] == origActive;
                vm.tabs[static_cast<size_t>(pos)].active = isActive;
                if (isActive) vm.active_tab = pos;
            }
        }
        for (int pos = 0; pos < static_cast<int>(vm.tabs.size()); ++pos) {
            const int orig = dragging ? s.tabOrder[pos] : pos;
            if (orig < 0 || orig >= static_cast<int>(s.pane->tabs.size())) continue;
            const app::Tab* key = s.pane->tabs[static_cast<size_t>(orig)].get();
            const auto off = s.tabOffsets.find(key);
            if (off != s.tabOffsets.end())
                vm.tabs[static_cast<size_t>(pos)].x_offset = off->second;
        }
        for (size_t gi = 0; gi < vm.tab_groups.size(); ++gi) {
            const auto off = s.chipOffsets.find(vm.tab_groups[gi].id);
            if (off != s.chipOffsets.end()) vm.tab_groups[gi].x_offset = off->second;
        }
    }
    // Staging tray fan deck: live cards (newest batch first) + exiting ghosts.
    {
        const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset));
        ui::TrayDeckView& deck = vm.tray_deck;
        deck.open = s.trayOpen;
        deck.offset = s.trayDeckOffset;
        deck.total_count = TrayItemTotalCount(s.tray);
        deck.batch_count = static_cast<int>(s.tray.batches().size());
        uint64_t total_size = 0;
        for (const auto& b : s.tray.batches()) total_size += b.total_size;
        deck.total_size = total_size;
        deck.live_count = static_cast<int>(entries.size());
        const int hover_index = TrayDeckHoverIndex(s);
        deck.hovered = hover_index >= 0 && hover_index < deck.live_count ? hover_index : -1;
        for (int i = 0; i < deck.live_count; ++i) {
            const app::TrayItem& item = *entries[static_cast<size_t>(i)].item;
            ui::TrayCardView card;
            card.path = item.path;
            card.batch = entries[static_cast<size_t>(i)].batch;
            card.sub = entries[static_cast<size_t>(i)].sub;
            if (card.batch >= 0 && card.batch < static_cast<int>(s.tray.batches().size()))
                card.batch_total_size = s.tray.batches()[static_cast<size_t>(card.batch)].total_size;
            card.is_dir = item.is_dir;
            card.attrs = item.attrs;
            card.missing = !item.exists;
            const auto found = s.trayCards.find(item.path);
            if (found != s.trayCards.end()) {
                card.name = found->second.name;
                card.slot = found->second.slot;
                card.hover = found->second.hover;
                card.appear = found->second.appear;
                card.opacity = found->second.opacity;
            } else {
                card.name = TrayItemName(item.path);
            }
            deck.cards.push_back(std::move(card));
        }
        for (const auto& [key, anim] : s.trayCards) {
            if (!anim.ghost) continue;
            ui::TrayCardView card;
            card.path = key;
            card.name = anim.name;
            card.is_dir = anim.is_dir;
            card.attrs = anim.attrs;
            card.missing = anim.missing;
            card.ghost = true;
            card.slot = anim.slot;
            card.appear = anim.appear;
            card.opacity = anim.opacity;
            deck.cards.push_back(std::move(card));
        }
    }
    // Right details panel: selection info + size walk + tag chips.
    vm.details_visible = s.showDetailsPanel;
    std::wstring sizeTarget; // folder that should be walking ("" = none)
    if (s.showDetailsPanel) {
        ui::DetailsPanelView& dv = vm.details;
        dv.scroll_y = s.detailsScroll;
        dv.preview_scroll_y = s.detailsPreviewScroll;
        dv.preview_pan_x = s.detailsPreviewPanX;
        dv.preview_pan_y = s.detailsPreviewPanY;
        dv.collapsed_mask = s.detailsCollapsedMask;
        app::Tab* tab = ActiveTab(s);
        const int selCount = tab ? tab->SelectedCount() : 0;
        if (tab && selCount >= 1) {
            dv.has_selection = true;
            dv.multi_count = selCount;
        }
        if (dv.has_selection && selCount == 1 && tab->snapshot &&
            tab->selected_index >= 0 &&
            tab->selected_index < static_cast<int>(tab->snapshot->size())) {
            const fs::DirEntry& e = (*tab->snapshot)[static_cast<size_t>(tab->selected_index)];
            const bool penetrated = !e.link_target.empty();
            dv.name = penetrated ? fs::StripLnkSuffix(e.name) : e.name;
            dv.is_dir = penetrated ? e.link_target_is_dir : e.is_dir;
            dv.attrs = e.attrs;
            if (penetrated && e.link_target_is_dir) dv.attrs |= FILE_ATTRIBUTE_DIRECTORY;
            dv.size_value = penetrated ? e.link_target_size : e.size;
            const FILETIME& shown_mtime = penetrated ? e.link_target_mtime : e.mtime;
            dv.modified_value = (static_cast<uint64_t>(shown_mtime.dwHighDateTime) << 32) |
                                shown_mtime.dwLowDateTime;
            dv.view_generation = tab->view_generation;
            dv.path = penetrated ? e.link_target
                                 : EntryFullPath(*tab, tab->selected_index);
        }
        if (dv.has_selection && selCount > 1 && tab->snapshot) {
            uint64_t knownSize = 0;
            int files = 0, folders = 0;
            for (int index : tab->SelectedIndices()) {
                if (index < 0 || index >= static_cast<int>(tab->snapshot->size())) continue;
                const fs::DirEntry& entry = (*tab->snapshot)[static_cast<size_t>(index)];
                if (entry.is_dir) ++folders;
                else { ++files; knownSize += entry.size; }
            }
            wchar_t composition[96]{};
            if (files && folders)
                swprintf_s(composition, L"%d 个文件，%d 个文件夹", files, folders);
            else if (files)
                swprintf_s(composition, L"%d 个文件", files);
            else
                swprintf_s(composition, L"%d 个文件夹", folders);
            dv.type_text = composition;
            dv.size_text = pulse::format::ByteSize(knownSize, true);
            dv.location_text = fs::IsVirtualPath(tab->current_path)
                ? L"多个位置" : TrayDisplayPath(tab->current_path);
        }
        if (dv.has_selection && dv.multi_count == 1 && !dv.path.empty()) {
            if (s.detailsSelPath != dv.path) {
                s.detailsSelPath = dv.path;
                s.detailsScroll = 0.0f;
                s.detailsPreviewScroll = 0.0f;
                s.detailsPreviewPanX = 0.0f;
                s.detailsPreviewPanY = 0.0f;
                dv.scroll_y = 0.0f;
                dv.preview_scroll_y = 0.0f;
                dv.preview_pan_x = 0.0f;
                dv.preview_pan_y = 0.0f;
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                s.detailsSelValid =
                    GetFileAttributesExW(dv.path.c_str(), GetFileExInfoStandard, &fad) != 0;
                if (s.detailsSelValid) {
                    s.detailsCreated = fad.ftCreationTime;
                    s.detailsModified = fad.ftLastWriteTime;
                    s.detailsAccessed = fad.ftLastAccessTime;
                }
                // Shell type name ("MP4 视频文件"); user-paced one-shot call.
                s.detailsTypeName.clear();
                const std::wstring shell_path = ClipboardPath(dv.path);
                SHFILEINFOW sfi{};
                if (SHGetFileInfoW(shell_path.c_str(), 0, &sfi, sizeof(sfi),
                                   SHGFI_TYPENAME) && sfi.szTypeName[0])
                    s.detailsTypeName = sfi.szTypeName;
                // 安全/其他 sections: clear stale meta and refetch off-thread.
                s.detailsMetaPath.clear();
                s.detailsOwner.clear();
                s.detailsPermissions.clear();
                s.detailsDrive.clear();
                s.detailsFileSystem.clear();
                s.detailsFreeSpace.clear();
                if (s.detailsSelValid && !fs::IsVirtualPath(dv.path))
                    PrefetchDetailsMeta(s.hwnd, dv.path);
            }
            s.detailsStarred = s.places.IsStarred(dv.path);
            dv.starred = s.detailsStarred;
            if (s.detailsSelValid) {
                dv.created_text = pulse::format::LocalFileTime(s.detailsCreated);
                dv.modified_text = pulse::format::LocalFileTime(s.detailsModified);
                dv.accessed_text = pulse::format::LocalFileTime(s.detailsAccessed);
            }
            dv.location_text = TrayDisplayPath(fs::ParentPath(dv.path));
            dv.attributes_text = DetailsAttributeText(dv.attrs);
            dv.type_text = s.detailsTypeName;
            if (!dv.is_dir) {
                dv.subtitle_text = s.detailsTypeName;
                if (!dv.subtitle_text.empty()) dv.subtitle_text += L" · ";
                dv.subtitle_text += pulse::format::ByteSize(dv.size_value, true);
                dv.size_text = pulse::format::ByteSize(dv.size_value, true) + L" (" +
                               pulse::format::GroupedInt(dv.size_value) +
                               L" \u5B57\u8282)";
            }
            if (s.detailsMetaPath == dv.path) {
                dv.owner_text = s.detailsOwner;
                dv.permissions_text = s.detailsPermissions;
                dv.drive_text = s.detailsDrive;
                dv.fs_text = s.detailsFileSystem;
                dv.free_space_text = s.detailsFreeSpace;
            }
            s.renderer.CachedPreviewProperties(dv.path, dv.modified_value, dv.size_value,
                                               dv.preview_properties);
            dv.preset_tags.clear();
            for (int idx = 0; idx < static_cast<int>(s.places.tags.size()); ++idx) {
                ui::DetailsPanelView::TagChip chip;
                chip.name = s.places.tags[static_cast<size_t>(idx)].name;
                chip.color = ui::HexColor(s.places.tags[static_cast<size_t>(idx)].rgb);
                chip.tag_index = idx;
                chip.assigned = s.places.PathHasTag(dv.path, idx);
                dv.preset_tags.push_back(std::move(chip));
            }
            if (dv.is_dir) sizeTarget = dv.path;
        }
    }
    std::wstring requestedSizePath;
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        requestedSizePath = s.detailsSizeRequested;
    }
    if (!sizeTarget.empty()) {
        if (requestedSizePath != sizeTarget) StartDetailsSizeWalk(s, sizeTarget);
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        if (s.detailsSize.done && s.detailsSize.path == sizeTarget) {
            vm.details.size_text = pulse::format::ByteSize(s.detailsSize.size, true);
            wchar_t contains[96];
            swprintf_s(contains, L"%llu 个文件，%llu 个文件夹",
                       s.detailsSize.files, s.detailsSize.dirs);
            vm.details.contains_text = contains;
        } else {
            vm.details.size_pending = true;
        }
    } else if (!requestedSizePath.empty()) {
        StopDetailsSizeWalk(s);
    }
    vm.tray_drop = s.dropTray;
    vm.drag_badge = s.dropBadge;
    vm.drag_badge_x = s.dropBadgeX;
    vm.drag_badge_y = s.dropBadgeY;
    vm.hover_region = s.hoverRegion;
    vm.hover_control_index = s.hoverControlIndex;
    vm.hover_pane_index = s.hoverPaneIndex;
    vm.column_resize_pressed = s.columnResizing;
    vm.tooltip_text = s.tooltipText;
    vm.tooltip_x = static_cast<float>(s.hoverPoint.x);
    vm.tooltip_y = static_cast<float>(s.hoverPoint.y);
    FillPaneSlots(s, vm);
    vm.window_effect = ui::WindowEffectFromId(s.appPrefs.window_effect);
    vm.background_image = s.appPrefs.background_image;
    return vm;
}

static std::wstring TooltipForHover(AppState& s) {
    using R = ui::HitTestResult;
    switch (static_cast<R::Region>(s.hoverRegion)) {
    case R::TabClose: return L"关闭标签页";
    case R::Tab: {
        if (!s.pane) return L"";
        int i = s.hoverControlIndex;
        if (s.tabDragging && i >= 0 && i < static_cast<int>(s.tabOrder.size()))
            i = s.tabOrder[static_cast<size_t>(i)];
        if (i < 0 || i >= static_cast<int>(s.pane->tabs.size()) ||
            !s.pane->tabs[static_cast<size_t>(i)])
            return L"";
        const app::Tab& tab = *s.pane->tabs[static_cast<size_t>(i)];
        if (!tab.virtual_title.empty()) return tab.virtual_title;
        return TrayDisplayPath(tab.current_path);
    }
    case R::TabNew: return L"新建标签页";
    case R::ThemeToggle: return L"切换主题";
    case R::SettingsButton: return L"设置";
    case R::SettingsEffect: {
        if (s.hoverControlIndex >= 0 && s.hoverControlIndex < ui::kWindowEffectCount)
            return std::wstring(L"窗口效果：") + ui::WindowEffectLabel(
                static_cast<ui::WindowEffect>(s.hoverControlIndex));
        return L"窗口效果";
    }
    case R::SettingsWallpaper:
        return s.hoverControlIndex == 1 ? L"清除背景图" : L"选择背景图";
    case R::SettingsDensity: return L"列表行高";
    case R::Minimize: return L"最小化";
    case R::Maximize: return s.maximized ? L"还原" : L"最大化";
    case R::Close: return L"关闭";
    case R::NavBack: return L"后退";
    case R::NavForward: return L"前进";
    case R::NavUp: return L"向上一级";
    case R::NavRefresh: return L"刷新";
    case R::NewButton: return L"新建";
    case R::Cut: return L"剪切 (Ctrl+X)";
    case R::Copy: return L"复制 (Ctrl+C)";
    case R::Paste: return L"粘贴 (Ctrl+V)";
    case R::Rename: return L"重命名 (F2)";
    case R::Delete: return L"删除 (Del)";
    case R::SplitButton: return L"分栏布局";
    case R::DetailsToggle: return s.showDetailsPanel ? L"收起详细信息" : L"展开详细信息";
    case R::PaneMediumIcons: return L"中图标";
    case R::PaneViewButton: return L"查看";
    case R::FilterBox: return L"过滤当前文件夹 (Ctrl+F)";
    case R::Splitter: return L"拖动调整分栏大小";
    case R::DetailsOpen: return L"打开";
    case R::DetailsStar: return L"收藏";
    case R::DetailsMore: return L"更多操作";
    case R::DetailsRename: return L"重命名";
    case R::DetailsTagAdd: return L"添加标签";
    case R::DetailsResize: return L"拖动调整详情栏宽度";
    case R::DetailsNewTab: return L"在新标签打开";
    case R::DetailsCopyPath: return L"复制路径";
    case R::DetailsPreview: return L"拖动平移预览";
    case R::DetailsSection: return L"展开/折叠";
    case R::DetailsAttrToggle:
        switch (s.hoverControlIndex) {
        case 0: return L"只读";
        case 1: return L"隐藏";
        case 2: return L"系统属性";
        default: return L"";
        }
    case R::DetailsSecurityChange: return L"系统属性";
    case R::RowStar: return L"星标";
    case R::RowNewTab: return L"在新标签打开";
    case R::RowMore: return L"更多操作";
    case R::SidebarItemAction: return L"取消钉住";
    case R::Row: {
        app::Pane* pane = PaneAtSlot(s, s.hoverPaneIndex);
        app::Tab* tab = pane ? pane->ActiveTab() : ActiveTab(s);
        if (tab && tab->snapshot && s.hoverControlIndex >= 0 &&
            s.hoverControlIndex < static_cast<int>(tab->snapshot->size())) {
            const auto& entry = (*tab->snapshot)[s.hoverControlIndex];
            std::wstring tooltip = entry.name;
            std::wstring full = entry.full_path;
            if (full.empty() && !fs::IsVirtualPath(tab->current_path)) {
                full = tab->current_path;
                if (!full.empty() && !full.ends_with(L"\\")) full += L"\\";
                full += entry.name;
            }
            if (const auto* indices = s.places.TagIndicesForPath(full); indices && !indices->empty()) {
                tooltip += L"\n标签：";
                bool first = true;
                for (int index : *indices) {
                    if (index < 0 || index >= static_cast<int>(s.places.tags.size())) continue;
                    if (!first) tooltip += L"、";
                    tooltip += s.places.tags[static_cast<size_t>(index)].name;
                    first = false;
                }
            }
            return tooltip;
        }
        return L"";
    }
    case R::TrayCard: {
        const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset));
        if (s.hoverControlIndex >= 0 &&
            s.hoverControlIndex < static_cast<int>(entries.size()))
            return TrayDisplayPath(entries[static_cast<size_t>(s.hoverControlIndex)].item->path);
        return L"";
    }
    default: return L"";
    }
}

// Full path of a directory entry (normalized, empty when out of range).
static std::wstring EntryFullPath(const app::Tab& tab, int index) {
    if (!tab.snapshot || index < 0 || index >= static_cast<int>(tab.snapshot->size())) return L"";
    const fs::DirEntry& e = (*tab.snapshot)[static_cast<size_t>(index)];
    if (!e.full_path.empty()) return e.full_path;
    if (fs::IsVirtualPath(tab.current_path)) return L"";
    std::wstring full = tab.current_path;
    if (!full.ends_with(L"\\")) full += L"\\";
    full += e.name;
    return full;
}

static std::vector<std::wstring> SelectedFullPaths(const app::Tab& tab) {
    std::vector<std::wstring> out;
    if (!tab.snapshot) return out;
    const auto indices = tab.SelectedIndices();
    out.reserve(indices.size());
    for (int index : indices) {
        std::wstring full = EntryFullPath(tab, index);
        if (!full.empty()) out.push_back(std::move(full));
    }
    return out;
}

static std::wstring TagDiscoveryKey(std::wstring path) {
    path = fs::NormalizePath(std::move(path));
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path;
}

static void QueueVisibleTagDiscovery(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->loading || !tab->snapshot || tab->snapshot->empty()) return;

    const D2D1_RECT_F list = ListRect(s);
    const float row_height = s.renderer.RowHeight();
    if (row_height <= 0.0f || list.bottom <= list.top) return;
    const int first = std::max(0, static_cast<int>(std::floor(tab->scroll_y / row_height)) - 1);
    const int visible_count = static_cast<int>(std::ceil((list.bottom - list.top) / row_height)) + 2;
    const int last = first + visible_count;
    if (s.tagAdsLastSnapshot == tab->snapshot.get() &&
        s.tagAdsLastViewPath == tab->current_path &&
        s.tagAdsLastFilter == tab->filter_text &&
        s.tagAdsLastFirstRow == first && s.tagAdsLastLastRow == last) {
        return;
    }
    s.tagAdsLastSnapshot = tab->snapshot.get();
    s.tagAdsLastViewPath = tab->current_path;
    s.tagAdsLastFilter = tab->filter_text;
    s.tagAdsLastFirstRow = first;
    s.tagAdsLastLastRow = last;

    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    std::vector<std::wstring> paths;
    const int end = std::min(last, static_cast<int>(pane.EntryCount()) - 1);
    for (int view_row = first; view_row <= end; ++view_row) {
        const int source = pane.SourceIndex(view_row);
        const std::wstring full = EntryFullPath(*tab, source);
        if (full.empty() || fs::IsVirtualPath(full) || s.places.TagIndicesForPath(full)) continue;
        const std::wstring key = TagDiscoveryKey(full);
        if (key.empty() || s.tagAdsDiscoveryChecked.contains(key) ||
            !s.tagAdsDiscoveryQueued.insert(key).second) {
            continue;
        }
        paths.push_back(full);
    }
    if (paths.empty()) return;

    const HWND notify = s.hwnd;
    s.worker.EnqueueIo([paths = std::move(paths), notify] {
        auto discoveries = std::make_unique<std::vector<TagAdsDiscovery>>();
        discoveries->reserve(paths.size());
        for (const auto& path : paths) {
            TagAdsDiscovery discovery;
            discovery.path = path;
            discovery.records = app::ReadTagAdsV2(path);
            if (discovery.records.empty()) discovery.legacy_names = app::ReadTagAds(path);
            discoveries->push_back(std::move(discovery));
        }
        if (notify && PostMessageW(notify, WM_TAG_ADS_DISCOVERED, 0,
                                   reinterpret_cast<LPARAM>(discoveries.get()))) {
            discoveries.release();
        }
    });
}

// Full path of the focused selected entry (normalized, empty when nothing selected).
static std::wstring SelectedFullPath(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return L"";
    return EntryFullPath(*tab, tab->selected_index);
}

// Plain path for clipboard interop (strip \\?\ / \\?\UNC\ prefixes).
static std::wstring ClipboardPath(const std::wstring& p) {
    if (p.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + p.substr(8);
    if (p.starts_with(L"\\\\?\\")) return p.substr(4);
    return p;
}

static bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request) {
    // Copy/move conflict discovery is part of the transfer worker's recursive
    // scan. The UI only consumes immutable conflict snapshots.
    s.ops.Submit(std::move(request));
    return true;
}

// Release one tray batch into the current folder through the ops layer.
static void ReleaseTrayBatch(AppState& s, size_t idx) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || idx >= s.tray.batches().size()) return;
    if (fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    const app::TrayBatch& b = s.tray.batches()[idx];
    ops::OpRequest req;
    req.type = b.move_intent ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = tab->current_path;
    for (const auto& it : b.items) {
        if (it.exists) req.sources.push_back(it.path);
    }
    if (req.sources.empty()) return;
    if (!SubmitWithConflictResolution(s, std::move(req))) return;
    // Cut batches are consumed by release; copy batches too (default per ui.md §7.4).
    s.tray.RemoveBatch(idx);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Ctrl+V: release newest tray batch, else paste from the system clipboard.
static void PasteIntoCurrent(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    if (!s.tray.batches().empty()) {
        size_t idx = s.tray.batches().size() - 1;
        if (s.tray.batches()[idx].move_intent) s.cutPaths.clear();
        ReleaseTrayBatch(s, idx);
        return;
    }
    ops::ClipboardData cb;
    if (ops::ReadClipboard(cb)) {
        ops::OpRequest req;
        req.type = cb.cut ? ops::OpType::Move : ops::OpType::Copy;
        req.dest_dir = tab->current_path;
        for (auto& p : cb.paths) req.sources.push_back(fs::NormalizePath(p));
        if (SubmitWithConflictResolution(s, std::move(req)) && cb.cut) s.cutPaths.clear();
        // TODO(1B-2): after a cut-paste completes, clear the source clipboard
        // like Explorer does (OLE_ISCUTCLEAR bookkeeping).
    }
}

// Forward declarations for the 1B-2 wiring below (definitions live further down).
static void CancelScrollAnimation(AppState& s);
static void RefreshActiveTab(AppState& s);
static void NavigateTo(AppState& s, const std::wstring& path);
static void GoBack(AppState& s);
static void OpenSelected(AppState& s);
static void CollectToTray(AppState& s, bool move_intent);
static void ShowRenameOverlay(AppState& s);
static void HideRenameOverlay(AppState& s, bool commit);
static void LayoutRenameOverlay(AppState& s);
static void ShowTagRenameOverlay(AppState& s, const app::TagId& tag_id);
static void HideTagRenameOverlay(AppState& s, bool commit);
static void LayoutTagRenameOverlay(AppState& s);
static void MaybePrefetchSearchPage(AppState& s);
static void ShowTagPicker(AppState& s, POINT screen_pt);

static void DeleteSelected(AppState& s, bool permanent) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty()) return;
    if (permanent) {
        std::wstring prompt;
        if (paths.size() == 1) {
            prompt = L"永久删除（不进回收站）：\n" + ClipboardPath(paths[0]) + L"\n\n确定吗？";
        } else {
            wchar_t buf[96];
            swprintf_s(buf, L"永久删除（不进回收站）%d 项？\n\n确定吗？", static_cast<int>(paths.size()));
            prompt = buf;
        }
        if (MessageBoxW(s.hwnd, prompt.c_str(), L"Pulse", MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
    }
    ops::OpRequest req;
    req.type = permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

// ---------------------------------------------------------------------------
// Stage 1B-2: built-in Fluent context menu + new-item dropdown.
// ---------------------------------------------------------------------------
static bool EnsureMenu(AppState& s) {
    if (!s.menu) {
        s.menu = std::make_unique<ui::FluentMenu>();
        if (!s.menu->Create(s.hwnd, &s.compositor, s.scale)) {
            s.menu.reset();
            return false;
        }
    }
    s.menu->SetTheme(s.darkMode, s.accentColor);
    return true;
}

static void CopySelectedPath(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty()) return;
    std::wstring text;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i) text += L"\r\n";
        text += ClipboardPath(paths[i]);
    }
    ops::WriteClipboardText(text);
}

// Creates "新建文件夹"/"新建文本文档.txt" via the ops layer, then (on the next
// snapshot) selects it and enters the rename overlay.
static void CreateNewItem(AppState& s, bool folder) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    std::wstring name = folder
        ? app::UniqueChildName(tab->current_path, L"新建文件夹", L"")
        : app::UniqueChildName(tab->current_path, L"新建文本文档", L".txt");
    std::wstring full = tab->current_path;
    if (!full.ends_with(L"\\")) full += L"\\";
    full += name;
    ops::OpRequest req;
    req.type = folder ? ops::OpType::CreateFolder : ops::OpType::CreateTextFile;
    req.sources.push_back(full);
    s.pendingRenameName = name;
    s.ops.Submit(std::move(req));
}

static void QueueTagAds(AppState& s, std::vector<app::TagAdsUpdate> updates) {
    const HWND notify = s.hwnd;
    s.worker.EnqueueIo([updates = std::move(updates), notify] {
        auto failed = std::make_unique<std::vector<std::wstring>>();
        for (const auto& update : updates) {
            if (app::WriteTagAdsV2(update.path, update.tags)) continue;
            wchar_t volume[MAX_PATH]{};
            if (GetVolumePathNameW(update.path.c_str(), volume, ARRAYSIZE(volume)))
                failed->push_back(volume);
            else if (fs::IsUncPath(update.path))
                failed->push_back(L"网络位置");
            else
                failed->push_back(L"此位置");
        }
        if (!failed->empty() && notify)
            PostMessageW(notify, WM_TAG_ADS_WARNING, 0,
                         reinterpret_cast<LPARAM>(failed.release()));
    });
}

static std::vector<app::TagAdsUpdate> BuildTagAdsUpdates(
        const app::PlacesCatalog& places, const std::vector<std::wstring>& paths,
        bool include_descendants = false) {
    std::vector<app::TagAdsUpdate> updates;
    std::unordered_set<std::wstring> seen;
    std::vector<std::wstring> candidates = paths;
    if (include_descendants) {
        auto below = [](const std::wstring& candidate, const std::wstring& root) {
            if (_wcsicmp(candidate.c_str(), root.c_str()) == 0) return true;
            if (candidate.size() <= root.size() ||
                _wcsnicmp(candidate.c_str(), root.c_str(), root.size()) != 0) return false;
            return root.ends_with(L"\\") || candidate[root.size()] == L'\\' ||
                   candidate[root.size()] == L'/';
        };
        for (const auto& tag : places.tags) {
            for (const auto& assigned : tag.paths) {
                if (std::any_of(paths.begin(), paths.end(),
                                [&](const std::wstring& root) { return below(assigned, root); })) {
                    candidates.push_back(assigned);
                }
            }
        }
    }
    updates.reserve(candidates.size());
    for (const auto& path : candidates) {
        const std::wstring key = TagDiscoveryKey(path);
        if (key.empty() || !seen.insert(key).second) continue;
        app::TagAdsUpdate update;
        update.path = path;
        for (int index : places.TagsForPath(path)) {
            if (index < 0 || index >= static_cast<int>(places.tags.size())) continue;
            const auto& tag = places.tags[static_cast<size_t>(index)];
            update.tag_names.push_back(tag.name);
            update.tags.push_back({ tag.id, tag.name, tag.rgb });
        }
        updates.push_back(std::move(update));
    }
    return updates;
}

static bool ToggleTagForSelection(AppState& s, const app::TagId& tag_id,
                                  const std::vector<std::wstring>& paths) {
    if (tag_id.empty() || paths.empty()) return false;
    const bool add = s.places.GetSelectionState(tag_id, paths) != app::TagSelectionState::All;
    std::vector<app::TagAdsUpdate> updates;
    if (!s.places.SetTagsBatch(tag_id, paths, add, &updates)) return false;
    QueueTagAds(s, std::move(updates));
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return true;
}

static void ShowTagPicker(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    const std::vector<std::wstring> paths = ActiveTab(s)
        ? SelectedFullPaths(*ActiveTab(s)) : std::vector<std::wstring>{};
    constexpr int kTagPickerBase = 20000;
    constexpr int kCreateTag = 29999;
    auto rebuild = [&s, &paths](const std::wstring& query) {
        std::vector<ui::FluentMenuItem> items;
        std::wstring needle = query;
        for (auto& c : needle) c = static_cast<wchar_t>(std::towlower(c));
        bool exact = false;
        for (int i = 0; i < static_cast<int>(s.places.tags.size()); ++i) {
            const auto& tag = s.places.tags[static_cast<size_t>(i)];
            std::wstring lower = tag.name;
            for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
            if (!needle.empty() && lower.find(needle) == std::wstring::npos) continue;
            exact = exact || (!needle.empty() && lower == needle);
            ui::FluentMenuItem item;
            item.command = kTagPickerBase + i;
            item.text = tag.name;
            item.has_swatch = true;
            item.swatch_color = ui::HexColor(tag.rgb);
            const auto state = s.places.GetSelectionState(tag.id, paths);
            item.checked = state == app::TagSelectionState::All;
            item.mixed = state == app::TagSelectionState::Mixed;
            items.push_back(std::move(item));
        }
        if (!needle.empty() && !exact) {
            ui::FluentMenuItem create;
            create.command = kCreateTag;
            create.text = L"创建标签“" + query + L"”";
            create.glyph = L"\xE710";
            if (!items.empty()) items.back().separator_after = true;
            items.push_back(std::move(create));
        }
        return items;
    };
    for (;;) {
        s.menu->SetFilterPlaceholder(L"搜索或新建标签…");
        int command = s.menu->TrackPopup(screen_pt, rebuild(L""), rebuild);
        const std::wstring query = s.menu->LastFilterQuery();
        if (command == app::CmdNone) {
            // Enter with no highlighted row commits the typed name: toggle an
            // exact match, otherwise fall through to the create branch.
            if (!s.menu->LastFilterCommitted() || query.empty()) break;
            command = kCreateTag;
            std::wstring lower = query;
            for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
            for (int i = 0; i < static_cast<int>(s.places.tags.size()); ++i) {
                std::wstring name = s.places.tags[static_cast<size_t>(i)].name;
                for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
                if (name == lower) { command = kTagPickerBase + i; break; }
            }
        }

        app::TagId tag_id;
        if (command == kCreateTag) {
            static constexpr uint32_t kFinderColors[] = {
                0xEF4444, 0xF59E0B, 0xEAB308, 0x22C55E, 0x3B82F6, 0xA855F7, 0x94A3B8
            };
            tag_id = s.places.CreateTag(query,
                kFinderColors[s.places.tags.size() % ARRAYSIZE(kFinderColors)]);
        } else if (command >= kTagPickerBase &&
                   command < kTagPickerBase + static_cast<int>(s.places.tags.size())) {
            tag_id = s.places.tags[static_cast<size_t>(command - kTagPickerBase)].id;
        }
        if (!tag_id.empty()) ToggleTagForSelection(s, tag_id, paths);
    }
    s.menu->SetFilterPlaceholder(L"搜索命令、文件夹…");
}

static void ShowTagSidebarMenu(AppState& s, const app::TagId& tag_id, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    const app::ColorTag* tag = s.places.FindTag(tag_id);
    if (!tag) return;
    constexpr int kRename = 31000;
    constexpr int kColorBase = 31100;
    constexpr int kDelete = 31200;
    static constexpr uint32_t kFinderColors[] = {
        0xEF4444, 0xF59E0B, 0xEAB308, 0x22C55E, 0x3B82F6, 0xA855F7, 0x94A3B8
    };
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem rename;
    rename.command = kRename;
    rename.text = L"重命名标签…";
    rename.glyph = L"\xE8AC";
    items.push_back(std::move(rename));
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(kFinderColors)); ++i) {
        ui::FluentMenuItem color;
        color.command = kColorBase + i;
        color.text = i == 0 ? L"红色" : i == 1 ? L"橙色" : i == 2 ? L"黄色"
                   : i == 3 ? L"绿色" : i == 4 ? L"蓝色" : i == 5 ? L"紫色" : L"灰色";
        color.has_swatch = true;
        color.swatch_color = ui::HexColor(kFinderColors[i]);
        color.checked = tag->rgb == kFinderColors[i];
        items.push_back(std::move(color));
    }
    items.back().separator_after = true;
    ui::FluentMenuItem remove;
    remove.command = kDelete;
    remove.text = L"删除标签";
    remove.glyph = L"\xE74D";
    items.push_back(std::move(remove));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kRename) {
        constexpr int kApplyRename = 31300;
        auto rebuild = [](const std::wstring& query) {
            ui::FluentMenuItem item;
            item.command = kApplyRename;
            item.text = query.empty() ? L"输入新名称" : L"重命名为“" + query + L"”";
            item.glyph = L"\xE8AC";
            item.enabled = !query.empty();
            return std::vector<ui::FluentMenuItem>{ std::move(item) };
        };
        s.menu->SetFilterPlaceholder(tag->name);
        const int apply = s.menu->TrackPopup(screen_pt, rebuild(L""), rebuild);
        const std::wstring name = s.menu->LastFilterQuery();
        s.menu->SetFilterPlaceholder(L"搜索命令、文件夹…");
        // Enter without highlighting the row also confirms the typed name.
        if (apply == kApplyRename ||
            (apply == app::CmdNone && s.menu->LastFilterCommitted())) {
            const std::vector<std::wstring> affected = s.places.PathsForTag(tag_id);
            if (s.places.RenameTag(tag_id, name))
                QueueTagAds(s, BuildTagAdsUpdates(s.places, affected));
        }
    } else if (command >= kColorBase && command < kColorBase + 7) {
        const std::vector<std::wstring> affected = s.places.PathsForTag(tag_id);
        if (s.places.SetTagColor(tag_id, kFinderColors[command - kColorBase]))
            QueueTagAds(s, BuildTagAdsUpdates(s.places, affected));
    } else if (command == kDelete) {
        const size_t count = s.places.PathsForTag(tag_id).size();
        const std::wstring message = count == 0
            ? L"删除此标签？"
            : L"此标签已用于 " + std::to_wstring(count) + L" 个项目。删除后将移除这些关联。";
        if (MessageBoxW(s.hwnd, message.c_str(), L"删除标签",
                        MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
            std::vector<app::TagAdsUpdate> updates;
            s.places.DeleteTag(tag_id, &updates);
            QueueTagAds(s, std::move(updates));
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void DispatchMenuCommand(AppState& s, int cmd) {
    if (cmd >= app::CmdViewBase && cmd < app::CmdViewBase + 8) {
        SetViewMode(s, ui::ViewModeFromIndex(cmd - app::CmdViewBase));
        return;
    }
    switch (cmd) {
    case app::CmdOpen: OpenSelected(s); break;
    case app::CmdCut: CollectToTray(s, true); break;
    case app::CmdCopy: CollectToTray(s, false); break;
    case app::CmdPaste: PasteIntoCurrent(s); break;
    case app::CmdDelete: DeleteSelected(s, (GetKeyState(VK_SHIFT) & 0x8000) != 0); break;
    case app::CmdRename: ShowRenameOverlay(s); break;
    case app::CmdProperties: {
        // Shell verb on the ops pool (plan §6.2); compile-verified in 1B-2.
        std::wstring full = SelectedFullPath(s);
        if (!full.empty()) s.ops.ShowProperties(ClipboardPath(full));
        break;
    }
    case app::CmdOpenTerminal: {
        app::Tab* tab = ActiveTab(s);
        if (tab && !tab->current_path.empty()) s.ops.OpenTerminal(ClipboardPath(tab->current_path));
        break;
    }
    case app::CmdCopyPath: {
        // Item menu copies the selection; background menu copies the folder.
        app::Tab* tab = ActiveTab(s);
        if (tab && tab->SelectedCount() > 0) {
            CopySelectedPath(s);
            break;
        }
        std::wstring full = tab ? tab->current_path : L"";
        if (!full.empty()) ops::WriteClipboardText(ClipboardPath(full));
        break;
    }
    case app::CmdUndo:
        if (s.ops.CanUndo()) s.ops.Undo();
        break;
    case app::CmdNewFolder: CreateNewItem(s, true); break;
    case app::CmdNewTextFile: CreateNewItem(s, false); break;
    case app::CmdTags: {
        POINT point{};
        GetCursorPos(&point);
        ShowTagPicker(s, point);
        break;
    }
    case app::CmdDetailsPanel:
        s.showDetailsPanel = !s.showDetailsPanel;
        s.renderer.SetDetailsPanelVisible(s.showDetailsPanel);
        break;
    case app::CmdLayoutSingle: ApplyLayoutPreset(s, app::LayoutPreset::Single); break;
    case app::CmdLayoutTwoVertical: ApplyLayoutPreset(s, app::LayoutPreset::TwoVertical); break;
    case app::CmdLayoutTwoHorizontal: ApplyLayoutPreset(s, app::LayoutPreset::TwoHorizontal); break;
    case app::CmdLayoutThree: ApplyLayoutPreset(s, app::LayoutPreset::Three); break;
    case app::CmdLayoutFourGrid: ApplyLayoutPreset(s, app::LayoutPreset::FourGrid); break;
    case app::CmdCopyToTarget: TransferToTarget(s, false); break;
    case app::CmdMoveToTarget: TransferToTarget(s, true); break;
    case app::CmdPinWorkspace: {
        std::wstring root = PinCandidate(s);
        if (!root.empty() && !fs::IsVirtualPath(root)) {
            if (s.places.FindWorkspace(root) >= 0) {
                s.places.UnpinWorkspace(root);
            } else {
                s.places.PinWorkspace(root, L"", static_cast<int>(s.layout), CollectPanePaths(s),
                                      CollectPaneViews(s));
            }
        }
        break;
    }
    case app::CmdPinNetwork: {
        std::wstring root = PinCandidate(s);
        if (root.empty()) {
            app::Tab* tab = ActiveTab(s);
            if (tab) root = tab->current_path;
        }
        if (IsUncPath(root)) s.places.PinNetwork(root, L"");
        break;
    }
    case app::CmdSearchAll: {
        const auto q = app::ParseOmnibarQuery(s.paletteQuery, false);
        if (!q.needle.empty())
            NavigateTo(s, app::MakeSearchPath(q.needle));
        break;
    }
    case app::CmdInstallFullIndex:
        s.index.RequestInstallService();
        break;
    case app::CmdSettings:
        OpenSettingsTab(s, 0);
        break;
    case app::CmdSettingsContextMenu:
        OpenSettingsTab(s, 1);
        break;
    default:
        if (cmd >= app::CmdIndexBase) {
            const int idx = cmd - app::CmdIndexBase;
            if (idx >= 0 && idx < static_cast<int>(s.paletteHits.size())) {
                const auto& hit = s.paletteHits[static_cast<size_t>(idx)];
                if (hit.is_dir) NavigateTo(s, hit.path);
                else s.ops.OpenWith(hit.path);
            }
        } else if (cmd >= app::CmdRecentBase) {
            const int idx = cmd - app::CmdRecentBase;
            if (idx >= 0 && idx < static_cast<int>(s.recentPaths.size()))
                NavigateTo(s, s.recentPaths[static_cast<size_t>(idx)]);
        }
        break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static bool ClipboardHasFiles() {
    return IsClipboardFormatAvailable(CF_HDROP) != 0;
}

static void ApplyWorkspacePinLabel(std::vector<ui::FluentMenuItem>& items, AppState& s) {
    const std::wstring root = PinCandidate(s);
    const bool pinned = !root.empty() && !fs::IsVirtualPath(root) &&
        s.places.FindWorkspace(root) >= 0;
    for (auto& item : items) {
        if (item.command != app::CmdPinWorkspace) continue;
        item.text = pinned ? L"取消工作区" : L"钉为工作区";
        break;
    }
}

static std::vector<ui::FluentMenuItem> BuildFinderItemMenu(
        AppState& s, bool can_undo, const std::wstring& undo_label) {
    std::vector<ui::FluentMenuItem> items = app::BuildItemMenu(can_undo, undo_label);
    ApplyWorkspacePinLabel(items, s);
    const std::vector<std::wstring> paths = ActiveTab(s)
        ? SelectedFullPaths(*ActiveTab(s)) : std::vector<std::wstring>{};
    const int quick_count = std::min(7, static_cast<int>(s.places.tags.size()));
    ui::FluentMenuItem quick_tags;
    quick_tags.command = app::CmdTags;
    quick_tags.quick_swatches.reserve(static_cast<size_t>(quick_count));
    for (int i = 0; i < quick_count; ++i) {
        const auto& tag = s.places.tags[static_cast<size_t>(i)];
        ui::FluentMenuSwatch swatch;
        swatch.command = app::CmdTagBase + i;
        swatch.color = ui::HexColor(tag.rgb);
        const auto state = s.places.GetSelectionState(tag.id, paths);
        swatch.checked = state == app::TagSelectionState::All;
        swatch.mixed = state == app::TagSelectionState::Mixed;
        quick_tags.quick_swatches.push_back(std::move(swatch));
    }
    const auto picker = std::find_if(items.begin(), items.end(), [](const ui::FluentMenuItem& item) {
        return item.command == app::CmdTags;
    });
    if (!quick_tags.quick_swatches.empty()) items.insert(picker, std::move(quick_tags));
    return items;
}

// ---------------------------------------------------------------------------
// Explorer context-menu session (优化.md §7).
// ---------------------------------------------------------------------------

// Lowercased common extension of the selection; "" for folders / mixed types.
static std::wstring CommonExtension(const app::Tab& tab,
                                    const std::vector<int>& indices) {
    if (!tab.snapshot) return L"";
    std::wstring ext;
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(tab.snapshot->size())) return L"";
        const fs::DirEntry& e = (*tab.snapshot)[static_cast<size_t>(index)];
        if (e.is_dir) return L"";
        const auto pos = e.name.find_last_of(L'.');
        if (pos == std::wstring::npos || pos == 0 || pos + 1 >= e.name.size()) return L"";
        std::wstring one = e.name.substr(pos);
        for (auto& c : one) c = static_cast<wchar_t>(std::towlower(c));
        if (ext.empty()) ext = std::move(one);
        else if (ext != one) return L"";
    }
    return ext;
}

// Registry static verbs are read off the UI thread and cached per extension;
// the reader posts WM_SHELL_VERBS back when done.
static void PrefetchStaticVerbs(AppState& s, const std::wstring& ext) {
    if (ext.empty() || s.shellVerbCache.count(ext) || s.shellVerbPending.count(ext))
        return;
    s.shellVerbPending.insert(ext);
    HWND hwnd = s.hwnd;
    std::thread([hwnd, ext] {
        auto* result = new ShellVerbsResult{ ext, app::EnumerateStaticVerbs(ext) };
        if (!PostMessageW(hwnd, WM_SHELL_VERBS, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
}

// Ends the current session: tells pulse_shell to drop its IContextMenu (a
// session consumed by InvokeShellMenu sets ctxToken = 0 first, the host closes
// those itself) and clears the per-menu state.
static void CloseCtxSession(AppState& s) {
    if (s.ctxToken) s.ops.CloseShellMenu(s.ctxToken);
    s.ctxToken = 0;
    s.ctxPaths.clear();
    s.ctxBackground = false;
    s.ctxComReady = false;
    s.ctxComItems.clear();
    s.ctxStaticVerbs.clear();
    s.ctxStaticExt.clear();
    s.ctxQueryAt = 0;
}

static std::wstring ComCacheKey(bool background, const std::wstring& ext) {
    if (background) return L":bg";
    return ext.empty() ? L":file" : ext;
}

static void SeedComItemsFromCache(AppState& s) {
    if (s.ctxComReady || !s.ctxComItems.empty()) return;
    auto it = s.shellComCache.find(ComCacheKey(s.ctxBackground, s.ctxStaticExt));
    if (it == s.shellComCache.end() || it->second.empty()) return;
    s.ctxComItems = it->second;
}

// Fired on WM_RBUTTONDOWN (prefetch) and again on menu open (no-op when the
// target is unchanged): pulse_shell starts building the COM menu while the
// Fluent menu fades in.
static void StartCtxQuery(AppState& s, std::vector<std::wstring> paths,
                          bool background, const std::wstring& ext) {
    if (paths.empty()) return;
    for (auto& p : paths) p = ClipboardPath(p);
    if (s.ctxToken && s.ctxBackground == background && s.ctxPaths == paths) {
        PrefetchStaticVerbs(s, ext); // cheap re-check; usually cached already
        return;
    }
    CloseCtxSession(s);
    const bool extended = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    s.ctxToken = s.ops.QueryShellMenu(paths, s.hwnd, background, extended);
    s.ctxPaths = std::move(paths);
    s.ctxBackground = background;
    s.ctxComReady = false;
    s.ctxQueryAt = GetTickCount64();
    s.ctxStaticExt = background ? L"" : ext;
    if (!s.ctxStaticExt.empty()) {
        auto it = s.shellVerbCache.find(s.ctxStaticExt);
        if (it != s.shellVerbCache.end()) s.ctxStaticVerbs = it->second;
        else PrefetchStaticVerbs(s, s.ctxStaticExt);
    }
}

// Drain Explorer-menu replies already sitting in the queue. COM handlers are
// slow on a cold load; the fast+complete path is: prefetch on mouse-down,
// reuse the last layout for this extension so mouse-up paints a full menu,
// then swap in this session's live ids when RSP_CTX_ITEMS lands.
static void PumpShellMenuMessages(AppState& s) {
    MSG msg{};
    while (PeekMessageW(&msg, s.hwnd, WM_SHELLCTX_ITEMS, WM_SHELLCTX_ITEMS, PM_REMOVE) ||
           PeekMessageW(&msg, s.hwnd, WM_SHELL_VERBS, WM_SHELL_VERBS, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void WaitForShellMenuReady(AppState& s) {
    if (!s.hwnd || s.ctxToken == 0) return;
    PumpShellMenuMessages(s);
    if (s.ctxComReady) return;
    const bool have_cache =
        s.shellComCache.count(ComCacheKey(s.ctxBackground, s.ctxStaticExt)) != 0;
    if (have_cache) return; // first frame from cache; live ids replace in place
    // Cold extension: wait for the real IContextMenu (up to 2s from mouse-up).
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (!s.ctxComReady) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)(deadline - now),
                                  QS_ALLINPUT);
        PumpShellMenuMessages(s);
    }
}

static const ops::ShellMenuItem* FindComItemById(const std::vector<ops::ShellMenuItem>& items,
                                                 uint32_t id) {
    for (const auto& it : items)
        if (it.id == id) return &it;
    return nullptr;
}

static uint32_t FindComIdByText(const std::vector<ops::ShellMenuItem>& items,
                                const std::wstring& text) {
    if (text.empty()) return 0;
    for (const auto& it : items)
        if (!it.has_children && it.id != 0 && it.text == text) return it.id;
    return 0;
}

static void ScheduleFolderRefresh(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return;
    s.store.MarkDirty(tab->current_path);
    RefreshActiveTab(s);
    // Bandizip / 7-Zip often return from InvokeCommand before the archive
    // lands on disk; a second pass catches the late create. UNC shares have
    // no DirWatch (CreateFile on a dead SMB would stall the UI thread).
    const ULONGLONG now = GetTickCount64();
    s.shellRefreshAt = now + 400;
    s.shellRefreshAgainAt = now + 1600;
}

// Static verbs first (they were in the first frame), then COM items. A COM
// has_children header plus its following child rows become one nested entry
// (one-level flyout in the menu).
static std::vector<app::ShellMenuEntry> ComposeShellEntries(AppState& s) {
    std::vector<app::ShellMenuEntry> entries;
    entries.reserve(s.ctxStaticVerbs.size() + s.ctxComItems.size());
    for (size_t i = 0; i < s.ctxStaticVerbs.size(); ++i) {
        app::ShellMenuEntry e;
        e.command = app::CmdShellStaticBase + static_cast<int>(i);
        e.text = s.ctxStaticVerbs[i].display;
        e.verb = s.ctxStaticVerbs[i].verb;
        e.from_com = false;
        entries.push_back(std::move(e));
    }
    const auto& items = s.ctxComItems;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        if (it.child) continue; // consumed by its header below
        if (it.has_children) {
            app::ShellMenuEntry parent;
            parent.command = 0;
            parent.text = it.text;
            parent.enabled = it.enabled;
            parent.verb = it.verb;
            parent.from_com = true;
            for (size_t j = i + 1; j < items.size() && items[j].child; ++j) {
                if (items[j].id > 0x7FFF) continue;
                app::ShellMenuEntry c;
                c.command = app::CmdShellComBase + static_cast<int>(items[j].id);
                c.text = items[j].text;
                c.enabled = items[j].enabled;
                c.verb = items[j].verb;
                c.from_com = true;
                parent.children.push_back(std::move(c));
            }
            if (!parent.children.empty()) entries.push_back(std::move(parent));
            continue;
        }
        if (it.id > 0x7FFF) continue;
        app::ShellMenuEntry e;
        e.command = app::CmdShellComBase + static_cast<int>(it.id);
        e.text = it.text;
        e.enabled = it.enabled;
        e.verb = it.verb;
        e.from_com = true;
        entries.push_back(std::move(e));
    }

    bool grew = false;
    for (const auto& e : entries) {
        const bool flyout = !e.children.empty();
        const auto cat = ipc::ClassifyExplorerItem(e.verb, e.text, flyout);
        if (s.ctxMenuPrefs.RecordSeen(ipc::CatalogKey(e.text, flyout), e.text, flyout, cat,
                                      e.from_com))
            grew = true;
    }
    if (grew) s.ctxMenuPrefs.Save();
    return app::ApplyExplorerPrefs(s.ctxMenuPrefs, entries);
}

static void AppendExplorerSection(AppState& s, std::vector<ui::FluentMenuItem>& display) {
    app::AppendShellSection(display, ComposeShellEntries(s));
    ui::FluentMenuItem manage;
    manage.command = app::CmdSettingsContextMenu;
    manage.text = L"管理右键项…";
    manage.glyph = L"\xE713";
    if (!display.empty()) display.back().separator_after = true;
    display.push_back(std::move(manage));
}

static void RefreshOpenCtxMenu(AppState& s) {
    if (!s.ctxMenuOpen || !s.menu || !s.menu->IsOpen()) return;
    std::vector<ui::FluentMenuItem> display = s.ctxBaseItems;
    AppendExplorerSection(s, display);
    s.menu->ReplaceItems(std::move(display));
}

// Dispatch for the merged Explorer rows. Returns true when cmd was a shell
// row (or the menu was dismissed) and no built-in dispatch should run.
static bool HandleShellMenuCommand(AppState& s, int cmd) {
    if (cmd >= app::CmdShellComBase) {
        const uint32_t clicked = static_cast<uint32_t>(cmd - app::CmdShellComBase);
        std::wstring text;
        if (const auto* hit = FindComItemById(s.ctxComItems, clicked))
            text = hit->text;
        if (!s.ctxComReady)
            WaitForShellMenuReady(s);
        uint32_t live = s.ctxComReady ? FindComIdByText(s.ctxComItems, text) : 0;
        if (live == 0 && s.ctxComReady) live = clicked;
        if (s.ctxToken && live != 0) {
            s.ops.InvokeShellMenu(s.ctxToken, live);
            s.ctxToken = 0; // host closes the session after invoking
        }
        CloseCtxSession(s);
        ScheduleFolderRefresh(s);
        return true;
    }
    if (cmd >= app::CmdShellStaticBase && cmd < app::CmdShellComBase) {
        const size_t idx = static_cast<size_t>(cmd - app::CmdShellStaticBase);
        if (idx < s.ctxStaticVerbs.size()) {
            const app::StaticVerb verb = s.ctxStaticVerbs[idx];
            constexpr size_t kMaxTargets = 16;
            for (size_t i = 0; i < s.ctxPaths.size() && i < kMaxTargets; ++i) {
                if (!verb.app_path.empty()) s.ops.OpenWithApp(verb.app_path, s.ctxPaths[i]);
                else s.ops.ExecuteVerb(s.ctxPaths[i], verb.verb);
                if (verb.verb == L"openas") break; // one picker dialog is enough
            }
        }
        CloseCtxSession(s);
        ScheduleFolderRefresh(s);
        return true;
    }
    // Built-in command or dismissed: the session is no longer needed.
    CloseCtxSession(s);
    return false;
}

static void ShowItemContextMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    app::Tab* tab = ActiveTab(s);
    const std::vector<std::wstring> paths =
        tab ? SelectedFullPaths(*tab) : std::vector<std::wstring>{};
    if (tab && !paths.empty())
        StartCtxQuery(s, paths, false, CommonExtension(*tab, tab->SelectedIndices()));
    WaitForShellMenuReady(s);
    SeedComItemsFromCache(s);

    std::wstring undoLabel = s.ops.UndoLabel();
    s.ctxBaseItems = BuildFinderItemMenu(s, s.ops.CanUndo(), undoLabel);
    std::vector<ui::FluentMenuItem> display = s.ctxBaseItems;
    AppendExplorerSection(s, display);
    const int quick_count = std::min(7, static_cast<int>(s.places.tags.size()));

    s.ctxMenuOpen = true;
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(display));
    s.ctxMenuOpen = false;
    if (HandleShellMenuCommand(s, cmd)) return;
    if (cmd == app::CmdTags) ShowTagPicker(s, screen_pt);
    else if (cmd >= app::CmdTagBase && cmd < app::CmdTagBase + quick_count)
        ToggleTagForSelection(s, s.places.tags[static_cast<size_t>(cmd - app::CmdTagBase)].id, paths);
    else if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

static void ShowBackgroundContextMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    app::Tab* tab = ActiveTab(s);
    if (tab && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
        StartCtxQuery(s, { tab->current_path }, true, L"");
    WaitForShellMenuReady(s);
    SeedComItemsFromCache(s);

    std::wstring undoLabel = s.ops.UndoLabel();
    bool canPaste = !s.tray.batches().empty() || ClipboardHasFiles();
    s.ctxBaseItems = app::BuildBackgroundMenu(canPaste, s.ops.CanUndo(), undoLabel);
    ApplyWorkspacePinLabel(s.ctxBaseItems, s);
    std::vector<ui::FluentMenuItem> display = s.ctxBaseItems;
    AppendExplorerSection(s, display);

    s.ctxMenuOpen = true;
    const int cmd = s.menu->TrackPopup(screen_pt, std::move(display));
    s.ctxMenuOpen = false;
    if (HandleShellMenuCommand(s, cmd)) return;
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

static void ShowNewDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    D2D1_RECT_F addr = s.renderer.AddressBarRect((float)s.compositor.Width());
    POINT pt{ (LONG)(addr.right + s.renderer.Margin()),
              (LONG)(s.renderer.TitleBarHeight() + s.renderer.ToolbarHeight()) };
    ClientToScreen(s.hwnd, &pt);
    int cmd = s.menu->TrackPopup(pt, app::BuildNewMenu());
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

// ---------------------------------------------------------------------------
// Stage 1B-2: OLE drag & drop wiring.
// ---------------------------------------------------------------------------
static void ClearDropFeedback(AppState& s) {
    s.dropRow = -1;
    s.dropPaneIndex = -1;
    s.dropBreadcrumb = -1;
    s.dropSidebar = -1;
    s.dropTray = false;
    s.dropBadge.clear();
    s.dropDestDir.clear();
    s.springRow = -1;
    s.springStart = 0;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static std::wstring BaseName(const std::wstring& path) {
    std::wstring_view v = path;
    if (v.size() > 1 && v.back() == L'\\') v.remove_suffix(1);
    auto pos = v.find_last_of(L"\\/");
    return std::wstring(pos != std::wstring::npos ? v.substr(pos + 1) : v);
}

// Resolves the drop target under pt (client coords), updates feedback state,
// and returns the DROPEFFECT_* to report back. Also drives the 800ms
// spring-loaded folder enter and Esc-back.
static DWORD ResolveDropTarget(AppState& s, const std::vector<std::wstring>& sources,
                               POINT pt, DWORD key_state, DWORD allowed) {
    s.dropRow = -1;
    s.dropPaneIndex = -1;
    s.dropBreadcrumb = -1;
    s.dropSidebar = -1;
    s.dropTray = false;
    s.dropDestDir.clear();
    s.dropBadge.clear();

    // Esc after a spring-loaded enter: go back instead of cancelling (self drags).
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) && s.springEntered) {
        GoBack(s);
        s.springEntered = false;
        return DROPEFFECT_NONE;
    }

    ui::WindowViewModel vm = BuildVm(s);
    D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s.compositor.Width(), (float)s.compositor.Height());
    ui::HitTestResult hit = s.renderer.HitTest(vm, rect, (float)pt.x, (float)pt.y);

    app::Pane* hitPane = PaneAtSlot(s, hit.pane_index);
    app::Tab* tab = hitPane ? hitPane->ActiveTab() : ActiveTab(s);
    if (!tab) return DROPEFFECT_NONE;

    std::wstring destName;
    if (hit.region == ui::HitTestResult::Row && hit.index >= 0 &&
        tab->snapshot && hit.index < (int)tab->snapshot->size() &&
        (*tab->snapshot)[hit.index].is_dir) {
        std::wstring full = tab->current_path;
        if (!full.ends_with(L"\\")) full += L"\\";
        full += (*tab->snapshot)[hit.index].name;
        s.dropDestDir = full;
        s.dropRow = hit.index;
        s.dropPaneIndex = hit.pane_index;
        destName = (*tab->snapshot)[hit.index].name;

        // Spring-loaded: hover 800ms on a folder row enters it (ui.md §7.8).
        if (s.springRow != hit.index) {
            s.springRow = hit.index;
            s.springStart = GetTickCount64();
        } else if (!s.springEntered && GetTickCount64() - s.springStart >= 800) {
            s.springEntered = true;
            s.springRow = -1;
            if (hitPane && hitPane != s.pane) FocusPane(s, hitPane);
            NavigateTo(s, full);
            return DROPEFFECT_NONE;
        }
    } else if (hit.region == ui::HitTestResult::BreadcrumbSegment && !hit.path.empty()) {
        s.dropDestDir = hit.path;
        s.dropBreadcrumb = hit.index;
        destName = BaseName(hit.path);
        s.springRow = -1;
    } else if (hit.region == ui::HitTestResult::SidebarItem && !hit.path.empty()) {
        if (hit.path.starts_with(L"pulse:tag:")) {
            s.dropDestDir = hit.path;
            s.dropSidebar = hit.index;
            destName.clear();
            s.springRow = -1;
            s.dropBadge = L"打标签";
            s.dropBadgeX = (float)pt.x;
            s.dropBadgeY = (float)pt.y;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return DROPEFFECT_COPY;
        }
        if (hit.path.starts_with(L"pulse:workspace:")) {
            std::wstring kind, rest;
            app::ParsePulsePath(hit.path, &kind, &rest);
            int wi = _wtoi(rest.c_str());
            if (wi >= 0 && wi < static_cast<int>(s.places.workspaces.size()))
                s.dropDestDir = s.places.workspaces[static_cast<size_t>(wi)].root;
            else
                s.dropDestDir.clear();
        } else if (hit.path.starts_with(L"pulse:")) {
            s.dropDestDir.clear();
        } else {
            s.dropDestDir = hit.path;
        }
        s.dropSidebar = hit.index;
        destName = BaseName(s.dropDestDir.empty() ? hit.path : s.dropDestDir);
        s.springRow = -1;
    } else if (hit.pane_index >= 0 ||
               hit.region == ui::HitTestResult::Row ||
               hit.region == ui::HitTestResult::Pane ||
               hit.region == ui::HitTestResult::ColumnHeader ||
               hit.region == ui::HitTestResult::FilterBox ||
               hit.region == ui::HitTestResult::Scrollbar) {
        s.dropDestDir = tab->current_path;
        s.dropPaneIndex = hit.pane_index;
        destName = BaseName(tab->current_path);
        s.springRow = -1;
    }
    // Tray zone: bottom card of the sidebar = staging, not a move.
    D2D1_RECT_F sb = s.renderer.SidebarRect(rect.right, rect.bottom);
    if (s.dropDestDir.empty() && pt.x < sb.right) {
        ui::WindowViewModel trayVm = BuildVm(s);
        D2D1_RECT_F trayRc = s.renderer.StagingTrayRect(trayVm, rect.right, rect.bottom);
        if (pt.x >= trayRc.left && pt.x < trayRc.right &&
            pt.y >= trayRc.top && pt.y < trayRc.bottom) {
            s.dropTray = true;
            s.springRow = -1;
            s.dropBadge = L"暂存到托盘";
            s.dropBadgeX = (float)pt.x;
            s.dropBadgeY = (float)pt.y;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return DROPEFFECT_COPY; // staging never moves the source
        }
    }
    if (s.dropDestDir.empty()) {
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return DROPEFFECT_NONE;
    }

    DWORD effect = ui::ComputeDropEffect(key_state, sources.front(), s.dropDestDir, allowed);
    s.dropBadge = (effect == DROPEFFECT_MOVE ? L"移动到 " : L"复制到 ") + destName;
    s.dropBadgeX = (float)pt.x;
    s.dropBadgeY = (float)pt.y;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return effect;
}

static DWORD DropExecute(AppState& s, const std::vector<std::wstring>& sources,
                         POINT pt, DWORD key_state, DWORD preferred) {
    // Resolve once more for the final position.
    DWORD effect = ResolveDropTarget(s, sources, pt, key_state, DROPEFFECT_COPY | DROPEFFECT_MOVE);
    std::wstring dest = s.dropDestDir;
    bool tray = s.dropTray;
    ClearDropFeedback(s);
    s.springEntered = false;

    if (tray) {
        std::vector<std::wstring> paths;
        for (auto& p : sources) paths.push_back(fs::NormalizePath(p));
        s.tray.Collect(paths, (preferred & DROPEFFECT_MOVE) != 0);
        ops::WriteClipboard(sources, (preferred & DROPEFFECT_MOVE) != 0);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return DROPEFFECT_COPY;
    }
    if (dest.starts_with(L"pulse:tag:")) {
        std::wstring kind, rest;
        app::ParsePulsePath(dest, &kind, &rest);
        const int ti = s.places.FindTagIndex(s.places.ResolveTagRef(rest));
        std::vector<app::TagAdsUpdate> ads_updates;
        s.places.SetTaggedBatch(ti, sources, true, &ads_updates);
        QueueTagAds(s, std::move(ads_updates));
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return DROPEFFECT_COPY;
    }
    if (dest.empty() || effect == DROPEFFECT_NONE || fs::IsVirtualPath(dest)) return DROPEFFECT_NONE;

    ops::OpRequest req;
    req.type = (effect == DROPEFFECT_MOVE) ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = fs::NormalizePath(dest);
    for (auto& p : sources) req.sources.push_back(fs::NormalizePath(p));
    return SubmitWithConflictResolution(s, std::move(req)) ? effect : DROPEFFECT_NONE;
}

// Starts the modal OLE drag-out for the selected entries.
static void StartDragOut(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (paths.empty()) return;
    for (auto& path : paths) path = ClipboardPath(path);

    s.clickCollapseIndex = -1;
    CancelScrollAnimation(s); // DoDragDrop's modal loop coexists with on-demand render
    DWORD effect = ui::DoFileDragDrop(paths, DROPEFFECT_COPY | DROPEFFECT_MOVE,
        [&s] { return s.springEntered; }); // Esc = 退回 when spring-entered
    s.springEntered = false;
    ClearDropFeedback(s);
    if (effect == DROPEFFECT_MOVE) {
        // The target took the file; refresh the listing.
        s.store.MarkDirty(tab->current_path);
        RefreshActiveTab(s);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static float MaxScrollForActivePane(AppState& s, ui::PaneViewModel* out = nullptr) {
    if (!s.pane || !s.pane->ActiveTab()) return 0.0f;
    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    const float max_scroll = s.renderer.MaxScrollForPane(pane, FocusedPaneRect(s));
    if (out) *out = std::move(pane);
    return max_scroll;
}

static void ClampScroll(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    const float maxScroll = MaxScrollForActivePane(s);
    tab->scroll_y = std::clamp(tab->scroll_y, 0.0f, maxScroll);
    s.scrollTargetY = std::clamp(s.scrollTargetY, 0.0f, maxScroll);
    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    const float maxScrollX = s.renderer.MaxScrollXForPane(pane, FocusedPaneRect(s));
    tab->scroll_x = std::clamp(tab->scroll_x, 0.0f, maxScrollX);
}

static void EnsureRowVisible(AppState& s, app::Tab& tab, int index) {
    if (index < 0) return;
    const D2D1_RECT_F list = ListRect(s);
    int viewRow = index;
    ui::PaneViewModel pane;
    MaxScrollForActivePane(s, &pane);
    if (!pane.filter_text.empty()) {
        viewRow = pane.ViewIndex(index);
        if (viewRow < 0) return;
    }
    const D2D1_RECT_F item = s.renderer.ItemRectInPane(pane, FocusedPaneRect(s), viewRow);
    if (item.top < list.top) tab.scroll_y += item.top - list.top;
    else if (item.bottom > list.bottom) tab.scroll_y += item.bottom - list.bottom;
    if (item.left < list.left) tab.scroll_x += item.left - list.left;
    else if (item.right > list.right) tab.scroll_x += item.right - list.right;
    ClampScroll(s);
}

static bool PointInList(const AppState& s, int mx, int my) {
    const D2D1_RECT_F list = ListRect(s);
    return mx >= list.left && mx < list.right && my >= list.top && my < list.bottom;
}

static void ResetMarquee(AppState& s) {
    s.marqueePending = false;
    s.marqueeActive = false;
    s.marqueeAdditive = false;
    s.marqueeBase.clear();
}

static void ApplyMarqueeSelection(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    const int n = static_cast<int>(tab->snapshot->size());
    const D2D1_RECT_F list = ListRect(s);
    const float left = static_cast<float>(std::min(s.marqueeStart.x, s.marqueeCur.x));
    const float top = static_cast<float>(std::min(s.marqueeStart.y, s.marqueeCur.y));
    const float right = static_cast<float>(std::max(s.marqueeStart.x, s.marqueeCur.x));
    const float bottom = static_cast<float>(std::max(s.marqueeStart.y, s.marqueeCur.y));
    const float clipL = std::max(left, list.left);
    const float clipT = std::max(top, list.top);
    const float clipR = std::min(right, list.right);
    const float clipB = std::min(bottom, list.bottom);

    tab->all_selected = false;
    tab->selected.clear();
    if (s.marqueeAdditive) tab->selected = s.marqueeBase;

    if (n > 0 && clipR > clipL && clipB > clipT) {
        ui::PaneViewModel pane;
        app::FillPaneViewModel(pane, *s.pane, &s.places);
        const auto [first, last] = s.renderer.VisibleRangeInPane(pane, FocusedPaneRect(s));
        const D2D1_RECT_F marquee = D2D1::RectF(clipL, clipT, clipR, clipB);
        for (int view = first; view >= 0 && view <= last; ++view) {
            const D2D1_RECT_F item = s.renderer.ItemRectInPane(pane, FocusedPaneRect(s), view);
            if (item.right <= marquee.left || item.left >= marquee.right ||
                item.bottom <= marquee.top || item.top >= marquee.bottom) continue;
            const int source = pane.SourceIndex(view);
            if (source >= 0) tab->selected.insert(source);
        }
    }

    if (tab->selected.empty()) {
        tab->selected_index = -1;
        return;
    }
    ui::PaneViewModel focusPane;
    app::FillPaneViewModel(focusPane, *s.pane, &s.places);
    int focus = s.renderer.ItemFromPointInPane(focusPane, FocusedPaneRect(s),
                                               static_cast<float>(s.marqueeCur.x),
                                               static_cast<float>(s.marqueeCur.y));
    if (tab->selected.contains(focus)) tab->selected_index = focus;
    else tab->selected_index = *tab->selected.begin();
    if (tab->selection_anchor < 0) tab->selection_anchor = tab->selected_index;
    if (static_cast<int>(tab->selected.size()) == n) {
        tab->all_selected = true;
        tab->selected.clear();
    }
}

static void HandleListRowClick(AppState& s, int index, bool ctrl, bool shift) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    s.clickCollapseIndex = -1;
    if (shift) {
        const int anchor = tab->selection_anchor >= 0 ? tab->selection_anchor
            : (tab->selected_index >= 0 ? tab->selected_index : index);
        tab->SelectRange(anchor, index);
    } else if (ctrl) {
        tab->ToggleSelect(index);
    } else if (tab->IsSelected(index) && tab->SelectedCount() > 1) {
        tab->selected_index = index;
        s.clickCollapseIndex = index;
    } else {
        tab->SelectOnly(index);
    }
}

static void CancelRenameClick(AppState& s) {
    s.renameClickCandidate = false;
    s.renameClickPane = nullptr;
    s.renameClickTab = nullptr;
    s.renameClickIndex = -1;
    s.renameClickDue = 0;
    s.renameClickPath.clear();
}

static bool PointInHitItemName(AppState& s, const ui::WindowViewModel& vm,
                               const ui::HitTestResult& hit, float x, float y) {
    if (hit.region != ui::HitTestResult::Row || hit.index < 0) return false;
    if (hit.pane_index >= 0 &&
        hit.pane_index < static_cast<int>(vm.pane_slots.size())) {
        const auto& slot = vm.pane_slots[static_cast<size_t>(hit.pane_index)];
        return s.renderer.PointInItemName(slot.pane, slot.rect, hit.index, x, y);
    }
    return s.renderer.PointInItemName(vm.pane, FocusedPaneRect(s), hit.index, x, y);
}

static void CancelScrollAnimation(AppState& s) {
    s.scrollAnimating = false;
}

// --- Tag slide animation ------------------------------------------------------
// Fluent spring curve shared by QFluentKit Pivot/SegmentedWidget indicators:
// cubic-bezier(0.34, 1.56, 0.64, 1.0) — y1 > 1 gives the visible overshoot.
static float TagEaseSpring(float x) {
    const float x1 = 0.34f, y1 = 1.56f, x2 = 0.64f, y2 = 1.0f;
    float t = x; // Newton-Raphson: solve x(t) = x for the bezier parameter
    for (int i = 0; i < 6; ++i) {
        const float u = 1.0f - t;
        const float bx = 3.0f * u * u * t * x1 + 3.0f * u * t * t * x2 + t * t * t - x;
        if (std::abs(bx) < 1e-4f) break;
        const float dx = 3.0f * u * u * x1 + 6.0f * u * t * (x2 - x1) + 3.0f * t * t * (1.0f - x2);
        if (std::abs(dx) < 1e-6f) break;
        t = std::clamp(t - bx / dx, 0.0f, 1.0f);
    }
    const float u = 1.0f - t;
    return 3.0f * u * u * t * y1 + 3.0f * u * t * t * y2 + t * t * t;
}

static float TagEaseInOutQuad(float t) { // used by the window-tab slide animation
    const float u = -2.0f * t + 2.0f;
    return t < 0.5f ? 2.0f * t * t : 1.0f - u * u * 0.5f;
}

static void TickTagTransitions(AppState& s) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = s.tagTracks.begin(); it != s.tagTracks.end();) {
        const float t = std::chrono::duration<float, std::milli>(now - it->second.t0).count()
            / static_cast<float>(it->second.durationMs);
        if (t >= 1.0f) {
            s.tagOffsets.erase(it->first);
            it = s.tagTracks.erase(it);
        } else {
            s.tagOffsets[it->first] = it->second.start * (1.0f - TagEaseSpring(t));
            ++it;
        }
    }
}

static void TickTabTransitions(AppState& s) {
    std::unordered_set<const app::Tab*> live;
    if (s.pane) {
        for (const auto& t : s.pane->tabs) live.insert(t.get());
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto it = s.tabTracks.begin(); it != s.tabTracks.end();) {
        if (!live.count(it->first)) {
            s.tabOffsets.erase(it->first);
            it = s.tabTracks.erase(it);
            continue;
        }
        const float t = std::chrono::duration<float, std::milli>(now - it->second.t0).count()
            / static_cast<float>(it->second.durationMs);
        if (t >= 1.0f) {
            s.tabOffsets.erase(it->first);
            it = s.tabTracks.erase(it);
        } else {
            s.tabOffsets[it->first] = it->second.start * (1.0f - TagEaseInOutQuad(t));
            ++it;
        }
    }
    for (auto it = s.tabOffsets.begin(); it != s.tabOffsets.end();) {
        if (!live.count(it->first)) it = s.tabOffsets.erase(it);
        else ++it;
    }
    // Chip slide channel: same easing, px units, keyed by group id.
    std::unordered_set<int> liveGroups;
    if (s.pane) {
        for (const auto& g : s.pane->tab_groups) liveGroups.insert(g.id);
    }
    for (auto it = s.chipTracks.begin(); it != s.chipTracks.end();) {
        if (!liveGroups.count(it->first)) {
            s.chipOffsets.erase(it->first);
            it = s.chipTracks.erase(it);
            continue;
        }
        const float t = std::chrono::duration<float, std::milli>(now - it->second.t0).count()
            / static_cast<float>(it->second.durationMs);
        if (t >= 1.0f) {
            s.chipOffsets.erase(it->first);
            it = s.chipTracks.erase(it);
        } else {
            s.chipOffsets[it->first] = it->second.start * (1.0f - TagEaseInOutQuad(t));
            ++it;
        }
    }
    for (auto it = s.chipOffsets.begin(); it != s.chipOffsets.end();) {
        if (!liveGroups.count(it->first)) it = s.chipOffsets.erase(it);
        else ++it;
    }
}

static void UpdateSmoothScroll(AppState& s);

static void StartSmoothScroll(AppState& s, float delta) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    ClampScroll(s);

    // Preserve the distance from earlier wheel pulses. Restarting from the
    // partially animated position discards most of a fast wheel gesture.
    if (s.scrollAnimating) {
        UpdateSmoothScroll(s);
    } else {
        s.scrollTargetY = tab->scroll_y;
        s.scrollLastUpdateTime = std::chrono::steady_clock::now();
    }

    const float maxScroll = MaxScrollForActivePane(s);
    s.scrollTargetY = std::clamp(s.scrollTargetY + delta, 0.0f, maxScroll);
    s.scrollAnimating = true;
    MaybePrefetchSearchPage(s);
}

static void UpdateSmoothScroll(AppState& s) {
    if (!s.scrollAnimating) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) { s.scrollAnimating = false; return; }
    auto now = std::chrono::steady_clock::now();
    const double elapsed = std::clamp(
        std::chrono::duration<double, std::milli>(now - s.scrollLastUpdateTime).count(),
        0.0, 50.0);
    s.scrollLastUpdateTime = now;

    const float remaining = s.scrollTargetY - tab->scroll_y;
    if (std::abs(remaining) <= 0.35f) {
        tab->scroll_y = s.scrollTargetY;
        s.scrollAnimating = false;
    } else {
        // Exponential response is independent of timer jitter and accepts a
        // moving target without resetting its easing curve on every pulse.
        const float response = 1.0f - static_cast<float>(
            std::exp(-elapsed / AppState::kScrollResponseMs));
        tab->scroll_y += remaining * response;
    }
    ClampScroll(s);
}

static std::vector<std::wstring> CollectPanePaths(const AppState& s) {
    std::vector<std::wstring> out;
    if (!s.root) return out;
    std::vector<app::Pane*> vis;
    s.root->CollectPanes(vis);
    for (app::Pane* p : vis) {
        app::Tab* t = p ? p->ActiveTab() : nullptr;
        out.push_back(t ? t->current_path : L"");
    }
    return out;
}

static std::vector<ui::ViewMode> CollectPaneViews(const AppState& s) {
    std::vector<ui::ViewMode> out;
    if (!s.root) return out;
    std::vector<app::Pane*> panes;
    s.root->CollectPanes(panes);
    out.reserve(panes.size());
    for (const app::Pane* pane : panes) {
        const app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
        out.push_back(tab ? tab->view_mode : ui::ViewMode::Details);
    }
    return out;
}

static bool IsUncPath(const std::wstring& p) {
    return fs::IsUncPath(p);
}

static void PumpUncProbe(AppState& s) {
    if (s.probeBusy || s.probeQueue.empty() || !s.hwnd) return;
    s.probeUnc = s.probeQueue.front();
    s.probeQueue.erase(s.probeQueue.begin());
    s.probeBusy = true;
    fs::StartUncProbe(s.hwnd, WM_NET_PROBE, s.probeUnc);
}

static void RequestUncProbe(AppState& s, std::wstring unc) {
    if (!fs::IsUncPath(unc) || !s.hwnd) return;
    unc = fs::NormalizePath(unc);
    if (s.probeBusy && s.probeUnc == unc) return;
    for (const auto& q : s.probeQueue) if (q == unc) return;
    s.probeQueue.push_back(std::move(unc));
    PumpUncProbe(s);
}

static void WarmupUnc(AppState& s, const std::wstring& path) {
    if (!fs::IsUncPath(path)) return;
    RequestUncProbe(s, path);
    uint64_t gen = 0;
    if (s.store.GetOrStart(path, gen)) return;
    if (!s.store.Peek(path)) {
        uint64_t ts = 0;
        if (auto disk = fs::LoadNetSnapshot(path, &ts))
            s.store.Update(path, gen, disk);
    }
    s.worker.Refresh(path, ui::SortColumn::Name, ui::SortDirection::Asc);
}

// Rebuild a pane's full tab strip and groups from a version-5 session entry.
static void RestorePaneTabs(AppState& s, app::Pane& pane,
                            const app::PaneSessionSnapshot& snap) {
    // Groups first so tab membership ids can be validated against them.
    pane.tab_groups.clear();
    int maxGroupId = 0;
    for (const auto& g : snap.groups) {
        if (g.id <= 0) continue;
        app::TabGroup grp;
        grp.id = g.id;
        grp.name = g.name;
        grp.color_rgb = g.color_rgb;
        grp.collapsed = g.collapsed;
        pane.tab_groups.push_back(std::move(grp));
        maxGroupId = std::max(maxGroupId, g.id);
    }
    pane.next_tab_group_id = std::max(pane.next_tab_group_id, maxGroupId + 1);

    pane.tabs.clear();
    pane.active_tab = 0;
    for (const auto& ts : snap.tabs) {
        if (ts.path.empty()) continue;
        auto tab = std::make_unique<app::Tab>();
        tab->pinned = ts.pinned;
        tab->view_mode = ts.view;
        // Drop membership in groups that were not restored.
        int gid = ts.group;
        if (gid != 0) {
            bool known = false;
            for (const auto& g : pane.tab_groups)
                if (g.id == gid) { known = true; break; }
            if (!known) gid = 0;
        }
        tab->tab_group = gid;
        app::Tab* raw = tab.get();
        pane.tabs.push_back(std::move(tab));
        WarmupUnc(s, ts.path);
        StartLoadingPath(s, *raw, ts.path);
    }
    if (pane.tabs.empty()) {
        pane.NewTab(L"C:\\");
        StartLoadingPath(s, *pane.ActiveTab(), L"C:\\");
    }
    size_t active = snap.active >= 0 ? static_cast<size_t>(snap.active) : 0;
    if (active >= pane.tabs.size()) active = pane.tabs.size() - 1;
    pane.SwitchTab(active);
    app::NormalizeGroupRuns(pane);
}

static std::wstring PinCandidate(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return L"";
    if (tab->SelectedCount() == 1 && tab->snapshot && tab->selected_index >= 0 &&
        tab->selected_index < static_cast<int>(tab->snapshot->size()) &&
        (*tab->snapshot)[static_cast<size_t>(tab->selected_index)].is_dir) {
        return EntryFullPath(*tab, tab->selected_index);
    }
    if (fs::IsVirtualPath(tab->current_path)) return L"";
    return tab->current_path;
}

static std::wstring ProjectSearchRoot(AppState& s) {
    if (s.places.active_workspace >= 0 &&
        s.places.active_workspace < static_cast<int>(s.places.workspaces.size())) {
        return s.places.workspaces[static_cast<size_t>(s.places.active_workspace)].root;
    }
    app::Tab* tab = ActiveTab(s);
    if (!tab) return L"";
    std::wstring git = app::FindGitRoot(tab->current_path);
    if (!git.empty()) return git;
    return fs::IsVirtualPath(tab->current_path) ? L"" : tab->current_path;
}

static void OpenWorkspace(AppState& s, int index) {
    if (index < 0 || index >= static_cast<int>(s.places.workspaces.size())) return;
    const app::Workspace w = s.places.workspaces[static_cast<size_t>(index)];
    s.places.active_workspace = index;
    ApplyLayoutPreset(s, static_cast<app::LayoutPreset>(std::clamp(w.layout, 0, 4)));
    if (w.pane_paths.empty()) {
        NavigateTo(s, w.root);
    } else {
        for (size_t i = 0; i < s.panes.size() && i < w.pane_paths.size(); ++i) {
            const std::wstring& pth = w.pane_paths[i];
            if (pth.empty()) continue;
            app::Tab* t = s.panes[i]->ActiveTab();
            if (!t) {
                s.panes[i]->NewTab(pth);
                t = s.panes[i]->ActiveTab();
            }
            if (t && i < w.pane_views.size()) t->view_mode = w.pane_views[i];
            if (t) StartLoadingPath(s, *t, pth);
        }
        RememberPath(s, w.root);
    }
    s.places.Save();
    for (const auto& n : s.places.networks) WarmupUnc(s, n.unc);
    for (const auto& pth : w.pane_paths) WarmupUnc(s, pth);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static index::Query MakeSearchPageQuery(const app::Tab& tab, const std::wstring& rest,
                                        size_t offset) {
    index::Query q;
    q.needle = rest;
    q.offset = offset;
    q.limit = index::kSearchUiPageSize;
    q.rank = false;
    q.sort = index::ResultSort::Name;
    q.sort_desc = tab.sort_direction == ui::SortDirection::Desc;
    switch (tab.sort_column) {
    case ui::SortColumn::Size: q.sort = index::ResultSort::Size; break;
    case ui::SortColumn::Mtime: q.sort = index::ResultSort::Mtime; break;
    default: q.sort = index::ResultSort::Name; break;
    }
    return q;
}

static void RequestSearchPage(AppState& s, app::Tab& tab, const std::wstring& rest,
                              bool reset) {
    const size_t offset = reset ? 0 : tab.search_next_offset;
    if (!reset && (tab.search_loading_more || offset >= tab.search_total)) return;
    if (reset) {
        tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
        tab.search_total = 0;
        tab.search_next_offset = 0;
        tab.SetSnapshot(tab.search_entries);
        tab.loading = true;
    } else {
        tab.search_loading_more = true;
    }
    const uint32_t id = ++s.nextIndexReq;
    tab.pending_generation = id;
    tab.pending_search_offset = offset;
    s.index.SearchAsync(MakeSearchPageQuery(tab, rest, offset), id);
}

static void ApplySearchHits(app::Tab& tab, const std::wstring& rest,
                            index::SearchResult&& result) {
    const size_t offset = tab.pending_search_offset;
    if (!tab.search_entries || offset == 0) {
        tab.search_entries = std::make_shared<std::vector<fs::DirEntry>>();
    }
    auto& entries = *tab.search_entries;
    if (offset != entries.size()) {
        tab.search_loading_more = false;
        tab.pending_generation = 0;
        return;
    }
    entries.reserve(entries.size() + result.hits.size());
    for (auto& hit : result.hits) {
        fs::DirEntry e;
        e.full_path = std::move(hit.path);
        e.name = std::move(hit.name);
        e.is_dir = hit.is_dir;
        e.size = hit.size;
        e.mtime.dwLowDateTime = static_cast<DWORD>(hit.mtime);
        e.mtime.dwHighDateTime = static_cast<DWORD>(hit.mtime >> 32);
        e.attrs = hit.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        entries.push_back(std::move(e));
    }
    tab.search_total = result.total;
    tab.search_next_offset = entries.size();
    if (tab.search_total > entries.size()) {
        tab.virtual_title = L"搜索 “" + rest + L"” · 共 " + std::to_wstring(tab.search_total) +
                            L" 项（已加载 " + std::to_wstring(entries.size()) + L"）";
    } else {
        tab.virtual_title = L"搜索 “" + rest + L"” · " + std::to_wstring(tab.search_total) + L" 项";
    }
    tab.SetSnapshot(tab.search_entries);
    tab.loading = false;
    tab.search_loading_more = false;
    tab.pending_generation = 0;
    if (offset == 0 && tab.snapshot && !tab.snapshot->empty()) tab.SelectOnly(0);
}

static void MaybePrefetchSearchPage(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->loading || tab->search_loading_more ||
        tab->search_next_offset >= tab->search_total) return;
    std::wstring kind, rest;
    if (!app::ParsePulsePath(tab->current_path, &kind, &rest) || kind != L"search") return;
    const D2D1_RECT_F list = ListRect(s);
    const float view_h = std::max(0.0f, list.bottom - list.top);
    const float max_scroll = MaxScrollForActivePane(s);
    const float scroll_y = std::max(tab->scroll_y, s.scrollTargetY);
    if (scroll_y + view_h * 2.0f >= max_scroll)
        RequestSearchPage(s, *tab, rest, false);
}

static void LoadVirtualView(AppState& s, app::Tab& tab, const std::wstring& path) {
    tab.current_path = path;
    tab.loading = false;
    tab.pending_generation = 0;
    tab.virtual_title.clear();
    if (s.pane && s.pane->ActiveTab() == &tab && s.watcher) s.watcher->Stop();

    std::wstring kind, rest;
    app::ParsePulsePath(path, &kind, &rest);
    if (kind == L"workspace") {
        OpenWorkspace(s, _wtoi(rest.c_str()));
        return;
    }

    if (kind == L"tag") {
        const app::TagId tag_id = s.places.ResolveTagRef(rest);
        const int ti = s.places.FindTagIndex(tag_id);
        if (ti >= 0 && ti < static_cast<int>(s.places.tags.size())) {
            const auto& tag = s.places.tags[static_cast<size_t>(ti)];
            tab.current_path = app::MakeTagPath(tag.id);
            tab.virtual_title = tag.name;
            tab.loading = true;
            tab.SetSnapshot(nullptr);
            tab.pending_generation = s.worker.LoadPaths(
                path, tag.paths, tab.sort_column, tab.sort_direction);
            return;
        }
    } else if (kind == L"search") {
        tab.virtual_title = L"搜索 “" + rest + L"”…";
        RequestSearchPage(s, tab, rest, true);
        return;
    } else if (kind == L"starred") {
        tab.virtual_title = L"星标常用文件";
        tab.loading = true;
        tab.SetSnapshot(nullptr);
        tab.pending_generation = s.worker.LoadPaths(
            path, s.places.starred, tab.sort_column, tab.sort_direction);
        return;
    } else if (kind == L"settings") {
        tab.virtual_title = L"设置";
        tab.loading = false;
        tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        s.settingsPage = (rest == L"context") ? 1 : 0;
        s.settingsScroll = 0.0f;
        return;
    }
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    tab.SetSnapshot(std::move(entries));
    if (tab.snapshot && !tab.snapshot->empty()) tab.SelectOnly(0);
}

static void StartLoadingPath(AppState& s, app::Tab& tab, const std::wstring& path) {
    std::wstring normalized = fs::NormalizePath(path);
    tab.current_path = normalized;
    if (const auto git = s.gitRoots.find(normalized); git != s.gitRoots.end())
        tab.git_root = git->second;
    else
        tab.git_root.clear();
    tab.loading = true;
    tab.ClearSelection();
    tab.pending_selected_name.clear();
    tab.pending_selected_names.clear();
    tab.pending_ensure_selection_visible = false;
    tab.pending_generation = 0;
    tab.applied_generation = 0;
    tab.scroll_y = 0.0f;
    tab.search_entries.reset();
    tab.search_total = 0;
    tab.search_next_offset = 0;
    tab.pending_search_offset = 0;
    tab.search_loading_more = false;
    if (fs::IsVirtualPath(normalized)) {
        LoadVirtualView(s, tab, normalized);
        return;
    }
    tab.virtual_title.clear();
    tab.banner_title.clear();
    tab.banner_message.clear();
    tab.net_readonly = false;
    tab.cache_unix = 0;
    if (tab.snapshot_path != normalized) {
        tab.SetSnapshot(nullptr);
        tab.applied_generation = 0;
    }
    const bool focused = s.pane && s.pane->ActiveTab() == &tab;
    if (focused) {
        s.scrollTargetY = 0.0f;
        s.scrollAnimating = false;
    }

    uint64_t shared_generation = 0;
    fs::SnapshotPtr shared_snapshot;
    for (const auto& pane : s.panes) {
        for (const auto& owned : pane->tabs) {
            const app::Tab* other = owned.get();
            if (!other || other == &tab || other->current_path != normalized ||
                other->pending_generation == 0 ||
                other->sort_column != tab.sort_column ||
                other->sort_direction != tab.sort_direction) {
                continue;
            }
            shared_generation = other->pending_generation;
            shared_snapshot = other->snapshot;
            break;
        }
        if (shared_generation != 0) break;
    }

    uint64_t gen = 0;
    fs::SnapshotPtr snap;
    bool memory_fresh = false;
    if (shared_generation != 0) {
        snap = shared_snapshot ? shared_snapshot : s.store.Peek(normalized);
    } else {
        snap = s.store.GetOrStart(normalized, gen);
        memory_fresh = snap != nullptr;
        if (!snap) snap = s.store.Peek(normalized);
    }
    uint64_t disk_ts = 0;
    if (!snap && fs::IsUncPath(normalized))
        snap = fs::LoadNetSnapshot(normalized, &disk_ts);

    if (snap) {
        tab.SetSnapshot(snap);
        tab.loading = false;
        if (tab.snapshot && !tab.snapshot->empty()) tab.SelectOnly(0);
        if (shared_generation != 0) {
            tab.pending_generation = shared_generation;
        } else if (!memory_fresh) {
            tab.pending_generation = s.worker.Refresh(normalized, tab.sort_column, tab.sort_direction);
            if (fs::IsUncPath(normalized)) {
                tab.cache_unix = disk_ts;
                tab.banner_title = L"缓存";
                tab.banner_message = disk_ts
                    ? fs::FormatCacheAge(disk_ts) + L" · 正在刷新…"
                    : L"正在刷新…";
            }
        } else {
            tab.pending_generation = 0;
        }
    } else {
        tab.pending_generation = shared_generation != 0
            ? shared_generation
            : s.worker.Refresh(normalized, tab.sort_column, tab.sort_direction);
    }

    if (focused && s.watcher) {
        if (normalized.empty() || fs::IsUncPath(normalized)) {
            s.watcher->Stop();
        } else {
            s.watcher->Start(path, [&s]() {
                app::Tab* t = ActiveTab(s);
                if (t && !fs::IsVirtualPath(t->current_path) && !fs::IsUncPath(t->current_path)) {
                    s.watchLastChange.store(GetTickCount64(), std::memory_order_relaxed);
                    s.watchDirty.store(true, std::memory_order_release);
                }
            });
        }
    }
    if (fs::IsUncPath(normalized)) RequestUncProbe(s, normalized);
}

static void ApplyWorkerResult(AppState& s, app::WorkResult& res) {
    if (s.panes.empty()) return;
    if (res.error) {
        for (auto& pane : s.panes) {
            for (auto& owned : pane->tabs) {
                app::Tab* tab = owned.get();
                if (!tab || tab->current_path != res.path) continue;
                if (tab->pending_generation != 0 &&
                    tab->pending_generation != res.generation) continue;
                tab->loading = false;
                tab->pending_generation = 0;
                tab->net_readonly = fs::IsUncPath(res.path);
                tab->banner_title = tab->net_readonly ? L"离线" : L"无法打开";
                tab->banner_message = tab->snapshot
                    ? L"只读浏览上次快照" : L"目录不可用";
            }
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (res.snapshot && !fs::IsVirtualPath(res.path)) {
        s.store.Update(res.path, res.generation, res.snapshot);
    }
    if (!fs::IsVirtualPath(res.path)) s.gitRoots[res.path] = res.git_root;
    bool any = false;
    for (auto& pane : s.panes) {
        for (auto& owned : pane->tabs) {
            app::Tab* tab = owned.get();
            if (!tab || tab->current_path != res.path) continue;
            if (tab->pending_generation != 0 && tab->pending_generation != res.generation) continue;
            if (tab->applied_generation != 0 && res.generation < tab->applied_generation) continue;
            any = true;

            const bool focusedTab = (pane.get() == s.pane && tab == pane->ActiveTab());
            std::vector<std::wstring> pendingNames = std::move(tab->pending_selected_names);
            std::wstring pendingFocus = std::move(tab->pending_selected_name);
            const bool ensurePendingVisible = tab->pending_ensure_selection_visible;
            tab->pending_selected_names.clear();
            tab->pending_selected_name.clear();
            tab->pending_ensure_selection_visible = false;
            std::wstring renameTarget;
            if (focusedTab) {
                renameTarget = s.pendingRenameName;
                if (renameTarget.empty() && s.renameIndex >= 0 && tab->snapshot &&
                    s.renameIndex < static_cast<int>(tab->snapshot->size())) {
                    renameTarget = (*tab->snapshot)[s.renameIndex].name;
                }
            }
            tab->SetSnapshot(res.snapshot);
            tab->git_root = res.git_root;
            tab->loading = false;
            tab->pending_generation = 0;
            tab->applied_generation = res.generation;
            tab->net_readonly = false;
            tab->banner_title.clear();
            tab->banner_message.clear();
            tab->cache_unix = 0;

            bool startedRename = false;
            if (focusedTab && !renameTarget.empty() && tab->snapshot) {
                for (int i = 0; i < static_cast<int>(tab->snapshot->size()); ++i) {
                    if ((*tab->snapshot)[i].name != renameTarget) continue;
                    tab->SelectOnly(i);
                    EnsureRowVisible(s, *tab, i);
                    s.pendingRenameName.clear();
                    if (s.renameIndex >= 0) {
                        s.renameIndex = i;
                        LayoutRenameOverlay(s);
                    } else {
                        ShowRenameOverlay(s);
                    }
                    startedRename = true;
                    break;
                }
                if (!startedRename) {
                    s.pendingRenameName = renameTarget;
                    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
                }
            }
            if (!startedRename) {
                if (focusedTab && !s.pendingRenameName.empty()) {
                    if (!pendingNames.empty()) tab->RemapSelection(pendingNames, pendingFocus);
                    else tab->ClearSelection();
                } else if (!pendingNames.empty()) {
                    tab->RemapSelection(pendingNames, pendingFocus);
                } else if (tab->snapshot && !tab->snapshot->empty()) {
                    tab->SelectOnly(0);
                } else {
                    tab->ClearSelection();
                }
            }
            if (focusedTab && ensurePendingVisible && tab->selected_index >= 0) {
                EnsureRowVisible(s, *tab, tab->selected_index);
                s.scrollTargetY = tab->scroll_y;
                s.scrollAnimating = false;
            }
        }
    }
    if (!any) return;

    s.timing.enum_ms = res.enum_ms;
    s.timing.sort_ms = res.sort_ms;
    s.timing.sort_done_ms = res.enum_ms + res.sort_ms;

    if (s.shot.active) InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void ProcessPendingResults(AppState& s) {
    std::queue<app::WorkResult> batch;
    {
        std::lock_guard<std::mutex> lock(s.resultMutex);
        batch = std::move(s.results);
        s.results = {};
    }
    while (!batch.empty()) {
        ApplyWorkerResult(s, batch.front());
        batch.pop();
    }
}

static void RefreshActiveTab(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty()) return;
    if (fs::IsVirtualPath(tab->current_path)) {
        LoadVirtualView(s, *tab, tab->current_path);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    tab->pending_selected_names.clear();
    tab->pending_selected_name.clear();
    tab->pending_ensure_selection_visible = false;
    if (tab->snapshot && tab->SelectedCount() > 0) {
        for (int index : tab->SelectedIndices()) {
            if (index >= 0 && index < static_cast<int>(tab->snapshot->size()))
                tab->pending_selected_names.push_back((*tab->snapshot)[static_cast<size_t>(index)].name);
        }
        if (tab->selected_index >= 0 &&
            tab->selected_index < static_cast<int>(tab->snapshot->size())) {
            tab->pending_selected_name = (*tab->snapshot)[tab->selected_index].name;
        }
    }
    // Keep the last complete snapshot visible during sort/watch refreshes.
    // The loading placeholder is only for a path that has no content yet.
    tab->loading = !tab->snapshot;
    tab->pending_generation = 0;
    for (const auto& pane : s.panes) {
        for (const auto& owned : pane->tabs) {
            const app::Tab* other = owned.get();
            if (!other || other == tab || other->current_path != tab->current_path ||
                other->pending_generation == 0 ||
                other->sort_column != tab->sort_column ||
                other->sort_direction != tab->sort_direction) {
                continue;
            }
            tab->pending_generation = other->pending_generation;
            break;
        }
        if (tab->pending_generation != 0) break;
    }
    if (tab->pending_generation == 0) {
        tab->pending_generation = s.worker.Refresh(
            tab->current_path, tab->sort_column, tab->sort_direction);
    }
}

static void RestoreNavigationReturnSelection(AppState& s, app::Tab& tab,
                                             const std::wstring& childName) {
    if (childName.empty()) return;
    tab.pending_selected_name = childName;
    tab.pending_selected_names = { childName };
    tab.pending_ensure_selection_visible = true;

    if (!tab.snapshot) return;
    for (int i = 0; i < static_cast<int>(tab.snapshot->size()); ++i) {
        const fs::DirEntry& entry = (*tab.snapshot)[static_cast<size_t>(i)];
        if (!entry.is_dir || _wcsicmp(entry.name.c_str(), childName.c_str()) != 0) continue;
        tab.SelectOnly(i);
        if (ActiveTab(s) == &tab) {
            EnsureRowVisible(s, tab, i);
            s.scrollTargetY = tab.scroll_y;
            s.scrollAnimating = false;
        }
        return;
    }
}

static void NavigateTo(AppState& s, const std::wstring& path) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    std::wstring normalized = fs::NormalizePath(path);
    const std::wstring returnedChild =
        app::NavigationReturnChildName(tab->current_path, normalized);
    tab->NavigateTo(normalized);
    StartLoadingPath(s, *tab, normalized);
    RestoreNavigationReturnSelection(s, *tab, returnedChild);
    RememberPath(s, normalized);
    s.timing.first_frame_recorded = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void FocusPane(AppState& s, app::Pane* p) {
    if (!p || p == s.pane) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (s.filterEditing) HideFilterEditor(s, true);
    for (auto& pane : s.panes) pane->focused = (pane.get() == p);
    s.pane = p;
    app::Tab* tab = p->ActiveTab();
    if (tab) {
        s.scrollTargetY = tab->scroll_y;
        s.scrollAnimating = false;
        if (s.watcher && tab && !fs::IsVirtualPath(tab->current_path)) {
            if (tab->current_path.empty()) {
                // This PC view has no directory to watch.
                s.watcher->Stop();
            } else {
            s.watcher->Start(tab->current_path, [&s]() {
                app::Tab* t = ActiveTab(s);
                if (t && !fs::IsVirtualPath(t->current_path)) {
                    s.watchLastChange.store(GetTickCount64(), std::memory_order_relaxed);
                    s.watchDirty.store(true, std::memory_order_release);
                }
            });
            }
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void ApplyLayoutPreset(AppState& s, app::LayoutPreset preset) {
    const size_t n = app::LayoutPresetCount(preset);
    std::wstring clone = L"C:\\";
    ui::ViewMode cloneView = ui::ViewMode::Details;
    std::array<float, 3> cloneColumns{};
    if (s.pane && s.pane->ActiveTab() && !s.pane->ActiveTab()->current_path.empty())
        clone = s.pane->ActiveTab()->current_path;
    if (s.pane && s.pane->ActiveTab()) {
        cloneView = s.pane->ActiveTab()->view_mode;
        cloneColumns = s.pane->ActiveTab()->details_column_dividers;
    }
    while (s.panes.size() < n) {
        auto p = std::make_unique<app::Pane>();
        p->focused = false;
        p->NewTab(clone);
        app::Tab* tab = p->ActiveTab();
        if (tab) {
            tab->view_mode = cloneView;
            tab->details_column_dividers = cloneColumns;
        }
        s.panes.push_back(std::move(p));
        if (tab) StartLoadingPath(s, *tab, clone);
    }
    std::vector<app::Pane*> used;
    used.reserve(n);
    for (size_t i = 0; i < n; ++i) used.push_back(s.panes[i].get());
    s.root = app::MakePresetTree(preset, used);
    s.layout = preset;
    bool focusOk = false;
    for (app::Pane* p : used) if (p == s.pane) focusOk = true;
    if (!focusOk && !used.empty()) FocusPane(s, used[0]);
    if (s.targetPane) {
        bool targetOk = false;
        for (app::Pane* p : used) if (p == s.targetPane) targetOk = true;
        if (!targetOk) {
            s.targetPane = nullptr;
            for (auto& p : s.panes) p->target = false;
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void TransferToTarget(AppState& s, bool move) {
    app::Tab* src = ActiveTab(s);
    if (!src || !s.targetPane || s.targetPane == s.pane) return;
    app::Tab* dst = s.targetPane->ActiveTab();
    if (!dst || dst->current_path.empty() || fs::IsVirtualPath(dst->current_path) || dst->net_readonly) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*src);
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = move ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = dst->current_path;
    req.sources = std::move(paths);
    if (move) s.cutPaths.clear();
    SubmitWithConflictResolution(s, std::move(req));
}

static void CycleFocus(AppState& s) {
    if (!s.root) return;
    std::vector<app::Pane*> vis;
    s.root->CollectPanes(vis);
    if (vis.size() < 2) return;
    int i = 0;
    for (; i < static_cast<int>(vis.size()); ++i) if (vis[static_cast<size_t>(i)] == s.pane) break;
    FocusPane(s, vis[static_cast<size_t>((i + 1) % static_cast<int>(vis.size()))]);
}

static void MarkTargetPane(AppState& s) {
    if (!s.pane) return;
    s.targetPane = s.pane;
    for (auto& p : s.panes) p->target = (p.get() == s.pane);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void ShowSplitDropdown(AppState& s) {
    if (!EnsureMenu(s)) return;
    D2D1_RECT_F addr = s.renderer.AddressBarRect((float)s.compositor.Width());
    POINT pt{ (LONG)(addr.right + 220.0f * s.scale),
              (LONG)(s.renderer.TitleBarHeight() + s.renderer.ToolbarHeight()) };
    ClientToScreen(s.hwnd, &pt);
    int cmd = s.menu->TrackPopup(pt, app::BuildSplitMenu(static_cast<int>(s.layout)));
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

// ---------------------------------------------------------------------------
// Browser-style tab groups: named + colored; strip chips open the group popup.
// ---------------------------------------------------------------------------
static const uint32_t kTabGroupPalette[] = {
    0xE74856, 0xF7630C, 0xFFB900, 0x6CCB5F,
    0x00B7C3, 0x0078D4, 0x8661C5, 0xE3008C,
};

static app::TabGroup* FindTabGroup(app::Pane& pane, int id) {
    for (auto& g : pane.tab_groups) if (g.id == id) return &g;
    return nullptr;
}

static void ShowTabGroupMenu(AppState& s, int group_id, POINT screen_pt);

// Chip click (Chromium): collapse hides member tabs; expanding restores them.
// Collapsing the group that holds the active tab moves activation to the
// nearest visible tab outside the group.
static void ToggleTabGroupCollapse(AppState& s, int group_id) {
    if (!s.pane) return;
    app::TabGroup* g = FindTabGroup(*s.pane, group_id);
    if (!g) return;
    g->collapsed = !g->collapsed;
    app::Pane& pane = *s.pane;
    if (g->collapsed && pane.active_tab < pane.tabs.size() &&
        pane.tabs[pane.active_tab]->tab_group == group_id) {
        auto visible = [&](size_t i) {
            const int tg = pane.tabs[i]->tab_group;
            if (tg == 0) return true;
            const app::TabGroup* tg_group = FindTabGroup(pane, tg);
            return !tg_group || !tg_group->collapsed;
        };
        const size_t cur = pane.active_tab;
        size_t target = pane.tabs.size();
        for (size_t i = cur + 1; i < pane.tabs.size(); ++i)
            if (visible(i)) { target = i; break; }
        if (target == pane.tabs.size())
            for (size_t i = cur; i-- > 0;)
                if (visible(i)) { target = i; break; }
        if (target < pane.tabs.size()) pane.SwitchTab(target);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Right-click an ungrouped tab: create a group on the spot (first unused
// palette color, empty name) and open the Edge-style editor so the user can
// type the group name immediately (app_main.cpp ShowTabGroupMenu).
static uint32_t FirstUnusedGroupColor(const app::Pane& pane) {
    for (uint32_t color : kTabGroupPalette) {
        bool used = false;
        for (const auto& g : pane.tab_groups)
            if (g.color_rgb == color) { used = true; break; }
        if (!used) return color;
    }
    return kTabGroupPalette[pane.tab_groups.size() % std::size(kTabGroupPalette)];
}

static void CreateTabGroupAndEdit(AppState& s, int tab_index, POINT screen_pt) {
    if (!s.pane || tab_index < 0 || tab_index >= static_cast<int>(s.pane->tabs.size()))
        return;
    app::Pane& pane = *s.pane;
    app::TabGroup group;
    group.id = pane.next_tab_group_id++;
    group.color_rgb = FirstUnusedGroupColor(pane);
    pane.tab_groups.push_back(group);
    pane.tabs[static_cast<size_t>(tab_index)]->tab_group = group.id;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    ShowTabGroupMenu(s, group.id, screen_pt);
}

// Group popup modeled on the browser: name field on top (live rename), a
// color dot row, then 在组中新建标签页 / 取消组合 / 关闭分组标签页.
static void ShowTabGroupMenu(AppState& s, int group_id, POINT screen_pt) {
    if (!s.pane || !EnsureMenu(s)) return;
    app::TabGroup* group = FindTabGroup(*s.pane, group_id);
    if (!group) return;
    const int gid = group->id;

    s.menu->SetFilterPlaceholder(L"标签组名称…");
    s.menu->SetInitialFilterText(group->name);
    s.menu->SetFilterMinWidth(260.0f);
    auto build = [&](const std::wstring& query) {
        if (app::TabGroup* g = FindTabGroup(*s.pane, gid)) {
            if (g->name != query) { // live rename while typing
                g->name = query;
                InvalidateRect(s.hwnd, nullptr, FALSE);
            }
        }
        std::vector<ui::FluentMenuItem> items;
        ui::FluentMenuItem strip;
        for (int i = 0; i < 8; ++i) {
            ui::FluentMenuSwatch sw;
            sw.command = app::CmdTabColorBase + i;
            sw.color = ui::HexColor(kTabGroupPalette[i]);
            sw.checked = group->color_rgb == kTabGroupPalette[i];
            strip.quick_swatches.push_back(sw);
        }
        strip.separator_after = true;
        items.push_back(std::move(strip));
        ui::FluentMenuItem newTab;
        newTab.command = app::CmdTabGroupNewTab;
        newTab.text = L"在组中新建标签页";
        items.push_back(std::move(newTab));
        ui::FluentMenuItem ungroup;
        ungroup.command = app::CmdTabGroupUngroup;
        ungroup.text = L"取消组合";
        items.push_back(std::move(ungroup));
        ui::FluentMenuItem closeAll;
        closeAll.command = app::CmdTabGroupClose;
        closeAll.text = L"关闭分组标签页";
        items.push_back(std::move(closeAll));
        return items;
    };
    const std::wstring initial_name = group->name;
    const int cmd = s.menu->TrackPopup(screen_pt, build(initial_name),
        [&](const std::wstring& query) { return build(query); });
    s.menu->SetFilterPlaceholder(L"搜索命令、文件夹…");

    app::Pane& pane = *s.pane;
    if (cmd >= app::CmdTabColorBase && cmd < app::CmdTabColorBase + 8) {
        if (app::TabGroup* g = FindTabGroup(pane, gid))
            g->color_rgb = kTabGroupPalette[cmd - app::CmdTabColorBase];
    } else if (cmd == app::CmdTabGroupNewTab) {
        int lastMember = -1;
        for (int i = 0; i < static_cast<int>(pane.tabs.size()); ++i)
            if (pane.tabs[static_cast<size_t>(i)]->tab_group == gid) lastMember = i;
        if (lastMember >= 0) {
            auto t = std::make_unique<app::Tab>();
            if (app::Tab* cur = pane.ActiveTab()) t->view_mode = cur->view_mode;
            t->current_path = pane.tabs[static_cast<size_t>(lastMember)]->current_path;
            t->loading = true;
            t->tab_group = gid;
            pane.tabs.insert(pane.tabs.begin() + lastMember + 1, std::move(t));
            pane.active_tab = static_cast<size_t>(lastMember + 1);
            StartLoadingPath(s, *pane.ActiveTab(), pane.ActiveTab()->current_path);
        }
    } else if (cmd == app::CmdTabGroupUngroup) {
        for (auto& t : pane.tabs) if (t->tab_group == gid) t->tab_group = 0;
        pane.tab_groups.erase(
            std::remove_if(pane.tab_groups.begin(), pane.tab_groups.end(),
                [&](const app::TabGroup& g) { return g.id == gid; }),
            pane.tab_groups.end());
    } else if (cmd == app::CmdTabGroupClose) {
        for (int i = static_cast<int>(pane.tabs.size()) - 1; i >= 0; --i)
            if (pane.tabs[static_cast<size_t>(i)]->tab_group == gid)
                pane.CloseTab(static_cast<size_t>(i));
        // CloseTab keeps one tab alive; if it was a member, detach it.
        for (auto& t : pane.tabs) if (t->tab_group == gid) t->tab_group = 0;
        pane.tab_groups.erase(
            std::remove_if(pane.tab_groups.begin(), pane.tab_groups.end(),
                [&](const app::TabGroup& g) { return g.id == gid; }),
            pane.tab_groups.end());
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Drop groups with no remaining members (after mass closes / leave operations).
static void PruneEmptyTabGroups(app::Pane& pane) {
    for (auto git = pane.tab_groups.begin(); git != pane.tab_groups.end();) {
        bool used = false;
        for (const auto& t : pane.tabs)
            if (t->tab_group == git->id) { used = true; break; }
        if (used) ++git; else git = pane.tab_groups.erase(git);
    }
}

// Chrome SetTabPinnedImpl: pin moves the tab to the end of the pinned block,
// unpin moves it to the pinned/unpinned boundary. Pinning a grouped tab
// ungroups it (pin and group do not coexist in Chromium either).
static void TogglePinTab(AppState& s, int index) {
    if (!s.pane || index < 0 || index >= static_cast<int>(s.pane->tabs.size())) return;
    app::Pane& pane = *s.pane;
    app::Tab& tab = *pane.tabs[static_cast<size_t>(index)];
    // Boundary is computed against the pre-flip state, as in Chromium.
    size_t first_unpinned = 0;
    while (first_unpinned < pane.tabs.size() && pane.tabs[first_unpinned]->pinned)
        ++first_unpinned;
    const bool pin = !tab.pinned;
    if (pin && tab.tab_group != 0) tab.tab_group = 0;
    tab.pinned = pin;
    const size_t target = pin ? first_unpinned
                              : (first_unpinned > 0 ? first_unpinned - 1 : 0);
    pane.MoveTab(static_cast<size_t>(index), target);
    PruneEmptyTabGroups(pane);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Edge-style tab context menu: tab actions + group membership.
static void ShowTabContextMenu(AppState& s, int tab_index, POINT screen_pt) {
    if (!s.pane || tab_index < 0 || tab_index >= static_cast<int>(s.pane->tabs.size()))
        return;
    if (!EnsureMenu(s)) return;
    app::Pane& pane = *s.pane;
    app::Tab& tab = *pane.tabs[static_cast<size_t>(tab_index)];
    const bool grouped = tab.tab_group != 0;

    auto item = [](int cmd, const wchar_t* text, const wchar_t* glyph) {
        ui::FluentMenuItem it;
        it.command = cmd;
        it.text = text;
        if (glyph) it.glyph = glyph;
        return it;
    };
    std::vector<ui::FluentMenuItem> items;
    items.push_back(item(app::CmdTabNewRight, L"在右侧新建标签页", L"\xE710"));
    items.push_back(item(app::CmdTabDuplicate, L"复制标签页", L"\xE8C8"));
    items.push_back(item(app::CmdTabPin,
                         tab.pinned ? L"取消固定标签页" : L"固定标签页", L"\xE718"));
    auto& pinItem = items.back();
    pinItem.separator_after = true;
    if (!grouped) {
        // No groups yet: this IS the create-group entry (opens the editor).
        items.push_back(item(app::CmdTabAddToNewGroup,
                             pane.tab_groups.empty() ? L"创建新组" : L"将标签页添加到新组",
                             nullptr));
        if (!pane.tab_groups.empty()) {
            ui::FluentMenuItem join;
            join.text = L"将标签页添加到";
            for (size_t gi = 0; gi < pane.tab_groups.size(); ++gi) {
                ui::FluentMenuItem child;
                child.command = app::CmdTabJoinGroupBase + static_cast<int>(gi);
                child.text = pane.tab_groups[gi].name.empty()
                    ? L"(未命名组)" : pane.tab_groups[gi].name;
                join.children.push_back(std::move(child));
            }
            items.push_back(std::move(join));
        }
        items.back().separator_after = true;
    } else {
        items.push_back(item(app::CmdTabRemoveFromGroup, L"从组中移除该标签页", nullptr));
        items.back().separator_after = true;
    }
    items.push_back(item(app::CmdTabClose, L"关闭标签页", L"\xE711"));
    items.back().enabled = !tab.pinned && pane.tabs.size() > 1;
    items.push_back(item(app::CmdTabCloseOthers, L"关闭其他标签页", nullptr));
    items.push_back(item(app::CmdTabCloseRight, L"关闭右侧标签页", nullptr));

    const int cmd = s.menu->TrackPopup(screen_pt, std::move(items));
    switch (cmd) {
    case app::CmdTabNewRight:
        pane.NewTabAt(static_cast<size_t>(tab_index) + 1,
                      ActiveTab(s) ? ActiveTab(s)->current_path : L"C:\\");
        StartLoadingPath(s, *pane.ActiveTab(), pane.ActiveTab()->current_path);
        break;
    case app::CmdTabDuplicate:
        pane.NewTabAt(static_cast<size_t>(tab_index) + 1, tab.current_path);
        StartLoadingPath(s, *pane.ActiveTab(), pane.ActiveTab()->current_path);
        break;
    case app::CmdTabPin:
        TogglePinTab(s, tab_index);
        return;
    case app::CmdTabAddToNewGroup:
        CreateTabGroupAndEdit(s, tab_index, screen_pt);
        return;
    case app::CmdTabRemoveFromGroup:
        tab.tab_group = 0;
        app::NormalizeGroupRuns(pane);
        PruneEmptyTabGroups(pane);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    case app::CmdTabClose:
        pane.CloseTab(static_cast<size_t>(tab_index));
        PruneEmptyTabGroups(pane);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    case app::CmdTabCloseOthers:
        for (int i = static_cast<int>(pane.tabs.size()) - 1; i >= 0; --i)
            if (i != tab_index) pane.CloseTab(static_cast<size_t>(i));
        PruneEmptyTabGroups(pane);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    case app::CmdTabCloseRight:
        for (int i = static_cast<int>(pane.tabs.size()) - 1; i > tab_index; --i)
            pane.CloseTab(static_cast<size_t>(i));
        PruneEmptyTabGroups(pane);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    default:
        break;
    }
    if (cmd >= app::CmdTabJoinGroupBase &&
        cmd < app::CmdTabJoinGroupBase + static_cast<int>(pane.tab_groups.size())) {
        tab.tab_group = pane.tab_groups[static_cast<size_t>(cmd - app::CmdTabJoinGroupBase)].id;
        app::NormalizeGroupRuns(pane);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

static void SetViewMode(AppState& s, ui::ViewMode mode) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->view_mode == mode) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    tab->view_mode = mode;
    ++tab->view_generation;
    tab->scroll_x = 0.0f;
    tab->scroll_y = 0.0f;
    s.scrollTargetY = 0.0f;
    s.scrollAnimating = false;
    if (tab->selected_index >= 0) EnsureRowVisible(s, *tab, tab->selected_index);
    ClampScroll(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void ShowViewDropdown(AppState& s, int pane_index) {
    if (!EnsureMenu(s)) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    const ui::WindowViewModel vm = BuildVm(s);
    D2D1_RECT_F paneRect = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    if (pane_index >= 0 && pane_index < static_cast<int>(vm.pane_slots.size()))
        paneRect = vm.pane_slots[static_cast<size_t>(pane_index)].rect;
    float filterExpand = 0.0f;
    if (app::Pane* p = PaneAtSlot(s, pane_index)) filterExpand = p->filter_expand;
    else if (s.pane) filterExpand = s.pane->filter_expand;
    const D2D1_RECT_F button = s.renderer.PaneViewButtonRect(paneRect, filterExpand);
    POINT anchor{ static_cast<LONG>(button.left), static_cast<LONG>(button.bottom) };
    ClientToScreen(s.hwnd, &anchor);
    const int cmd = s.menu->TrackPopup(anchor, app::BuildViewMenu(tab->view_mode, s.showDetailsPanel));
    if (cmd != app::CmdNone) DispatchMenuCommand(s, cmd);
}

static void ShowOmnibar(AppState& s, OmnibarMode mode) {
    if (!EnsureMenu(s)) {
        if (mode == OmnibarMode::Path) ShowAddressEditor(s);
        return;
    }
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.filterEditing) HideFilterEditor(s, true);

    app::Tab* tab = ActiveTab(s);
    std::wstring prefill;
    bool select_all = true;
    bool hover_first = true;
    if (mode == OmnibarMode::Command) {
        prefill = L">";
        select_all = false;
    } else if (mode == OmnibarMode::Path) {
        hover_first = false;
        if (tab) {
            prefill = ClipboardPath(tab->current_path);
            if (prefill.empty()) prefill = L"This PC";
        }
    }

    s.addressEditing = true;
    InvalidateRect(s.hwnd, nullptr, FALSE);

    D2D1_RECT_F addr = s.renderer.AddressBarRect(static_cast<float>(s.compositor.Width()));
    POINT tl{ static_cast<LONG>(std::lround(addr.left)),
              static_cast<LONG>(std::lround(addr.top)) };
    POINT br{ static_cast<LONG>(std::lround(addr.right)),
              static_cast<LONG>(std::lround(addr.bottom)) };
    ClientToScreen(s.hwnd, &tl);
    ClientToScreen(s.hwnd, &br);
    RECT anchor{ tl.x, tl.y, br.x, br.y };
    const float addr_w = std::max(1.0f, addr.right - addr.left);
    s.menu->SetAnchorRect(anchor);
    s.menu->SetFilterMinWidth(addr_w / std::max(0.01f, s.scale));
    s.menu->SetInitialFilterText(prefill);
    s.menu->SetSelectAllOnOpen(select_all);
    s.menu->SetHoverFirstOnOpen(hover_first);
    s.menu->SetFilterPlaceholder(
        mode == OmnibarMode::Command ? L"输入命令…" :
        mode == OmnibarMode::Project ? L"跳转到项目文件夹…" :
        L"路径、命令（>）或搜索（?）…");

    const bool project_only = mode == OmnibarMode::Project;
    auto rebuild = [&s, project_only](const std::wstring& query) {
        const auto parsed = app::ParseOmnibarQuery(query, project_only);
        s.paletteQuery = query;
        bool folders_only = parsed.kind == app::OmnibarQuery::Kind::Project;
        std::wstring prefix;
        if (parsed.kind == app::OmnibarQuery::Kind::Project) {
            folders_only = true;
            prefix = ProjectSearchRoot(s);
        }
        const bool run_search = parsed.kind != app::OmnibarQuery::Kind::Command &&
                                (!parsed.needle.empty() || project_only) &&
                                (parsed.kind == app::OmnibarQuery::Kind::Search ||
                                 !app::LooksLikeFilesystemPath(parsed.needle));
        if (run_search) {
            const bool same = parsed.needle == s.paletteIssuedNeedle &&
                              prefix == s.paletteIssuedPrefix &&
                              folders_only == s.paletteIssuedFolders;
            if (!same) {
                s.paletteIssuedNeedle = parsed.needle;
                s.paletteIssuedPrefix = prefix;
                s.paletteIssuedFolders = folders_only;
                s.paletteHits.clear();
                s.paletteTotal = 0;
                s.paletteSearching = true;
                index::Query q;
                q.needle = parsed.needle;
                q.path_prefix = prefix;
                q.folders_only = folders_only;
                q.limit = 24;
                q.rank = true;
                s.paletteSearchId = ++s.nextIndexReq;
                s.index.SearchAsync(q, s.paletteSearchId);
            }
        } else {
            s.paletteIssuedNeedle.clear();
            s.paletteIssuedPrefix.clear();
            s.paletteIssuedFolders = false;
            s.paletteHits.clear();
            s.paletteTotal = 0;
            s.paletteSearching = false;
        }
        const std::wstring current = ActiveTab(s) ? ActiveTab(s)->current_path : L"";
        auto items = app::BuildCommandPalette(query, s.recentPaths, s.paletteHits,
                                              project_only, s.paletteTotal, current);
        if (s.paletteSearching) {
            ui::FluentMenuItem wait;
            wait.text = L"搜索中…";
            wait.enabled = false;
            items.insert(items.begin(), std::move(wait));
        }
        if (items.empty()) {
            ui::FluentMenuItem none;
            none.text = query.empty() ? L"输入以搜索命令、文件夹…" : L"无匹配项";
            none.enabled = false;
            items.push_back(std::move(none));
        }
        return items;
    };

    POINT pt{ tl.x, br.y };
    const int cmd = s.menu->TrackPopup(pt, rebuild(prefill), rebuild, false);
    s.addressEditing = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
    if (cmd != app::CmdNone) {
        DispatchMenuCommand(s, cmd);
        return;
    }
    if (!s.menu->LastFilterCommitted()) return;
    const auto q = app::ParseOmnibarQuery(s.menu->LastFilterQuery(), project_only);
    if (q.kind == app::OmnibarQuery::Kind::Search) {
        if (!q.needle.empty()) NavigateTo(s, app::MakeSearchPath(q.needle));
        return;
    }
    if (q.kind == app::OmnibarQuery::Kind::Command) return;
    if (q.needle.empty()) return;
    const std::wstring path = FormatAddressPath(q.needle);
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (app::LooksLikeFilesystemPath(q.needle) || attrs != INVALID_FILE_ATTRIBUTES)
        NavigateTo(s, path);
}

static void SortBy(AppState& s, ui::SortColumn col) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (tab->sort_column == col) {
        tab->sort_direction = (tab->sort_direction == ui::SortDirection::Asc)
            ? ui::SortDirection::Desc : ui::SortDirection::Asc;
    } else {
        tab->sort_column = col;
        tab->sort_direction = ui::SortDirection::Asc;
    }
    RefreshActiveTab(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void OpenSelected(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    const auto indices = tab->SelectedIndices();
    if (indices.empty()) return;
    if (indices.size() == 1) {
        const fs::DirEntry& e = (*tab->snapshot)[static_cast<size_t>(indices[0])];
        if (!e.link_target.empty()) {
            if (e.link_target_is_dir) NavigateTo(s, e.link_target);
            else s.ops.OpenWith(e.link_target);
            return;
        }
        std::wstring full = EntryFullPath(*tab, indices[0]);
        if (e.is_dir) NavigateTo(s, full);
        else s.ops.OpenWith(full);
        return;
    }
    for (int index : indices) {
        const fs::DirEntry& e = (*tab->snapshot)[static_cast<size_t>(index)];
        if (!e.link_target.empty()) {
            if (!e.link_target_is_dir) s.ops.OpenWith(e.link_target);
            continue;
        }
        if (e.is_dir) continue;
        std::wstring full = EntryFullPath(*tab, index);
        if (!full.empty()) s.ops.OpenWith(full);
    }
}

static void GoUp(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (fs::IsVirtualPath(tab->current_path)) {
        if (tab->CanGoBack()) GoBack(s);
        return;
    }
    const std::wstring up = fs::ParentPath(tab->current_path);
    if (_wcsicmp(up.c_str(), tab->current_path.c_str()) != 0) NavigateTo(s, up);
    else if (!tab->current_path.empty()) NavigateTo(s, L""); // drive root -> This PC
}

static void GoBack(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->CanGoBack()) return;
    const std::wstring from = tab->current_path;
    std::wstring path = tab->GoBack();
    StartLoadingPath(s, *tab, path);
    RestoreNavigationReturnSelection(
        s, *tab, app::NavigationReturnChildName(from, path));
    s.timing.first_frame_recorded = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void GoForward(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->CanGoForward()) return;
    std::wstring path = tab->GoForward();
    StartLoadingPath(s, *tab, path);
    s.timing.first_frame_recorded = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static bool IsSettingsTab(const app::Tab* tab) {
    if (!tab) return false;
    std::wstring kind;
    return app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"settings";
}

static std::wstring NewTabPath(const AppState& s) {
    const app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr;
    if (!tab || IsSettingsTab(tab) || tab->current_path.empty())
        return s.recentPaths.empty() ? L"C:\\" : s.recentPaths.front();
    return tab->current_path;
}

static void NewTab(AppState& s, const std::wstring& path) {
    if (!s.pane) return;
    s.pane->NewTab(path.empty() ? L"C:\\" : path);
    StartLoadingPath(s, *s.pane->ActiveTab(), s.pane->ActiveTab()->current_path);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void OpenSettingsTab(AppState& s, int page) {
    const std::wstring path = app::MakeSettingsPath(page == 1 ? L"context" : L"general");
    s.settingsPage = page;
    if (s.pane) {
        for (size_t i = 0; i < s.pane->tabs.size(); ++i) {
            if (!IsSettingsTab(s.pane->tabs[i].get())) continue;
            s.pane->SwitchTab(i);
            app::Tab* tab = s.pane->ActiveTab();
            if (tab) {
                tab->current_path = path;
                tab->virtual_title = L"设置";
            }
            s.settingsScroll = 0.0f;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return;
        }
    }
    NewTab(s, path);
}

static void EnsureTrayIcon(AppState& s, bool show) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = s.hwnd;
    nid.uID = 1;
    if (show) {
        if (s.trayIconAdded) return;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_PULSE));
        if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(nid.szTip, L"Pulse");
        if (Shell_NotifyIconW(NIM_ADD, &nid) || Shell_NotifyIconW(NIM_MODIFY, &nid))
            s.trayIconAdded = true;
    } else if (s.trayIconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        s.trayIconAdded = false;
    }
}

static void HideToTray(AppState& s) {
    EnsureTrayIcon(s, true);
    ShowWindow(s.hwnd, SW_HIDE);
}

static void RestoreFromTray(AppState& s) {
    if (!s.hwnd) return;
    ShowWindow(s.hwnd, IsIconic(s.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(s.hwnd);
}

static void ApplyAppWindowChrome(AppState& s) {
    if (!s.hwnd) return;
    const auto effect = ui::WindowEffectFromId(s.appPrefs.window_effect);
    // Any selected image takes over the window base: sampled as material for
    // Mica/Acrylic, drawn as-is when the effect is None.
    const bool sample_image = !s.appPrefs.background_image.empty();
    if (sample_image) {
        ui::ApplyWindowEffect(s.hwnd, ui::WindowEffect::None, s.darkMode);
        s.backdropActive = true;
        return;
    }
    s.backdropActive = ui::ApplyWindowEffect(s.hwnd, effect, s.darkMode);
}

static bool PickImageFile(HWND owner, std::wstring& path) {
    ui::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return false;
    }
    COMDLG_FILTERSPEC filters[] = {
        { L"图片", L"*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.jfif" },
        { L"所有文件", L"*.*" },
    };
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(L"选择背景图");
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (FAILED(dialog->Show(owner))) return false;
    ui::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return false;
    PWSTR file = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &file)) || !file) return false;
    path.assign(file);
    CoTaskMemFree(file);
    return !path.empty();
}

static void HandleSettingsEffect(AppState& s, int index) {
    if (index < 0 || index >= ui::kWindowEffectCount) return;
    const auto effect = static_cast<ui::WindowEffect>(index);
    if (s.appPrefs.window_effect == ui::WindowEffectId(effect)) return;
    s.appPrefs.window_effect = ui::WindowEffectId(effect);
    s.appPrefs.Save();
    s.renderer.InvalidateWallpaper();
    ApplyAppWindowChrome(s);
}

static void HandleSettingsDensity(AppState& s, int index) {
    static constexpr int kDips[] = { 28, 34, 40 };
    if (index < 0 || index >= static_cast<int>(std::size(kDips))) return;
    if (s.appPrefs.row_height == kDips[index]) return;
    s.appPrefs.row_height = kDips[index];
    s.appPrefs.Save();
    s.renderer.SetRowHeightDip(static_cast<float>(kDips[index]));
}

static void HandleSettingsWallpaper(AppState& s, int index) {
    if (index == 0) {
        std::wstring path;
        if (!PickImageFile(s.hwnd, path)) return;
        s.appPrefs.StoreBackgroundImage(path);
        s.appPrefs.Save();
        s.renderer.InvalidateWallpaper();
        ApplyAppWindowChrome(s);
        return;
    }
    if (index == 1) {
        if (s.appPrefs.background_image.empty()) return;
        s.appPrefs.ClearBackgroundImage();
        s.appPrefs.Save();
        s.renderer.InvalidateWallpaper();
        ApplyAppWindowChrome(s);
    }
}

static void HandleSettingsToggle(AppState& s, int index) {
    static constexpr ipc::CtxMenuGroup kGroups[] = {
        ipc::CtxMenuGroup::Software, ipc::CtxMenuGroup::OpenWith,
        ipc::CtxMenuGroup::Share, ipc::CtxMenuGroup::System, ipc::CtxMenuGroup::Print
    };
    if (index == 1) {
        s.appPrefs.ApplyLaunchOnStartup(!s.appPrefs.launch_on_startup);
        s.appPrefs.Save();
        return;
    }
    if (index == 2) {
        s.appPrefs.keep_running_on_close = !s.appPrefs.keep_running_on_close;
        s.appPrefs.Save();
        EnsureTrayIcon(s, s.appPrefs.keep_running_on_close);
        return;
    }
    if (index >= 10 && index < 15) {
        const auto g = kGroups[index - 10];
        s.ctxMenuPrefs.SetGroupEnabled(g, !s.ctxMenuPrefs.GroupEnabled(g));
        s.ctxMenuPrefs.Save();
        return;
    }
    if (index >= 100) {
        const size_t i = static_cast<size_t>(index - 100);
        if (i >= s.ctxMenuPrefs.seen.size()) return;
        const auto& seen = s.ctxMenuPrefs.seen[i];
        const bool on = s.ctxMenuPrefs.ItemEnabled(seen.key, seen.category, seen.from_com);
        s.ctxMenuPrefs.SetItemEnabled(seen.key, !on);
        s.ctxMenuPrefs.Save();
    }
}

static void CloseActiveTab(AppState& s) {
    if (!s.pane) return;
    s.pane->CloseTab(s.pane->active_tab);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void SwitchTab(AppState& s, size_t idx) {
    if (!s.pane) return;
    s.pane->SwitchTab(idx);
    app::Tab* tab = ActiveTab(s);
    if (tab && !tab->loading && !tab->snapshot) {
        StartLoadingPath(s, *tab, tab->current_path);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void CollectToTray(AppState& s, bool move_intent) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    if (!paths.empty()) {
        s.tray.Collect(paths, move_intent);
        // Mirror the cut state onto the list rows (ui.md §5.2 rule 6).
        s.cutPaths = move_intent ? paths : std::vector<std::wstring>{};
        // Interop with Explorer: mirror the collection onto the system clipboard.
        std::vector<std::wstring> cbPaths;
        cbPaths.reserve(paths.size());
        for (const auto& p : paths) cbPaths.push_back(ClipboardPath(p));
        ops::WriteClipboard(cbPaths, move_intent);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

static std::wstring FormatAddressPath(const std::wstring& text) {
    std::wstring t = text;
    if (t.empty()) return L"C:\\";
    auto start = t.find_first_not_of(L" \"");
    auto end = t.find_last_not_of(L" \"");
    if (start == std::wstring::npos) return L"C:\\";
    t = t.substr(start, end - start + 1);
    if (t.size() == 2 && t[1] == L':') t += L'\\';
    return t;
}

static LRESULT CALLBACK AddressEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
static LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
static LRESULT CALLBACK TagRenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
static LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData);
static void EnsureEditVisuals(AppState& s);

static void PlaceHostedEdit(HWND hwnd, HWND owner, const D2D1_RECT_F& cell, float scale,
                            int left_margin_dip, int right_margin_dip) {
    POINT pt{ static_cast<int>(std::lround(cell.left)),
              static_cast<int>(std::lround(cell.top)) };
    ClientToScreen(owner, &pt);
    const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left)));
    const int cellH = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    int lineH = cellH;
    if (font) {
        HDC hdc = GetDC(hwnd);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
        lineH = std::max(1, static_cast<int>(tm.tmHeight));
    }
    lineH = std::min(lineH, cellH);
    const int y = pt.y + std::max(0, (cellH - lineH) / 2);
    const int left = std::max(0, static_cast<int>(std::lround(static_cast<float>(left_margin_dip) * scale)));
    const int right = std::max(0, static_cast<int>(std::lround(static_cast<float>(right_margin_dip) * scale)));
    SendMessageW(hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(left, right));
    SetWindowPos(hwnd, HWND_TOP, pt.x, y, w, lineH, SWP_NOACTIVATE);
}

static HWND CreateHostedEdit(AppState& s, SUBCLASSPROC proc) {
    EnsureEditVisuals(s);
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"EDIT", L"",
        WS_POPUP | ES_AUTOHSCROLL,
        0, 0, 0, 0, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return nullptr;
    SetWindowTheme(hwnd, L"", L"");
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)s.editFont, TRUE);
    SetWindowSubclass(hwnd, proc, 1, reinterpret_cast<DWORD_PTR>(&s));
    return hwnd;
}

static void LayoutAddressEditor(AppState& s) {
    if (!s.hwndAddressEdit || !s.hwnd) return;
    D2D1_RECT_F addr = s.renderer.AddressBarRect((float)s.compositor.Width());
    const float insetX = 10.0f * s.scale;
    const float insetY = 2.0f * s.scale;
    PlaceHostedEdit(s.hwndAddressEdit, s.hwnd,
        D2D1::RectF(addr.left + insetX, addr.top + insetY,
                    addr.right - insetX, addr.bottom - insetY),
        s.scale, 0, 0);
}

static void EnsureEditVisuals(AppState& s) {
    if (!s.editFont) {
        const int height = -std::max(14, static_cast<int>(std::lround(14.0f * s.scale)));
        s.editFont = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
        if (!s.editFont) {
            s.editFont = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        }
    }
    if (!s.editBrush) {
        s.editBrush = CreateSolidBrush(s.darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255));
    }
}

static void HideRenameOverlay(AppState& s, bool commit);

static void ShowAddressEditor(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.filterEditing) HideFilterEditor(s, true);
    s.addressEditing = true;
    if (s.hwndAddressEdit && (GetWindowLongW(s.hwndAddressEdit, GWL_STYLE) & WS_CHILD)) {
        DestroyWindow(s.hwndAddressEdit);
        s.hwndAddressEdit = nullptr;
    }
    if (!s.hwndAddressEdit) {
        s.hwndAddressEdit = CreateHostedEdit(s, AddressEditProc);
        if (!s.hwndAddressEdit) {
            s.addressEditing = false;
            return;
        }
    }
    const std::wstring shown = ClipboardPath(tab->current_path);
    s.addressIgnoreKillFocus = true;
    SetWindowTextW(s.hwndAddressEdit, shown.empty() ? L"This PC" : shown.c_str());
    LayoutAddressEditor(s);
    ShowWindow(s.hwndAddressEdit, SW_SHOW);
    SetForegroundWindow(s.hwndAddressEdit);
    SetFocus(s.hwndAddressEdit);
    SendMessageW(s.hwndAddressEdit, EM_SETSEL, 0, -1);
    s.addressIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void HideAddressEditor(AppState& s, bool navigate) {
    if (!s.hwndAddressEdit) return;
    if (navigate) {
        wchar_t buf[MAX_PATH * 4];
        GetWindowTextW(s.hwndAddressEdit, buf, ARRAYSIZE(buf));
        NavigateTo(s, FormatAddressPath(buf));
    }
    s.addressIgnoreKillFocus = true;
    ShowWindow(s.hwndAddressEdit, SW_HIDE);
    s.addressEditing = false;
    if (s.hwnd) SetFocus(s.hwnd);
    s.addressIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void LayoutFilterEditor(AppState& s) {
    if (!s.hwndFilterEdit || !s.hwnd || !s.filterEditing) return;
    const float expand = s.pane ? s.pane->filter_expand : 0.0f;
    PlaceHostedEdit(s.hwndFilterEdit, s.hwnd,
                    s.renderer.FilterEditRect(FocusedPaneRect(s), expand),
                    s.scale, 0, 0);
}

static void ShowFilterEditor(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (s.filterEditing && s.hwndFilterEdit && !s.filterFocusPending) {
        SetForegroundWindow(s.hwndFilterEdit);
        SetFocus(s.hwndFilterEdit);
        SendMessageW(s.hwndFilterEdit, EM_SETSEL, 0, -1);
        return;
    }
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.addressEditing) HideAddressEditor(s, false);
    s.filterEditing = true;
    if (!s.hwndFilterEdit) {
        s.hwndFilterEdit = CreateHostedEdit(s, FilterEditProc);
        if (!s.hwndFilterEdit) {
            s.filterEditing = false;
            return;
        }
    }
    s.filterIgnoreKillFocus = true;
    SetWindowTextW(s.hwndFilterEdit, tab->filter_text.c_str());
    ShowWindow(s.hwndFilterEdit, SW_HIDE);
    s.filterFocusPending = true;
    s.filterIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void HideFilterEditor(AppState& s, bool commit) {
    if (!s.hwndFilterEdit || !s.filterEditing) return;
    if (commit) {
        wchar_t buf[512];
        GetWindowTextW(s.hwndFilterEdit, buf, ARRAYSIZE(buf));
        if (app::Tab* tab = ActiveTab(s)) tab->filter_text = buf;
    }
    s.filterIgnoreKillFocus = true;
    ShowWindow(s.hwndFilterEdit, SW_HIDE);
    s.filterEditing = false;
    s.filterFocusPending = false;
    if (s.hwnd) SetFocus(s.hwnd);
    s.filterIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void LayoutRenameOverlay(AppState& s) {
    if (!s.hwndRenameEdit || s.renameIndex < 0 || !s.hwnd) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    ui::WindowViewModel vm = BuildVm(s);
    const D2D1_RECT_F pane = FocusedPaneRect(s);
    const D2D1_RECT_F list = s.renderer.PaneListRect(pane,
        tab->banner_message.empty() ? 0.0f : 36.0f * s.scale, tab->view_mode);
    D2D1_RECT_F field = s.renderer.RenameFieldRect(vm.pane, list, s.renameIndex);
    if (field.right <= field.left) return;
    // Seat the EDIT inside the Fluent frame: frame stroke + text padding.
    const float insetX = 3.0f * s.scale;
    const float insetY = 2.0f * s.scale;
    field.left += insetX;
    field.right -= insetX;
    field.top += insetY;
    field.bottom -= insetY;
    PlaceHostedEdit(s.hwndRenameEdit, s.hwnd, field, s.scale, 4, 4);
}

static void ShowRenameOverlay(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->selected_index < 0) return;
    if (tab->net_readonly) return;
    if (tab->selected_index >= (int)tab->snapshot->size()) return;
    if (!s.tagRenameId.empty()) HideTagRenameOverlay(s, true);
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.filterEditing) HideFilterEditor(s, true);
    s.renameIndex = tab->selected_index;
    EnsureRowVisible(s, *tab, s.renameIndex);

    if (s.hwndRenameEdit && (GetWindowLongW(s.hwndRenameEdit, GWL_STYLE) & WS_CHILD)) {
        DestroyWindow(s.hwndRenameEdit);
        s.hwndRenameEdit = nullptr;
    }
    if (!s.hwndRenameEdit) {
        s.hwndRenameEdit = CreateHostedEdit(s, RenameEditProc);
        if (!s.hwndRenameEdit) {
            s.renameIndex = -1;
            return;
        }
    }

    const std::wstring& name = (*tab->snapshot)[s.renameIndex].name;
    s.renameIgnoreKillFocus = true;
    SetWindowTextW(s.hwndRenameEdit, name.c_str());
    LayoutRenameOverlay(s);
    ShowWindow(s.hwndRenameEdit, SW_SHOW);
    SetForegroundWindow(s.hwndRenameEdit);
    SetFocus(s.hwndRenameEdit);
    int stem = (int)name.find_last_of(L'.');
    bool isDir = (*tab->snapshot)[s.renameIndex].is_dir;
    SendMessageW(s.hwndRenameEdit, EM_SETSEL, 0, (stem > 0 && !isDir) ? stem : -1);
    s.renameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void HideRenameOverlay(AppState& s, bool commit) {
    if (!s.hwndRenameEdit || s.renameIndex < 0) return;
    const int index = s.renameIndex;
    s.renameIndex = -1;
    if (commit) {
        app::Tab* tab = ActiveTab(s);
        wchar_t buf[512];
        GetWindowTextW(s.hwndRenameEdit, buf, ARRAYSIZE(buf));
        if (tab && buf[0] && index < (int)(tab->snapshot ? tab->snapshot->size() : 0)) {
            std::wstring full = tab->current_path;
            if (!full.ends_with(L"\\")) full += L"\\";
            full += (*tab->snapshot)[index].name;
            if (buf != (*tab->snapshot)[index].name) {
                ops::OpRequest req;
                req.type = ops::OpType::Rename;
                req.sources.push_back(full);
                req.new_name = buf;
                tab->pending_selected_name = buf;
                tab->pending_selected_names = { buf };
                s.ops.Submit(std::move(req));
            }
        }
    }
    s.renameIgnoreKillFocus = true;
    ShowWindow(s.hwndRenameEdit, SW_HIDE);
    if (s.hwnd) SetFocus(s.hwnd);
    s.renameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static bool TagRenameCell(AppState& s, const app::TagId& tag_id, D2D1_RECT_F& cell) {
    ui::WindowViewModel vm = BuildVm(s);
    const std::wstring path = app::MakeTagPath(tag_id);
    for (int group = 0; group < static_cast<int>(vm.sidebar.size()); ++group) {
        for (int item = 0; item < static_cast<int>(vm.sidebar[group].items.size()); ++item) {
            if (vm.sidebar[group].items[item].path != path) continue;
            if (!s.renderer.TagItemRect(vm, static_cast<float>(s.compositor.Width()),
                                        static_cast<float>(s.compositor.Height()),
                                        group, item, &cell)) {
                return false;
            }
            cell.left += 34.0f * s.scale;
            cell.right -= 38.0f * s.scale;
            cell.top += 2.0f * s.scale;
            cell.bottom -= 2.0f * s.scale;
            return cell.right > cell.left;
        }
    }
    return false;
}

static void LayoutTagRenameOverlay(AppState& s) {
    if (!s.hwndTagRenameEdit || s.tagRenameId.empty() || !s.hwnd) return;
    D2D1_RECT_F cell{};
    if (TagRenameCell(s, s.tagRenameId, cell))
        PlaceHostedEdit(s.hwndTagRenameEdit, s.hwnd, cell, s.scale, 0, 0);
}

static void ShowTagRenameOverlay(AppState& s, const app::TagId& tag_id) {
    const app::ColorTag* tag = s.places.FindTag(tag_id);
    if (!tag) return;
    if (s.renameIndex >= 0) HideRenameOverlay(s, false);
    if (s.addressEditing) HideAddressEditor(s, false);
    if (s.filterEditing) HideFilterEditor(s, true);
    s.tagRenameId = tag_id;
    if (!s.hwndTagRenameEdit)
        s.hwndTagRenameEdit = CreateHostedEdit(s, TagRenameEditProc);
    if (!s.hwndTagRenameEdit) {
        s.tagRenameId.clear();
        return;
    }
    s.tagRenameIgnoreKillFocus = true;
    SetWindowTextW(s.hwndTagRenameEdit, tag->name.c_str());
    LayoutTagRenameOverlay(s);
    ShowWindow(s.hwndTagRenameEdit, SW_SHOW);
    SetForegroundWindow(s.hwndTagRenameEdit);
    SetFocus(s.hwndTagRenameEdit);
    SendMessageW(s.hwndTagRenameEdit, EM_SETSEL, 0, -1);
    s.tagRenameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void HideTagRenameOverlay(AppState& s, bool commit) {
    if (!s.hwndTagRenameEdit || s.tagRenameId.empty()) return;
    const app::TagId tag_id = s.tagRenameId;
    if (commit) {
        wchar_t text[512]{};
        GetWindowTextW(s.hwndTagRenameEdit, text, ARRAYSIZE(text));
        const app::ColorTag* before = s.places.FindTag(tag_id);
        if (before && before->name != text) {
            const std::vector<std::wstring> affected = s.places.PathsForTag(tag_id);
            if (s.places.RenameTag(tag_id, text))
                QueueTagAds(s, BuildTagAdsUpdates(s.places, affected));
            else
                MessageBeep(MB_ICONWARNING);
        }
    }
    s.tagRenameId.clear();
    s.tagRenameIgnoreKillFocus = true;
    ShowWindow(s.hwndTagRenameEdit, SW_HIDE);
    if (s.hwnd) SetFocus(s.hwnd);
    s.tagRenameIgnoreKillFocus = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

static void UpdateProcessMetrics(AppState& s) {
    const ULONGLONG now = GetTickCount64();
    if (s.processSampleTick != 0 && now - s.processSampleTick < 500) return;

    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const uint64_t processTime = FileTimeValue(kernel) + FileTimeValue(user);
        if (s.processSampleTick != 0 && processTime >= s.lastProcessTime100ns) {
            const double wall100ns = static_cast<double>(now - s.processSampleTick) * 10000.0;
            const DWORD processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            s.processCpuPercent = 100.0 * static_cast<double>(processTime - s.lastProcessTime100ns)
                / (wall100ns * processors);
        }
        s.lastProcessTime100ns = processTime;
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        s.workingSetMb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
    s.processSampleTick = now;
}

static void Render(AppState& s) {
    auto t0 = std::chrono::steady_clock::now();

    if (s.hwnd) {
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        int w = rc.right;
        int h = rc.bottom;
        if (w != s.compositor.Width() || h != s.compositor.Height()) {
            s.compositor.Resize(w, h);
        }
    }

    ID2D1DeviceContext2* dc = s.compositor.Dc();
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    bool hc = s.shot_high_contrast || ui::IsHighContrast();
    ui::Theme theme = hc ? ui::MakeHighContrastTheme() : ui::MakeTheme(s.darkMode, s.accentColor);

    UpdateProcessMetrics(s);
    ui::WindowViewModel vm = BuildVm(s);
    vm.backdrop_enabled = !hc && s.compositor.UsesTransparentComposition() && s.backdropActive;
    vm.pane.hover_index = s.hoverRow;

    D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s.compositor.Width(), (float)s.compositor.Height());
    s.renderer.Render(vm, rect, theme);

    dc->EndDraw();
    s.compositor.Present();

    if (s.renameIndex >= 0) LayoutRenameOverlay(s);
    if (!s.tagRenameId.empty()) LayoutTagRenameOverlay(s);
    if (s.addressEditing) LayoutAddressEditor(s);
    if (s.filterEditing) LayoutFilterEditor(s);

    if (s.shot.active && !s.timing.first_frame_recorded) {
        s.timing.first_frame_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - s.shot.start).count();
        s.timing.first_frame_recorded = true;
    }

    auto t1 = std::chrono::steady_clock::now();
    s.lastFrameMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double dt = std::chrono::duration<double>(t1 - s.lastFrameTime).count();
    if (dt > 0.0) s.lastFps = 1.0 / dt;
    s.lastFrameTime = t1;
}

static LRESULT CALLBACK AddressEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            HideAddressEditor(*s, true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideAddressEditor(*s, false);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->addressIgnoreKillFocus) HideAddressEditor(*s, false);
        break;
    case WM_ERASEBKGND: {
        if (!s) break;
        EnsureEditVisuals(*s);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, s->editBrush);
        return 1;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_ESCAPE) {
            HideFilterEditor(*s, wParam == VK_RETURN);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam != VK_RETURN && wParam != VK_ESCAPE) {
            LRESULT lr = DefSubclassProc(hwnd, msg, wParam, lParam);
            if (app::Tab* tab = ActiveTab(*s)) {
                wchar_t buf[512];
                GetWindowTextW(hwnd, buf, ARRAYSIZE(buf));
                tab->filter_text = buf;
                InvalidateRect(s->hwnd, nullptr, FALSE);
            }
            return lr;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->filterIgnoreKillFocus) HideFilterEditor(*s, true);
        break;
    case WM_ERASEBKGND: {
        if (!s) break;
        EnsureEditVisuals(*s);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, s->editBrush);
        return 1;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            HideRenameOverlay(*s, true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideRenameOverlay(*s, false);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->renameIgnoreKillFocus) HideRenameOverlay(*s, true);
        break;
    case WM_ERASEBKGND: {
        if (!s) break;
        EnsureEditVisuals(*s);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, s->editBrush);
        return 1;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK TagRenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData) {
    AppState* s = reinterpret_cast<AppState*>(dwRefData);
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            HideTagRenameOverlay(*s, true);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideTagRenameOverlay(*s, false);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (!s->tagRenameIgnoreKillFocus) HideTagRenameOverlay(*s, true);
        break;
    case WM_ERASEBKGND: {
        if (!s) break;
        EnsureEditVisuals(*s);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, s->editBrush);
        return 1;
    }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void ToggleTheme(AppState& s) {
    // A title-bar toggle must always produce visible feedback. Cycling through
    // Auto first can resolve to the theme already on screen and appears to
    // require a second click.
    s.themeOverride = s.darkMode ? ui::ThemeMode::Light : ui::ThemeMode::Dark;
    s.darkMode = ui::ShouldUseDarkMode(s.themeOverride);
    ApplyAppWindowChrome(s);
    if (s.editBrush) {
        DeleteObject(s.editBrush);
        s.editBrush = nullptr;
    }
    EnsureEditVisuals(s);
    if (s.hwndAddressEdit) InvalidateRect(s.hwndAddressEdit, nullptr, TRUE);
    if (s.hwndRenameEdit) InvalidateRect(s.hwndRenameEdit, nullptr, TRUE);
    if (s.hwndTagRenameEdit) InvalidateRect(s.hwndTagRenameEdit, nullptr, TRUE);
    if (s.hwndFilterEdit) InvalidateRect(s.hwndFilterEdit, nullptr, TRUE);
    if (s.operationWindow) s.operationWindow->SetTheme(s.darkMode, s.accentColor);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

static void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog) {
    if (!s.operationWindow) return;
    const auto now = std::chrono::steady_clock::now();
    const ops::OpStatus status = s.ops.Status();
    s.operationWindow->Update(status);

    if (status.active && status.task_id != s.operationUiTaskId) {
        s.operationUiTaskId = status.task_id;
        s.operationAutoShown = false;
        s.operationStartedAt = now;
        s.operationFinishedAt = {};
    }

    if (allow_conflict_dialog) {
        if (const auto conflict = s.ops.PendingConflict();
            conflict && conflict->token != s.conflictUiToken) {
            s.conflictUiToken = conflict->token;
            const ui::ConflictDialogResult result = ui::ShowFileConflictDialog(
                s.hwnd, *conflict, s.darkMode, s.accentColor);
            s.ops.ResolveConflict(conflict->token, result.choice, result.apply_to_all);
        }
    }

    if (status.task_id != 0 && status.task_id == s.operationDismissedTaskId) {
        if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
        return;
    }

    if (status.active) {
        if (status.phase == ops::OpPhase::WaitingForConflict) return;
        // Operations that finish quickly (e.g. deleting an empty folder) never
        // surface a window; only long-running work gets the progress dialog.
        if (!s.operationAutoShown &&
            now - s.operationStartedAt >= std::chrono::milliseconds(2000)) {
            s.operationWindow->Show(false);
            s.operationAutoShown = true;
        }
        return;
    }

    if (status.phase == ops::OpPhase::Completed) {
        if (s.operationWindow->IsVisible()) {
            if (s.operationFinishedAt.time_since_epoch().count() == 0)
                s.operationFinishedAt = now;
            if (now - s.operationFinishedAt >= std::chrono::milliseconds(600))
                s.operationWindow->Hide();
        }
    } else if (status.phase == ops::OpPhase::Failed) {
        if (status.last_error == L"已取消") {
            s.operationWindow->Hide();
        } else if (!s.operationWindow->IsVisible()) {
            s.operationWindow->Show(true);
        }
    }
}

static void CrashLog(unsigned int code, const char* where, unsigned int msg = 0, void* addr = nullptr) {
    // Under %LOCALAPPDATA%\Pulse: an installed copy lives in Program Files, where a
    // relative path (or the exe directory) is not writable for a non-admin user.
    const std::wstring dir = app::GetPulseDataDir();
    if (dir.empty()) return;
    const std::wstring path = dir + L"\\pulse_crash.log";
    if (HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); f != INVALID_HANDLE_VALUE) {
        char buf[2048];
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&CrashLog), &self);
        int n = snprintf(buf, sizeof(buf),
            "crash where=%s code=0x%08X msg=0x%04X addr=%p base=%p",
            where, code, msg, addr, self);
        // Best-effort stack trace: symbol names when a PDB is nearby, else the
        // module-relative offset so the site can still be mapped offline.
        void* frames[24] = {};
        const USHORT count = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
        for (USHORT i = 0; i < count && n > 0 && n < (int)sizeof(buf) - 96; ++i) {
            n += snprintf(buf + n, sizeof(buf) - n, " #%u=%p", i, frames[i]);
        }
        if (n > 0 && n < (int)sizeof(buf) - 2) { buf[n++] = '\n'; buf[n] = 0; }
        if (n > 0) { DWORD w = 0; WriteFile(f, buf, (DWORD)n, &w, nullptr); }
        CloseHandle(f);
    }
}

static LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = GetAppState(hwnd);

    switch (msg) {
    case WM_NCCALCSIZE: {
        if (wParam && IsZoomed(hwnd)) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const UINT dpi = GetDpiForWindow(hwnd);
            const int frameX = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                             + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int frameY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                             + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            params->rgrc[0].left += frameX;
            params->rgrc[0].right -= frameX;
            params->rgrc[0].top += frameY;
            params->rgrc[0].bottom -= frameY;
        }
        return 0;
    }

    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = reinterpret_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->hwnd = hwnd;

        s->scale = s->shot_scale_override > 0.0f
            ? s->shot_scale_override : (float)GetDpiForWindow(hwnd) / 96.0f;
        s->accentColor = ui::GetAccentColor();
        s->darkMode = ui::ShouldUseDarkMode(s->themeOverride);
        s->backdropActive = ui::ApplyWindowEffect(hwnd, ui::WindowEffect::MicaAlt, s->darkMode);

        if (!s->compositor.Init(hwnd)) {
            MessageBoxW(hwnd, L"Failed to initialize D3D/D2D/DWrite", L"Pulse", MB_OK);
            return -1;
        }
        s->compositor.RecreateTextFormats(s->scale);
        s->renderer.SetCompositor(&s->compositor);
        s->renderer.SetScale(s->scale);
        s->renderer.SetIconNotifyWindow(hwnd);

        s->panes.push_back(std::make_unique<app::Pane>());
        s->pane = s->panes.back().get();
        s->pane->focused = true;
        s->root = app::SplitContainer::CreateLeaf(s->pane);
        s->sidebar = app::BuildSidebarModel();
        s->places.Load();
        s->ctxMenuPrefs.Load();
        s->appPrefs.Load();
        s->renderer.SetRowHeightDip(static_cast<float>(s->appPrefs.row_height));
        ApplyAppWindowChrome(*s);
        if (s->appPrefs.keep_running_on_close) EnsureTrayIcon(*s, true);
        s->index.Start(hwnd, WM_INDEX_NOTIFY, WM_INDEX_SEARCH);

        s->worker.Start([s](app::WorkResult res) { PostWorkerResult(*s, std::move(res)); });
        s->watcher = std::make_unique<fs::DirWatch>();

        // Ops layer: queue worker + shell host IPC; notify repaints the status bar.
        s->ops.Start([hwnd] { PostMessageW(hwnd, WM_OPS_NOTIFY, 0, 0); });
        // Explorer verbs arrive on the shell client's reader thread; hop to
        // the UI thread with an owned payload (freed by the WM handler).
        s->ops.SetShellMenuCallback([hwnd](uint32_t token,
                                           std::vector<ops::ShellMenuItem> items) {
            auto* payload = new std::vector<ops::ShellMenuItem>(std::move(items));
            if (!PostMessageW(hwnd, WM_SHELLCTX_ITEMS, token,
                              reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
        if (!s->pending_undo_json.empty()) s->ops.UndoFromJson(s->pending_undo_json);
        s->operationWindow = std::make_unique<ui::FileOperationWindow>();
        ui::FileOperationCallbacks operation_callbacks;
        operation_callbacks.cancel = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) state->ops.CancelCurrent();
        };
        operation_callbacks.pause = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) state->ops.PauseCurrent();
        };
        operation_callbacks.resume = [hwnd] {
            if (AppState* state = GetAppState(hwnd)) state->ops.ResumeCurrent();
        };
        operation_callbacks.dismiss = [hwnd] {
            if (AppState* state = GetAppState(hwnd))
                state->operationDismissedTaskId = state->ops.Status().task_id;
        };
        s->operationWindow->Create(hwnd, std::move(operation_callbacks));
        s->operationWindow->SetTheme(s->darkMode, s->accentColor);

        // OLE drop target (1B-2): list rows, breadcrumb segments, sidebar, tray.
        {
            ui::DropTargetCallbacks dcb;
            dcb.drag_over = [hwnd](const std::vector<std::wstring>& srcs, POINT pt,
                                   DWORD keys, DWORD allowed) -> DWORD {
                AppState* st = GetAppState(hwnd);
                return st ? ResolveDropTarget(*st, srcs, pt, keys, allowed) : DROPEFFECT_NONE;
            };
            dcb.drag_leave = [hwnd] {
                if (AppState* st = GetAppState(hwnd)) ClearDropFeedback(*st);
            };
            dcb.drop = [hwnd](const std::vector<std::wstring>& srcs, POINT pt,
                              DWORD keys, DWORD preferred) -> DWORD {
                AppState* st = GetAppState(hwnd);
                return st ? DropExecute(*st, srcs, pt, keys, preferred) : DROPEFFECT_NONE;
            };
            s->dropTarget = new ui::WindowDropTarget(hwnd, std::move(dcb));
            RegisterDragDrop(hwnd, s->dropTarget);
        }

        if (!s->shot.active && !s->session_pane_tabs.empty()) {
            // Version-5 session: rebuild every tab and group in every pane.
            if (s->session_layout > 0)
                ApplyLayoutPreset(*s, static_cast<app::LayoutPreset>(s->session_layout));
            for (size_t i = 0; i < s->panes.size() && i < s->session_pane_tabs.size(); ++i)
                RestorePaneTabs(*s, *s->panes[i], s->session_pane_tabs[i]);
            if (!s->session_path.empty())
                RememberPath(*s, s->session_path);
            else if (app::Tab* t = ActiveTab(*s))
                RememberPath(*s, t->current_path);
            if (s->session_focused >= 0 &&
                s->session_focused < static_cast<int>(s->panes.size())) {
                FocusPane(*s, s->panes[static_cast<size_t>(s->session_focused)].get());
            }
            if (s->session_target >= 0 &&
                s->session_target < static_cast<int>(s->panes.size())) {
                s->targetPane = s->panes[static_cast<size_t>(s->session_target)].get();
                for (auto& p : s->panes) p->target = (p.get() == s->targetPane);
            }
        } else {
        std::wstring startPath = s->shot.active ? s->shot.path : L"C:\\";
        if (!s->shot.active && !s->session_path.empty()) startPath = s->session_path;
        s->pane->NewTab(startPath);
        if (s->shot.active) s->pane->ActiveTab()->view_mode = s->shot.view_mode;
        if (!s->shot.active && !s->session_pane_views.empty())
            s->pane->ActiveTab()->view_mode = s->session_pane_views[0];
        if (!s->shot.active && !s->session_pane_columns.empty())
            s->pane->ActiveTab()->details_column_dividers = s->session_pane_columns[0];
        StartLoadingPath(*s, *s->pane->ActiveTab(), startPath);
        RememberPath(*s, startPath);

        if (!s->shot.active && s->session_layout > 0) {
            ApplyLayoutPreset(*s, static_cast<app::LayoutPreset>(s->session_layout));
            for (size_t i = 0; i < s->panes.size() && i < s->session_pane_paths.size(); ++i) {
                const std::wstring& pth = s->session_pane_paths[i];
                if (pth.empty()) continue;
                app::Tab* t = s->panes[i]->ActiveTab();
                if (!t) {
                    s->panes[i]->NewTab(pth);
                    t = s->panes[i]->ActiveTab();
                }
                if (t && i < s->session_pane_views.size())
                    t->view_mode = s->session_pane_views[i];
                if (t && i < s->session_pane_columns.size())
                    t->details_column_dividers = s->session_pane_columns[i];
                if (t && t->current_path != fs::NormalizePath(pth))
                    StartLoadingPath(*s, *t, pth);
            }
            if (s->session_focused >= 0 &&
                s->session_focused < static_cast<int>(s->panes.size())) {
                FocusPane(*s, s->panes[static_cast<size_t>(s->session_focused)].get());
            }
            if (s->session_target >= 0 &&
                s->session_target < static_cast<int>(s->panes.size())) {
                s->targetPane = s->panes[static_cast<size_t>(s->session_target)].get();
                for (auto& p : s->panes) p->target = (p.get() == s->targetPane);
            }
        }
        }

        s->lastFrameTime = std::chrono::steady_clock::now();
        s->renderer.SetDetailsPanelVisible(s->showDetailsPanel);
        s->renderer.SetDetailsPanelWidth(s->detailsPanelWidth);
        SetTimer(hwnd, kTimerUi, 16, nullptr);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
            SWP_NOACTIVATE | SWP_FRAMECHANGED);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        // Arrives before WM_CREATE; GWLP_USERDATA is not set yet.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        float sc = s ? s->scale : 1.0f;
        mmi->ptMinTrackSize.x = (LONG)(640 * sc);
        mmi->ptMinTrackSize.y = (LONG)(420 * sc);
        return 0;
    }

    case WM_NCHITTEST: {
        POINT screenPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (!IsZoomed(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const UINT dpi = GetDpiForWindow(hwnd);
            const int frameX = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                             + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int frameY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                             + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const bool left = screenPt.x >= wr.left && screenPt.x < wr.left + frameX;
            const bool right = screenPt.x < wr.right && screenPt.x >= wr.right - frameX;
            const bool top = screenPt.y >= wr.top && screenPt.y < wr.top + frameY;
            const bool bottom = screenPt.y < wr.bottom && screenPt.y >= wr.bottom - frameY;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        if (!s) break;
        POINT pt = screenPt;
        ScreenToClient(hwnd, &pt);
        float x = (float)pt.x;
        float y = (float)pt.y;
        float tbH = s->renderer.TitleBarHeight();
        if (y >= 0 && y < tbH) {
            D2D1_RECT_F bounds = D2D1::RectF(0, 0,
                (float)s->compositor.Width(), (float)s->compositor.Height());
            ui::HitTestResult hit = s->renderer.HitTest(BuildVm(*s), bounds, x, y);
            if (hit.region == ui::HitTestResult::Maximize) return HTMAXBUTTON;
            if (hit.region == ui::HitTestResult::Minimize) return HTMINBUTTON;
            if (hit.region == ui::HitTestResult::Close) return HTCLOSE;
            if (hit.region != ui::HitTestResult::None) return HTCLIENT;
            return HTCAPTION;
        }
        break;
    }

    case WM_NCMOUSEMOVE: {
        if (!s) break;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(BuildVm(*s), bounds,
            (float)pt.x, (float)pt.y);
        s->hoverPoint = pt;
        const int region = static_cast<int>(hit.region);
        if (region != s->hoverRegion || hit.index != s->hoverControlIndex) {
            s->hoverRegion = region;
            s->hoverControlIndex = hit.index;
            s->hoverSubIndex = hit.sub_index;
            s->hoverSince = GetTickCount64();
            s->tooltipText.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }

    case WM_NCMOUSELEAVE:
        if (s) {
            s->hoverRegion = 0;
            s->hoverControlIndex = -1;
            s->hoverSubIndex = -1;
            s->hoverSince = 0;
            s->tooltipText.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_NCLBUTTONDOWN: {
        if (!s) break;
        if (wParam == HTMINBUTTON) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }
        if (wParam == HTMAXBUTTON) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        if (wParam == HTCLOSE) {
            if (s->appPrefs.keep_running_on_close) HideToTray(*s);
            else DestroyWindow(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE: {
        if (s && s->appPrefs.keep_running_on_close) {
            HideToTray(*s);
            return 0;
        }
        break;
    }

    case WM_TRAYICON: {
        if (!s) return 0;
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            RestoreFromTray(*s);
            break;
        case WM_RBUTTONUP: {
            POINT pt{};
            GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            if (menu) {
                AppendMenuW(menu, MF_STRING, 1, L"打开");
                AppendMenuW(menu, MF_STRING, 2, L"退出");
                SetForegroundWindow(hwnd);
                const int cmd = TrackPopupMenu(menu,
                    TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                    pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                if (cmd == 1) RestoreFromTray(*s);
                else if (cmd == 2) DestroyWindow(hwnd);
            }
            break;
        }
        }
        return 0;
    }

    case WM_DPICHANGED: {
        s->scale = (float)HIWORD(wParam) / 96.0f;
        RECT* rc = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
            rc->right - rc->left, rc->bottom - rc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        s->compositor.RecreateTextFormats(s->scale);
        s->renderer.SetScale(s->scale);
        if (s->editFont) {
            DeleteObject(s->editFont);
            s->editFont = nullptr;
        }
        EnsureEditVisuals(*s);
        if (s->hwndAddressEdit) SendMessageW(s->hwndAddressEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->hwndRenameEdit) SendMessageW(s->hwndRenameEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->hwndTagRenameEdit) SendMessageW(s->hwndTagRenameEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->addressEditing) LayoutAddressEditor(*s);
        if (!s->tagRenameId.empty()) LayoutTagRenameOverlay(*s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGCHANGE:
        if (s && (lParam == 0 ||
                  wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0 ||
                  wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"HighContrast") == 0)) {
            if (s->themeOverride == ui::ThemeMode::Auto)
                s->darkMode = ui::ShouldUseDarkMode(s->themeOverride);
            ApplyAppWindowChrome(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_ACTIVATE:
        if (s) s->renderer.NotifyPreviewActivate(LOWORD(wParam) != WA_INACTIVE);
        break;

    case WM_ACTIVATEAPP:
        if (s) s->renderer.NotifyPreviewActivate(wParam != 0);
        return 0;

    case WM_MOVE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SIZE: {
        if (s) {
            s->compositor.Resize(LOWORD(lParam), HIWORD(lParam));
            s->maximized = (wParam == SIZE_MAXIMIZED);
            if (s->addressEditing) LayoutAddressEditor(*s);
            if (s->filterEditing && !s->filterFocusPending) LayoutFilterEditor(*s);
            if (!s->tagRenameId.empty()) LayoutTagRenameOverlay(*s);
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (s) Render(*s);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT: {
        if (!s) break;
        EnsureEditVisuals(*s);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, s->darkMode ? RGB(255, 255, 255) : RGB(26, 26, 26));
        SetBkColor(hdc, s->darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255));
        SetBkMode(hdc, OPAQUE);
        return reinterpret_cast<LRESULT>(s->editBrush);
    }

    case WM_TIMER: {
        if (s && wParam == kTimerUi) {
            bool dirty = false;
            if (s->watchDirty.load(std::memory_order_acquire)) {
                const ULONGLONG now = GetTickCount64();
                if (now - s->watchLastChange.load(std::memory_order_relaxed) >= 150 &&
                    s->watchDirty.exchange(false, std::memory_order_acq_rel)) {
                    const ULONGLONG latest = s->watchLastChange.load(std::memory_order_relaxed);
                    if (now - latest >= 150) {
                        if (app::Tab* tab = ActiveTab(*s)) s->store.MarkDirty(tab->current_path);
                        RefreshActiveTab(*s);
                        dirty = true;
                    } else {
                        s->watchDirty.store(true, std::memory_order_release);
                    }
                }
            }
            const ULONGLONG now = GetTickCount64();
            if (s->shellRefreshAt && now >= s->shellRefreshAt) {
                s->shellRefreshAt = 0;
                RefreshActiveTab(*s);
                dirty = true;
            }
            if (s->shellRefreshAgainAt && now >= s->shellRefreshAgainAt) {
                s->shellRefreshAgainAt = 0;
                RefreshActiveTab(*s);
                dirty = true;
            }
            if (s->renameClickCandidate && s->renameClickDue != 0 &&
                now >= s->renameClickDue) {
                app::Tab* tab = ActiveTab(*s);
                const int index = s->renameClickIndex;
                const bool valid = s->pane == s->renameClickPane &&
                    tab == s->renameClickTab && tab &&
                    tab->SelectedCount() == 1 && tab->selected_index == index &&
                    index >= 0 && index < tab->CountBound() &&
                    (s->renameClickPath.empty() ||
                     EntryFullPath(*tab, index) == s->renameClickPath) &&
                    s->renameIndex < 0 && s->tagRenameId.empty() &&
                    !s->addressEditing && !s->filterEditing &&
                    !s->dragPending && !s->marqueePending && !s->marqueeActive &&
                    !s->scrollbarDragging && !s->splitterDragging;
                CancelRenameClick(*s);
                if (valid) ShowRenameOverlay(*s);
            }
            // Search affordance: expand only the focused pane, then reveal
            // the hosted edit once it has enough room for stable text layout.
            for (auto& pane : s->panes) {
                const float filterTarget =
                    (s->filterEditing && pane.get() == s->pane) ? 1.0f : 0.0f;
                const float filterStep = (filterTarget - pane->filter_expand) * 0.24f;
                if (std::abs(filterStep) > 0.008f) {
                    pane->filter_expand += filterStep;
                    dirty = true;
                } else if (pane->filter_expand != filterTarget) {
                    pane->filter_expand = filterTarget;
                    dirty = true;
                }
            }
            const float focusedExpand = s->pane ? s->pane->filter_expand : 0.0f;
            if (s->filterFocusPending && focusedExpand >= 0.985f &&
                s->hwndFilterEdit) {
                if (s->pane) s->pane->filter_expand = 1.0f;
                LayoutFilterEditor(*s);
                s->filterIgnoreKillFocus = true;
                ShowWindow(s->hwndFilterEdit, SW_SHOW);
                SetForegroundWindow(s->hwndFilterEdit);
                SetFocus(s->hwndFilterEdit);
                SendMessageW(s->hwndFilterEdit, EM_SETSEL, 0, -1);
                s->filterFocusPending = false;
                s->filterIgnoreKillFocus = false;
                dirty = true;
            }
            // Scrollbar hover expansion.
            float target = s->scrollbarHovered ? (s->renderer.Margin() * 1.5f - 6.0f * s->scale) : 0.0f;
            target = std::max(0.0f, target);
            float step = (target - s->scrollbarHoverWidth) * 0.25f;
            if (std::abs(step) > 0.1f) {
                s->scrollbarHoverWidth += step;
                dirty = true;
            } else if (s->scrollbarHoverWidth != target) {
                s->scrollbarHoverWidth = target;
                dirty = true;
            }
            // Smooth scroll.
            if (s->scrollAnimating) {
                UpdateSmoothScroll(*s);
                dirty = true;
            }
            // Tag slide animation.
            if (!s->tagTracks.empty()) {
                TickTagTransitions(*s);
                dirty = true;
            }
            if (!s->tabTracks.empty() || !s->chipTracks.empty()) {
                TickTabTransitions(*s);
                dirty = true;
            }
            if (TickTrayDeck(*s)) dirty = true;
            if (s->hoverRegion != 0 && s->tooltipText.empty() && s->hoverSince != 0 &&
                GetTickCount64() - s->hoverSince >= 400) {
                s->tooltipText = TooltipForHover(*s);
                dirty = !s->tooltipText.empty() || dirty;
            }
            QueueVisibleTagDiscovery(*s);
            UpdateOperationWindow(*s, false);
            if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if (!s || LOWORD(lParam) != HTCLIENT) break;
        if (s->tagDragActive || s->tabDragging) { // QFluent TabBar grabs a drag cursor while reordering
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, bounds, (float)pt.x, (float)pt.y);
        if (s->detailsPanelResizing || hit.region == ui::HitTestResult::DetailsResize) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        if (s->detailsPreviewPanning || hit.region == ui::HitTestResult::DetailsPreview) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        if (s->columnResizing || hit.region == ui::HitTestResult::ColumnDivider) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        const bool splitter = s->splitterDragging || hit.region == ui::HitTestResult::Splitter;
        if (splitter) {
            const bool vertical = s->splitterDragging
                ? (s->splitterOrientation == app::SplitOrientation::Vertical)
                : (hit.index >= 0 && hit.index < static_cast<int>(vm.splitters.size()) &&
                   vm.splitters[static_cast<size_t>(hit.index)].vertical);
            SetCursor(LoadCursorW(nullptr, vertical ? IDC_SIZEWE : IDC_SIZENS));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::AddressBar || s->addressEditing) {
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            return TRUE;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        if (!s) break;
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        s->hoverPoint = POINT{ mx, my };

        if (s->columnResizing) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->columnResizing = false;
                s->columnResizeIndex = -1;
                s->columnResizePane = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
            } else {
                ui::WindowViewModel resizeVm = BuildVm(*s);
                D2D1_RECT_F paneRect = s->renderer.ContentRect(
                    static_cast<float>(s->compositor.Width()),
                    static_cast<float>(s->compositor.Height()));
                if (s->columnResizePane >= 0 &&
                    s->columnResizePane < static_cast<int>(resizeVm.pane_slots.size())) {
                    paneRect = resizeVm.pane_slots[
                        static_cast<size_t>(s->columnResizePane)].rect;
                }
                app::Pane* resizePane = PaneAtSlot(*s, s->columnResizePane);
                app::Tab* resizeTab = resizePane ? resizePane->ActiveTab() : nullptr;
                if (resizeTab) {
                    resizeTab->details_column_dividers =
                        s->renderer.ResizeDetailsColumnDivider(
                            paneRect, resizeTab->details_column_dividers,
                            s->columnResizeIndex, static_cast<float>(mx));
                }
                s->hoverRegion = static_cast<int>(ui::HitTestResult::ColumnDivider);
                s->hoverControlIndex = s->columnResizeIndex;
                s->hoverPaneIndex = s->columnResizePane;
                if (s->renameIndex >= 0) LayoutRenameOverlay(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (s->detailsPanelResizing) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->detailsPanelResizing = false;
                if (GetCapture() == hwnd) ReleaseCapture();
            } else {
                const float width = (static_cast<float>(s->compositor.Width())
                    - s->renderer.Margin() - static_cast<float>(mx)) / s->scale;
                s->detailsPanelWidth = std::clamp(width, 300.0f, 480.0f);
                s->renderer.SetDetailsPanelWidth(s->detailsPanelWidth);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (s->detailsPreviewPanning) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->detailsPreviewPanning = false;
                if (GetCapture() == hwnd) ReleaseCapture();
            } else {
                const float dx = static_cast<float>(mx - s->detailsPreviewPanLast.x) / s->scale;
                const float dy = static_cast<float>(my - s->detailsPreviewPanLast.y) / s->scale;
                s->detailsPreviewPanLast = POINT{ mx, my };
                const float maxX = s->renderer.DetailsCoverMaxPanX();
                const float maxY = s->renderer.DetailsCoverMaxPanY();
                s->detailsPreviewPanX = std::clamp(s->detailsPreviewPanX - dx, 0.0f, maxX);
                s->detailsPreviewPanY = std::clamp(s->detailsPreviewPanY - dy, 0.0f, maxY);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (s->splitterDragging) {
            if (UpdateSplitterDrag(*s, mx, my)) {
                if (!s->splitterDragging && GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }

        if (s->scrollbarDragging) {
            app::Tab* tab = ActiveTab(*s);
            if (!tab || (GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->scrollbarDragging = false;
                s->scrollbarHorizontal = false;
                if (GetCapture() == hwnd) ReleaseCapture();
            } else {
                ui::WindowViewModel dragVm = BuildVm(*s);
                D2D1_RECT_F track{}, thumb{};
                float maxScroll = 0.0f;
                if (s->scrollbarHorizontal &&
                    HorizontalScrollbarGeometry(*s, dragVm.pane, track, thumb, maxScroll)) {
                    const float travel = std::max(1.0f,
                        (track.right - track.left) - (thumb.right - thumb.left));
                    tab->scroll_x = std::clamp(s->scrollbarDragStartScroll +
                        (mx - s->scrollbarDragStartX) * maxScroll / travel, 0.0f, maxScroll);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (!s->scrollbarHorizontal &&
                           ScrollbarGeometry(*s, dragVm.pane, track, thumb, maxScroll)) {
                    const float travel = std::max(1.0f,
                        (track.bottom - track.top) - (thumb.bottom - thumb.top));
                    tab->scroll_y = std::clamp(
                        s->scrollbarDragStartScroll
                            + (my - s->scrollbarDragStartY) * maxScroll / travel,
                        0.0f, maxScroll);
                    s->scrollTargetY = tab->scroll_y;
                    MaybePrefetchSearchPage(*s);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }

        if (s->tabDragPending) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->tabDragPending = false;
                s->tabDragging = false;
                s->tabDragIndex = -1;
                s->tabDragRunPos = 0;
                s->tabDragRunLen = 1;
                s->tabDragFromChip = false;
                s->tabDragGroupId = 0;
                s->tabDragSlots = 1.0f;
                s->tabOrder.clear();
                if (GetCapture() == hwnd) ReleaseCapture();
            } else if (s->tabDragging ||
                       (s->pane && s->pane->tabs.size() >= 2 &&
                        std::abs(mx - s->tabDragStartPt.x) >= 2)) {
                if (!s->tabDragging && s->pane &&
                    s->tabDragIndex >= 0 &&
                    s->tabDragIndex < static_cast<int>(s->pane->tabs.size())) {
                    ui::WindowViewModel vm0 = BuildVm(*s);
                    const float w0 = static_cast<float>(s->compositor.Width());
                    const int n0 = static_cast<int>(vm0.tabs.size());
                    D2D1_RECT_F first{}, last{}, self{};
                    bool haveEnds = false;
                    for (int i = 0; i < n0; ++i) {
                        D2D1_RECT_F rc{};
                        if (!s->renderer.TabItemRect(vm0, w0, i, &rc)) continue; // hidden
                        if (!haveEnds) first = rc;
                        last = rc;
                        haveEnds = true;
                    }
                    bool selfOk = s->renderer.TabItemRect(vm0, w0, s->tabDragIndex, &self);
                    bool chipBlock = false;
                    if (!selfOk && s->tabDragFromChip) {
                        // Collapsed group: the chip itself is the drag block.
                        for (int gi = 0; gi < static_cast<int>(vm0.tab_groups.size()); ++gi) {
                            if (vm0.tab_groups[static_cast<size_t>(gi)].id == s->tabDragGroupId &&
                                s->renderer.TabGroupChipRect(vm0, w0, gi, &self)) {
                                selfOk = true;
                                chipBlock = true;
                                break;
                            }
                        }
                    }
                    if (n0 >= 2 && n0 == static_cast<int>(s->pane->tabs.size()) &&
                        haveEnds && selfOk) {
                        s->tabDragging = true;
                        s->tabFlowLeft = first.left;
                        s->tabFlowRight = last.right;
                        s->tabSlotW = self.right - self.left;
                        // Chrome ConstrainMoveIndex: pinned and unpinned tabs
                        // each stay within their own region while dragging.
                        {
                            const bool dragPinned = s->tabDragFromChip
                                ? false // groups never contain pinned tabs
                                : s->pane->tabs[static_cast<size_t>(s->tabDragIndex)]->pinned;
                            int firstUnpinned = -1;
                            for (int i = 0; i < n0; ++i)
                                if (!s->pane->tabs[static_cast<size_t>(i)]->pinned) {
                                    firstUnpinned = i;
                                    break;
                                }
                            if (firstUnpinned > 0) {
                                D2D1_RECT_F b{};
                                if (s->renderer.TabItemRect(vm0, w0, firstUnpinned, &b)) {
                                    if (dragPinned) s->tabFlowRight = b.left;
                                    else s->tabFlowLeft = b.left;
                                }
                            }
                        }
                        // Uniform pitch excludes group-chip offsets; rest
                        // rects come from TabItemRect where chips matter.
                        s->tabPitch = s->renderer.TabPitchPx(vm0, w0);
                        s->tabDragPressLeft = self.left;
                        s->tabDragFloatLeft = std::clamp(
                            self.left + static_cast<float>(mx - s->tabDragStartPt.x),
                            first.left, last.left);
                        s->tabDragLastX = mx;
                        s->tabTracks.clear();
                        s->tabOffsets.clear();
                        s->chipTracks.clear();
                        s->chipOffsets.clear();
                        s->tabOrder.resize(static_cast<size_t>(n0));
                        for (int i = 0; i < n0; ++i) s->tabOrder[static_cast<size_t>(i)] = i;
                        // Whole-group drags start from the chip; member tabs
                        // always drag solo (Chromium semantics).
                        s->tabDragRunPos = s->tabDragIndex;
                        s->tabDragRunLen = 1;
                        s->tabDragSlots = 1.0f;
                        s->tabDragBlockW = 0.0f;
                        if (s->tabDragFromChip) {
                            const int dragGroup =
                                s->pane->tabs[static_cast<size_t>(s->tabDragIndex)]->tab_group;
                            int pos = s->tabDragIndex;
                            while (pos > 0 &&
                                   s->pane->tabs[static_cast<size_t>(pos - 1)]->tab_group == dragGroup)
                                --pos;
                            int end = s->tabDragIndex;
                            while (end + 1 < n0 &&
                                   s->pane->tabs[static_cast<size_t>(end + 1)]->tab_group == dragGroup)
                                ++end;
                            s->tabDragRunPos = pos;
                            s->tabDragRunLen = end - pos + 1;
                            if (chipBlock) {
                                s->tabDragSlots = s->tabPitch > 0.0f
                                    ? (self.right - self.left) / s->tabPitch : 1.0f;
                                // Collapsed group: the chip alone is the drag
                                // block; its width is px, not slots.
                                s->tabDragBlockW = app::CollapsedChipBlockW(
                                    self.right - self.left, 4.0f * s->scale);
                            } else {
                                s->tabDragSlots = static_cast<float>(s->tabDragRunLen);
                                if (s->tabDragRunLen > 1) {
                                    D2D1_RECT_F runRc{};
                                    if (s->renderer.TabItemRect(vm0, w0, pos, &runRc))
                                        s->tabDragPressLeft = runRc.left;
                                }
                            }
                        }
                    } else {
                        s->tabDragPending = false; // geometry unavailable: plain click
                    }
                }
                if (s->tabDragging) {
                    const int dx = mx - s->tabDragLastX;
                    // Collapsed chip drags clamp by the px block width; slot
                    // math only holds for tab-sized blocks.
                    const float dragBlockW = s->tabDragBlockW > 0.0f
                        ? s->tabDragBlockW : s->tabSlotW * s->tabDragSlots;
                    s->tabDragFloatLeft = std::clamp(
                        s->tabDragPressLeft + static_cast<float>(mx - s->tabDragStartPt.x),
                        s->tabFlowLeft,
                        std::max(s->tabFlowLeft, s->tabFlowRight - dragBlockW));
                    if (std::abs(dx) >= 2 && s->tabDragFromChip) {
                        // Whole-group drag: rotate the run as a block when its
                        // leading/trailing edge crosses the neighbor's center.
                        s->tabDragLastX = mx;
                        const int n = static_cast<int>(s->tabOrder.size());
                        const int runPos = s->tabDragRunPos;
                        const int runLen = s->tabDragRunLen;
                        int blockShift = 0; // -1 left, +1 right, 0 none
                        int siblingPos = -1;
                        if (dx < 0 && runPos > 0) siblingPos = runPos - 1;
                        else if (dx > 0 && runPos + runLen < n) siblingPos = runPos + runLen;
                        if (siblingPos >= 0 && s->tabDragBlockW > 0.0f) {
                            // Collapsed group: the chip alone is the drag
                            // block, so all geometry here is px-based (chips
                            // don't occupy whole slots).
                            const int adjIdx = s->tabOrder[static_cast<size_t>(siblingPos)];
                            const app::Tab* adjTab =
                                (adjIdx >= 0 && adjIdx < static_cast<int>(s->pane->tabs.size()))
                                    ? s->pane->tabs[static_cast<size_t>(adjIdx)].get() : nullptr;
                            ui::WindowViewModel vmNow = BuildVm(*s);
                            const float ww = static_cast<float>(s->compositor.Width());
                            float adjRest = 0.0f; // neighbor rest left (px)
                            float adjCur = 0.0f;  // neighbor slide offset (px)
                            float adjW = 0.0f;    // neighbor visual width (px)
                            int adjChipGi = -1;   // vm.tab_groups index when the
                                                  // neighbor is another chip
                            int adjChipGid = 0;   // app::TabGroup::id of it
                            D2D1_RECT_F adjRc{};
                            if (s->renderer.TabItemRect(vmNow, ww, siblingPos, &adjRc)) {
                                adjRest = adjRc.left;
                                adjW = adjRc.right - adjRc.left;
                                if (adjTab) {
                                    const auto oit = s->tabOffsets.find(adjTab);
                                    if (oit != s->tabOffsets.end())
                                        adjCur = oit->second * s->tabPitch;
                                }
                            } else if (adjTab && adjTab->tab_group != 0) {
                                // Hidden member of another collapsed group:
                                // measure and animate that group's chip.
                                for (int gi = 0;
                                     gi < static_cast<int>(vmNow.tab_groups.size()); ++gi) {
                                    if (vmNow.tab_groups[static_cast<size_t>(gi)].id
                                            != adjTab->tab_group)
                                        continue;
                                    D2D1_RECT_F chipRc{};
                                    if (s->renderer.TabGroupChipRect(vmNow, ww, gi, &chipRc)) {
                                        adjChipGi = gi;
                                        adjChipGid = adjTab->tab_group;
                                        adjRest = chipRc.left;
                                        adjW = chipRc.right - chipRc.left;
                                        const auto oit = s->chipOffsets.find(adjChipGid);
                                        if (oit != s->chipOffsets.end()) adjCur = oit->second;
                                    }
                                    break;
                                }
                            }
                            if (adjTab && adjW > 0.0f) {
                                const float adjCenter = adjRest + adjCur + adjW * 0.5f;
                                if (app::ChipBlockCrossed(s->tabDragFloatLeft,
                                        s->tabDragBlockW, adjCenter, dx))
                                    blockShift = dx < 0 ? -1 : 1;
                            }
                            if (blockShift != 0 && adjTab) {
                                s->tabDragRunPos = app::MoveTabRun(
                                    s->tabOrder, runPos, runLen, blockShift);
                                // Measure the displaced unit's NEW rest with
                                // the post-move order; the track covers the
                                // difference from where it visibly sits now.
                                ui::WindowViewModel vmAfter = BuildVm(*s);
                                float newRest = 0.0f;
                                bool haveNew = false;
                                D2D1_RECT_F newRc{};
                                if (adjChipGi >= 0) {
                                    // Group index is stable: pane.tab_groups
                                    // order does not change with tab order.
                                    haveNew = s->renderer.TabGroupChipRect(
                                        vmAfter, ww, adjChipGi, &newRc);
                                } else {
                                    haveNew = s->renderer.TabItemRect(vmAfter, ww,
                                        siblingPos - blockShift * runLen, &newRc);
                                }
                                if (haveNew) newRest = newRc.left;
                                if (haveNew) {
                                    const float startPx = app::DisplacedRestDelta(
                                        adjRest, adjCur, newRest);
                                    if (adjChipGi >= 0) {
                                        if (std::abs(startPx) >= 1.0f) {
                                            s->chipTracks[adjChipGid] = AppState::TabTrack{
                                                startPx, 150, std::chrono::steady_clock::now() };
                                            s->chipOffsets[adjChipGid] = startPx;
                                        }
                                    } else if (s->tabPitch > 0.0f) {
                                        const float start = startPx / s->tabPitch;
                                        if (std::abs(start) >= 0.01f) {
                                            s->tabTracks[adjTab] = AppState::TabTrack{
                                                start, 150, std::chrono::steady_clock::now() };
                                            s->tabOffsets[adjTab] = start;
                                        }
                                    }
                                }
                            }
                        } else if (siblingPos >= 0) {
                            const int adjIdx = s->tabOrder[static_cast<size_t>(siblingPos)];
                            const app::Tab* adjTab =
                                (adjIdx >= 0 && adjIdx < static_cast<int>(s->pane->tabs.size()))
                                    ? s->pane->tabs[static_cast<size_t>(adjIdx)].get() : nullptr;
                            const float adjOff = [&]() {
                                if (!adjTab) return 0.0f;
                                const auto oit = s->tabOffsets.find(adjTab);
                                return oit != s->tabOffsets.end() ? oit->second : 0.0f;
                            }();
                            float adjRest = s->tabFlowLeft
                                + static_cast<float>(siblingPos) * s->tabPitch;
                            {
                                ui::WindowViewModel vmNow = BuildVm(*s);
                                D2D1_RECT_F adjRc{};
                                if (s->renderer.TabItemRect(vmNow,
                                        static_cast<float>(s->compositor.Width()),
                                        siblingPos, &adjRc))
                                    adjRest = adjRc.left;
                            }
                            const float adjCenter = adjRest + adjOff * s->tabPitch
                                + s->tabSlotW * 0.5f;
                            if (dx < 0 && s->tabDragFloatLeft < adjCenter) blockShift = -1;
                            else if (dx > 0 && s->tabDragFloatLeft
                                     + s->tabDragSlots * s->tabSlotW > adjCenter)
                                blockShift = 1;
                            if (blockShift != 0 && adjTab) {
                                s->tabDragRunPos = app::MoveTabRun(
                                    s->tabOrder, runPos, runLen, blockShift);
                                const float start = static_cast<float>(-blockShift * runLen)
                                    + adjOff;
                                if (std::abs(start) >= 0.01f) {
                                    s->tabTracks[adjTab] = AppState::TabTrack{
                                        start, 150, std::chrono::steady_clock::now() };
                                    s->tabOffsets[adjTab] = start;
                                }
                            }
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (std::abs(dx) >= 2) { // 2px deadzone against jitter, as in TabBar
                        s->tabDragLastX = mx;
                        int cur = -1;
                        for (int p = 0; p < static_cast<int>(s->tabOrder.size()); ++p)
                            if (s->tabOrder[p] == s->tabDragIndex) { cur = p; break; }
                        const int n = static_cast<int>(s->tabOrder.size());
                        const int adj = dx < 0 ? cur - 1 : cur + 1;
                        int swapWith = -1;
                        if (cur >= 0 && adj >= 0 && adj < n) {
                            const int adjIdx = s->tabOrder[adj];
                            const app::Tab* adjTab =
                                (adjIdx >= 0 && adjIdx < static_cast<int>(s->pane->tabs.size()))
                                    ? s->pane->tabs[static_cast<size_t>(adjIdx)].get() : nullptr;
                            const int dragGid =
                                s->pane->tabs[static_cast<size_t>(s->tabDragIndex)]->tab_group;
                            const int adjGid = adjTab ? adjTab->tab_group : 0;
                            if (adjGid != 0 && adjGid != dragGid) {
                                // Group neighbor: hop the WHOLE run (expanded
                                // or collapsed) past the dragged tab, so the
                                // tab can never land between group members.
                                std::vector<int> groupOf(s->pane->tabs.size());
                                for (size_t i = 0; i < s->pane->tabs.size(); ++i)
                                    groupOf[i] = s->pane->tabs[i]->tab_group;
                                const app::GroupRun run =
                                    app::FindGroupRun(s->tabOrder, groupOf, adj, adjGid);
                                if (run.len > 0) {
                                    ui::WindowViewModel vmNow = BuildVm(*s);
                                    const float ww = static_cast<float>(s->compositor.Width());
                                    float blockLeft = 0.0f, blockRight = 0.0f, blockOff = 0.0f;
                                    bool haveBlock = false;
                                    int chipGi = -1; // collapsed: vm.tab_groups index
                                    D2D1_RECT_F rcFirst{}, rcLast{};
                                    if (s->renderer.TabItemRect(vmNow, ww, run.pos, &rcFirst) &&
                                        s->renderer.TabItemRect(vmNow, ww,
                                            run.pos + run.len - 1, &rcLast)) {
                                        blockLeft = rcFirst.left;
                                        blockRight = rcLast.right;
                                        const int fIdx = s->tabOrder[static_cast<size_t>(run.pos)];
                                        if (fIdx >= 0 &&
                                            fIdx < static_cast<int>(s->pane->tabs.size())) {
                                            const auto oit = s->tabOffsets.find(
                                                s->pane->tabs[static_cast<size_t>(fIdx)].get());
                                            if (oit != s->tabOffsets.end())
                                                blockOff = oit->second * s->tabPitch;
                                        }
                                        haveBlock = true;
                                    } else {
                                        // Collapsed group: the chip is the block.
                                        for (int gi = 0;
                                             gi < static_cast<int>(vmNow.tab_groups.size()); ++gi) {
                                            if (vmNow.tab_groups[static_cast<size_t>(gi)].id
                                                    != adjGid)
                                                continue;
                                            D2D1_RECT_F chipRc{};
                                            if (s->renderer.TabGroupChipRect(
                                                    vmNow, ww, gi, &chipRc)) {
                                                chipGi = gi;
                                                blockLeft = chipRc.left;
                                                blockRight = chipRc.right;
                                                const auto oit = s->chipOffsets.find(adjGid);
                                                if (oit != s->chipOffsets.end())
                                                    blockOff = oit->second;
                                                haveBlock = true;
                                            }
                                            break;
                                        }
                                    }
                                    if (haveBlock &&
                                        app::ChipBlockCrossed(s->tabDragFloatLeft, s->tabSlotW,
                                            (blockLeft + blockRight) * 0.5f + blockOff, dx)) {
                                        // The run slides one slot against the
                                        // drag direction (MoveTabRun keeps the
                                        // group contiguous by construction).
                                        const int dir = dx < 0 ? 1 : -1;
                                        const int newRunPos = app::MoveTabRun(
                                            s->tabOrder, run.pos, run.len, dir);
                                        ui::WindowViewModel vmAfter = BuildVm(*s);
                                        if (chipGi >= 0) {
                                            D2D1_RECT_F newRc{};
                                            if (s->renderer.TabGroupChipRect(
                                                    vmAfter, ww, chipGi, &newRc)) {
                                                const float startPx = app::DisplacedRestDelta(
                                                    blockLeft, blockOff, newRc.left);
                                                if (std::abs(startPx) >= 1.0f) {
                                                    s->chipTracks[adjGid] = AppState::TabTrack{
                                                        startPx, 150,
                                                        std::chrono::steady_clock::now() };
                                                    s->chipOffsets[adjGid] = startPx;
                                                }
                                            }
                                        } else if (s->tabPitch > 0.0f) {
                                            // Every visible member slides one
                                            // slot; track each from where it
                                            // visibly sits now.
                                            for (int p = newRunPos;
                                                 p < newRunPos + run.len && p < n; ++p) {
                                                const int tIdx = s->tabOrder[static_cast<size_t>(p)];
                                                if (tIdx < 0 ||
                                                    tIdx >= static_cast<int>(s->pane->tabs.size()))
                                                    continue;
                                                const app::Tab* member =
                                                    s->pane->tabs[static_cast<size_t>(tIdx)].get();
                                                float oldRest = s->tabFlowLeft
                                                    + static_cast<float>(p - dir) * s->tabPitch;
                                                D2D1_RECT_F oldRc{}, newRc{};
                                                if (s->renderer.TabItemRect(
                                                        vmNow, ww, p - dir, &oldRc))
                                                    oldRest = oldRc.left;
                                                if (!s->renderer.TabItemRect(
                                                        vmAfter, ww, p, &newRc))
                                                    continue; // hidden: no visual to animate
                                                float memberOff = 0.0f;
                                                const auto oit = s->tabOffsets.find(member);
                                                if (oit != s->tabOffsets.end())
                                                    memberOff = oit->second * s->tabPitch;
                                                const float startPx = app::DisplacedRestDelta(
                                                    oldRest, memberOff, newRc.left);
                                                const float start = startPx / s->tabPitch;
                                                if (std::abs(start) >= 0.01f) {
                                                    s->tabTracks[member] = AppState::TabTrack{
                                                        start, 150,
                                                        std::chrono::steady_clock::now() };
                                                    s->tabOffsets[member] = start;
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                const float adjOff = [&]() {
                                    if (!adjTab) return 0.0f;
                                    const auto oit = s->tabOffsets.find(adjTab);
                                    return oit != s->tabOffsets.end() ? oit->second : 0.0f;
                                }();
                                // Chip offsets make slots non-uniform: ask the
                                // renderer for this slot's rest rect instead of
                                // extrapolating from the pitch.
                                float adjRest = s->tabFlowLeft
                                    + static_cast<float>(adj) * s->tabPitch;
                                {
                                    ui::WindowViewModel vmNow = BuildVm(*s);
                                    D2D1_RECT_F adjRc{};
                                    if (s->renderer.TabItemRect(vmNow,
                                            static_cast<float>(s->compositor.Width()),
                                            adj, &adjRc))
                                        adjRest = adjRc.left;
                                }
                                const float adjCenter = adjRest + adjOff * s->tabPitch
                                    + s->tabSlotW * 0.5f;
                                if ((dx < 0 && s->tabDragFloatLeft < adjCenter) ||
                                    (dx > 0 && s->tabDragFloatLeft + s->tabSlotW > adjCenter))
                                    swapWith = adj;
                            }
                        }
                        if (swapWith >= 0) {
                            const int siblingIdx = s->tabOrder[swapWith];
                            std::swap(s->tabOrder[cur], s->tabOrder[swapWith]);
                            const app::Tab* sibling =
                                (siblingIdx >= 0 && siblingIdx < static_cast<int>(s->pane->tabs.size()))
                                    ? s->pane->tabs[static_cast<size_t>(siblingIdx)].get() : nullptr;
                            if (sibling) {
                                const auto off = s->tabOffsets.find(sibling);
                                const float curOff = off != s->tabOffsets.end() ? off->second : 0.0f;
                                const float start = static_cast<float>(swapWith) + curOff
                                    - static_cast<float>(cur);
                                if (std::abs(start) >= 0.01f) {
                                    s->tabTracks[sibling] = AppState::TabTrack{
                                        start, 150, std::chrono::steady_clock::now() };
                                    s->tabOffsets[sibling] = start;
                                }
                            }
                        }
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
        }

        if (s->marqueePending || s->marqueeActive) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                if (s->marqueeActive) ApplyMarqueeSelection(*s);
                ResetMarquee(*s);
            } else {
                s->marqueeCur = POINT{ mx, my };
                if (!s->marqueeActive &&
                    (std::abs(mx - s->marqueeStart.x) >= GetSystemMetrics(SM_CXDRAG) ||
                     std::abs(my - s->marqueeStart.y) >= GetSystemMetrics(SM_CYDRAG))) {
                    s->marqueeActive = true;
                    s->marqueePending = false;
                }
                if (s->marqueeActive) ApplyMarqueeSelection(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        // Drag-out threshold (1B-2): left button held after a row click.
        if (s->dragPending) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->dragPending = false;
                if (s->renameClickCandidate && s->renameClickDue == 0)
                    CancelRenameClick(*s);
            } else if (std::abs(mx - s->dragStartPt.x) >= GetSystemMetrics(SM_CXDRAG) ||
                       std::abs(my - s->dragStartPt.y) >= GetSystemMetrics(SM_CYDRAG)) {
                s->dragPending = false;
                CancelRenameClick(*s);
                StartDragOut(*s);
                return 0;
            }
        }

        // Tag reorder drag (QFluentKit TabBar model): the dragged tag follows
        // cursor deltas 1:1 and swaps with a sibling on center crossing.
        if (s->tagDragPending || s->tagDragActive) {
            if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {
                s->tagDragPending = false;
                s->tagDragActive = false;
                s->tagDragTag = -1;
                s->tagDragPath.clear();
                s->tagOrder.clear();
            } else {
                if (!s->tagDragActive &&
                    std::abs(my - s->tagDragStartPt.y) >= 2 && // TabBar starts on a 2px delta
                    s->tagDragTag >= 0 &&
                    s->tagDragTag < static_cast<int>(s->places.tags.size()) &&
                    s->renderer.EffectiveSidebarWidth(static_cast<float>(s->compositor.Width()))
                        > 60.0f * s->scale) {
                    // Capture the tag flow geometry once so the gesture can map
                    // cursor deltas 1:1 and evaluate sibling-center crossings.
                    ui::WindowViewModel vm0 = BuildVm(*s);
                    const float w0 = static_cast<float>(s->compositor.Width());
                    const float h0 = static_cast<float>(s->compositor.Height());
                    int g0 = -1;
                    for (int g = 0; g < static_cast<int>(vm0.sidebar.size()); ++g) {
                        if (!vm0.sidebar[g].items.empty() && vm0.sidebar[g].items[0].is_tag) {
                            g0 = g;
                            break;
                        }
                    }
                    const int n0 = g0 >= 0 ? static_cast<int>(vm0.sidebar[g0].items.size()) : 0;
                    D2D1_RECT_F first{}, last{}, self{};
                    if (n0 == static_cast<int>(s->places.tags.size()) &&
                        s->renderer.TagItemRect(vm0, w0, h0, g0, 0, &first) &&
                        s->renderer.TagItemRect(vm0, w0, h0, g0, n0 - 1, &last) &&
                        s->renderer.TagItemRect(vm0, w0, h0, g0, s->tagDragTag, &self)) {
                        s->tagDragActive = true;
                        s->tagFlowTop = first.top;
                        s->tagFlowBottom = last.bottom;
                        s->tagSlotH = self.bottom - self.top;
                        s->tagPitch = n0 >= 2 ? (last.top - first.top) / (n0 - 1) : s->tagSlotH;
                        s->tagDragFloatTop = self.top;
                        s->tagDragGrabDy = static_cast<float>(my) - self.top;
                        s->tagDragY = (self.top + self.bottom) * 0.5f;
                        s->tagDragLastY = my;
                        s->tagTracks.clear();
                        s->tagOffsets.clear();
                        s->tagOrder.resize(s->places.tags.size());
                        for (int i = 0; i < static_cast<int>(s->tagOrder.size()); ++i)
                            s->tagOrder[i] = i;
                    } else {
                        s->tagDragPending = false; // geometry unavailable: plain click
                    }
                }
                if (s->tagDragActive) {
                    const int dy = my - s->tagDragLastY;
                    if (std::abs(dy) >= 2) { // 2px deadzone against jitter, as in TabBar
                        s->tagDragLastY = my;
                        // Grab-relative position keeps the tag glued to the cursor
                        // even after clamping at a flow edge (TabBar edge behavior).
                        s->tagDragFloatTop = std::clamp(
                            static_cast<float>(my) - s->tagDragGrabDy, s->tagFlowTop,
                            std::max(s->tagFlowTop, s->tagFlowBottom - s->tagSlotH));
                        s->tagDragY = s->tagDragFloatTop + s->tagSlotH * 0.5f;
                        int cur = -1;
                        for (int p = 0; p < static_cast<int>(s->tagOrder.size()); ++p)
                            if (s->tagOrder[p] == s->tagDragTag) { cur = p; break; }
                        const int n = static_cast<int>(s->tagOrder.size());
                        const int adj = dy < 0 ? cur - 1 : cur + 1;
                        int swapWith = -1;
                        if (cur >= 0 && adj >= 0 && adj < n) {
                            // Compare against the sibling's current (possibly
                            // mid-slide) center, exactly like QFluent TabBar.
                            const int adjIdx = s->tagOrder[adj];
                            const std::wstring& adjLabel =
                                s->places.tags[static_cast<size_t>(adjIdx)].name;
                            const auto oit = s->tagOffsets.find(adjLabel);
                            const float adjOff =
                                oit != s->tagOffsets.end() ? oit->second : 0.0f;
                            const float adjCenter = s->tagFlowTop
                                + (adj + adjOff) * s->tagPitch + s->tagSlotH * 0.5f;
                            if ((dy < 0 && s->tagDragFloatTop < adjCenter) ||
                                (dy > 0 && s->tagDragFloatTop + s->tagSlotH > adjCenter))
                                swapWith = adj;
                        }
                        if (swapWith >= 0) {
                            const int siblingIdx = s->tagOrder[swapWith];
                            std::swap(s->tagOrder[cur], s->tagOrder[swapWith]);
                            // The sibling slides from its current visual slot to the
                            // slot the dragged tag just vacated (spring curve).
                            const std::wstring& label =
                                s->places.tags[static_cast<size_t>(siblingIdx)].name;
                            const auto off = s->tagOffsets.find(label);
                            const float curOff = off != s->tagOffsets.end() ? off->second : 0.0f;
                            const float start = static_cast<float>(swapWith) + curOff
                                - static_cast<float>(cur);
                            if (std::abs(start) >= 0.01f) {
                                s->tagTracks[label] = AppState::TagTrack{
                                    start, 300, std::chrono::steady_clock::now() };
                                s->tagOffsets[label] = start;
                            }
                        }
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
        }

        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);

        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        const int newRegion = static_cast<int>(hit.region);
        if (newRegion != s->hoverRegion || hit.index != s->hoverControlIndex ||
            hit.pane_index != s->hoverPaneIndex) {
            s->hoverRegion = newRegion;
            s->hoverControlIndex = hit.index;
            s->hoverSubIndex = hit.sub_index;
            s->hoverSince = GetTickCount64();
            s->tooltipText.clear();
            if (hit.region == ui::HitTestResult::SidebarItem && fs::IsUncPath(hit.path))
                RequestUncProbe(*s, hit.path);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        int newHover = (hit.region == ui::HitTestResult::Row ||
                        hit.region == ui::HitTestResult::RowStar ||
                        hit.region == ui::HitTestResult::RowNewTab ||
                        hit.region == ui::HitTestResult::RowMore) ? hit.index : -1;
        if (newHover != s->hoverRow || hit.pane_index != s->hoverPaneIndex) {
            s->hoverRow = newHover;
            s->hoverPaneIndex = hit.pane_index;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        int newCrumb = (hit.region == ui::HitTestResult::BreadcrumbSegment) ? hit.index : -1;
        if (newCrumb != s->breadcrumbHover) {
            s->breadcrumbHover = newCrumb;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        D2D1_RECT_F content = s->renderer.ContentRect((float)s->compositor.Width(), (float)s->compositor.Height());
        float extra = 0.0f;
        if (app::Tab* tab = ActiveTab(*s); tab && !tab->banner_message.empty()) extra = 36.0f * s->scale;
        float listTop = content.top + s->renderer.PaneHeaderHeight() + extra + s->renderer.ColumnHeaderHeight();
        bool sbHit = (mx >= content.right - 14 * s->scale && mx < content.right &&
                      my >= listTop && my < content.bottom);
        if (sbHit != s->scrollbarHovered) {
            s->scrollbarHovered = sbHit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (s) {
            s->scrollbarHovered = false;
            s->hoverRow = -1;
            s->hoverPaneIndex = -1;
            s->breadcrumbHover = -1;
            s->hoverRegion = 0;
            s->hoverControlIndex = -1;
            s->hoverSubIndex = -1;
            s->hoverSince = 0;
            s->tooltipText.clear();
            if (GetCapture() != hwnd) s->dragPending = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!s) break;
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        CancelRenameClick(*s);
        CancelScrollAnimation(*s);
        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        if (hit.pane_index >= 0) {
            if (app::Pane* p = PaneAtSlot(*s, hit.pane_index)) FocusPane(*s, p);
        }
        if (hit.region == ui::HitTestResult::DetailsResize) {
            s->dragPending = false;
            s->detailsPanelResizing = true;
            SetCapture(hwnd);
            return 0;
        }
        if (hit.region == ui::HitTestResult::DetailsPreview) {
            // Begin a cover-mode pan. Clamp against the current bitmap limits
            // first so a stale state (resized window, new bitmap) can't stick.
            s->dragPending = false;
            s->detailsPreviewPanX = std::clamp(s->detailsPreviewPanX, 0.0f,
                                               s->renderer.DetailsCoverMaxPanX());
            s->detailsPreviewPanY = std::clamp(s->detailsPreviewPanY, 0.0f,
                                               s->renderer.DetailsCoverMaxPanY());
            s->detailsPreviewPanning = true;
            s->detailsPreviewPanLast = POINT{ mx, my };
            SetCapture(hwnd);
            return 0;
        } else if (hit.region == ui::HitTestResult::ColumnDivider) {
            if (s->renameIndex >= 0) HideRenameOverlay(*s, true);
            s->dragPending = false;
            s->columnResizing = true;
            s->columnResizeIndex = hit.index;
            s->columnResizePane = hit.pane_index;
            s->hoverRegion = static_cast<int>(ui::HitTestResult::ColumnDivider);
            s->hoverControlIndex = hit.index;
            s->hoverPaneIndex = hit.pane_index;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        } else if (hit.region == ui::HitTestResult::Splitter) {
            s->dragPending = false;
            s->splitterDragging = true;
            s->splitterDragIndex = hit.index;
            if (hit.index >= 0 && hit.index < static_cast<int>(vm.splitters.size())) {
                s->splitterParentBounds = vm.splitters[static_cast<size_t>(hit.index)].parent_bounds;
                s->splitterOrientation = vm.splitters[static_cast<size_t>(hit.index)].vertical
                    ? app::SplitOrientation::Vertical : app::SplitOrientation::Horizontal;
            }
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::Scrollbar) {
            s->dragPending = false;
            D2D1_RECT_F track{}, thumb{};
            float maxScroll = 0.0f;
            app::Tab* tab = ActiveTab(*s);
            s->scrollbarHorizontal = hit.sub_index == 1;
            const bool hasGeometry = s->scrollbarHorizontal
                ? HorizontalScrollbarGeometry(*s, vm.pane, track, thumb, maxScroll)
                : ScrollbarGeometry(*s, vm.pane, track, thumb, maxScroll);
            if (tab && hasGeometry) {
                const bool outside = s->scrollbarHorizontal
                    ? (mx < thumb.left || mx >= thumb.right)
                    : (my < thumb.top || my >= thumb.bottom);
                if (outside) {
                    const float travel = std::max(1.0f,
                        s->scrollbarHorizontal
                            ? (track.right-track.left)-(thumb.right-thumb.left)
                            : (track.bottom-track.top)-(thumb.bottom-thumb.top));
                    const float pointer = s->scrollbarHorizontal ? mx-track.left : my-track.top;
                    const float thumbExtent = s->scrollbarHorizontal
                        ? thumb.right-thumb.left : thumb.bottom-thumb.top;
                    const float value = std::clamp((pointer-thumbExtent*0.5f)*maxScroll/travel,
                                                   0.0f,maxScroll);
                    if (s->scrollbarHorizontal) tab->scroll_x=value;
                    else { tab->scroll_y=value; s->scrollTargetY=value; MaybePrefetchSearchPage(*s); }
                }
                s->scrollbarDragging = true;
                s->scrollbarDragStartX = mx;
                s->scrollbarDragStartY = my;
                s->scrollbarDragStartScroll = s->scrollbarHorizontal ? tab->scroll_x : tab->scroll_y;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::Row && hit.index >= 0) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            app::Tab* tab = ActiveTab(*s);
            const bool renameCandidate = !ctrl && !shift && tab && !tab->net_readonly &&
                s->renameIndex < 0 && tab->SelectedCount() == 1 &&
                tab->selected_index == hit.index && tab->IsSelected(hit.index) &&
                PointInHitItemName(*s, vm, hit, static_cast<float>(mx), static_cast<float>(my));
            HandleListRowClick(*s, hit.index, ctrl, shift);
            if (renameCandidate && tab == ActiveTab(*s)) {
                s->renameClickCandidate = true;
                s->renameClickPane = s->pane;
                s->renameClickTab = tab;
                s->renameClickIndex = hit.index;
                s->renameClickPath = EntryFullPath(*tab, hit.index);
            }
            // Potential drag-out start; resolved by movement in WM_MOUSEMOVE.
            s->dragPending = true;
            s->dragStartPt = POINT{ mx, my };
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TabGroup) {
            // Group chip (Chromium behavior): press arms a whole-group drag;
            // a plain release toggles collapse; right-click opens the editor.
            if (hit.index >= 0 && hit.index < static_cast<int>(vm.tab_groups.size()) &&
                s->pane) {
                const int gid = vm.tab_groups[static_cast<size_t>(hit.index)].id;
                int first = -1;
                for (int i = 0; i < static_cast<int>(s->pane->tabs.size()); ++i)
                    if (s->pane->tabs[static_cast<size_t>(i)]->tab_group == gid) {
                        first = i;
                        break;
                    }
                if (first >= 0) {
                    s->tabDragPending = true;
                    s->tabDragging = false;
                    s->tabDragFromChip = true;
                    s->tabDragGroupId = gid;
                    s->tabDragIndex = first;
                    s->tabDragStartPt = POINT{ mx, my };
                    SetCapture(hwnd);
                }
            }
        } else if (hit.region == ui::HitTestResult::Tab && hit.index >= 0) {
            SwitchTab(*s, hit.index);
            s->tabDragPending = true;
            s->tabDragging = false;
            s->tabDragFromChip = false;
            s->tabDragIndex = hit.index;
            s->tabDragStartPt = POINT{ mx, my };
            SetCapture(hwnd);
        } else if (hit.region == ui::HitTestResult::TabClose && hit.index >= 0) {
            s->pane->CloseTab(static_cast<size_t>(hit.index));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TabNew) {
            NewTab(*s, NewTabPath(*s));
        } else if (hit.region == ui::HitTestResult::ThemeToggle) {
            ToggleTheme(*s);
        } else if (hit.region == ui::HitTestResult::SettingsButton) {
            OpenSettingsTab(*s, 0);
        } else if (hit.region == ui::HitTestResult::SettingsNav) {
            OpenSettingsTab(*s, hit.index);
        } else if (hit.region == ui::HitTestResult::SettingsToggle) {
            HandleSettingsToggle(*s, hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsEffect) {
            HandleSettingsEffect(*s, hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsDensity) {
            HandleSettingsDensity(*s, hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsWallpaper) {
            HandleSettingsWallpaper(*s, hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SettingsRestore) {
            s->ctxMenuPrefs.ResetToDefaults();
            s->ctxMenuPrefs.Save();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::NavBack) {
            GoBack(*s);
        } else if (hit.region == ui::HitTestResult::NavForward) {
            GoForward(*s);
        } else if (hit.region == ui::HitTestResult::NavUp) {
            GoUp(*s);
        } else if (hit.region == ui::HitTestResult::NavRefresh) {
            RefreshActiveTab(*s);
        } else if (hit.region == ui::HitTestResult::NewButton) {
            ShowNewDropdown(*s);
        } else if (hit.region == ui::HitTestResult::BreadcrumbSegment) {
            // Empty path is the This PC segment; NavigateTo handles it.
            NavigateTo(*s, hit.path);
        } else if (hit.region == ui::HitTestResult::Copy) {
            CollectToTray(*s, false);
        } else if (hit.region == ui::HitTestResult::Cut) {
            CollectToTray(*s, true);
        } else if (hit.region == ui::HitTestResult::Paste) {
            PasteIntoCurrent(*s);
        } else if (hit.region == ui::HitTestResult::Rename) {
            ShowRenameOverlay(*s);
        } else if (hit.region == ui::HitTestResult::Delete) {
            DeleteSelected(*s, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        } else if (hit.region == ui::HitTestResult::TrayRelease) {
            ReleaseTrayBatch(*s, (size_t)hit.index);
        } else if (hit.region == ui::HitTestResult::TrayClose) {
            s->tray.RemoveBatch((size_t)hit.index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TrayItemRemove) {
            s->tray.RemoveItem((size_t)hit.index, (size_t)hit.sub_index);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::TrayClear) {
            s->tray.Clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::RowStar && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                const std::wstring p = EntryFullPath(*tab, hit.index);
                if (!p.empty()) ToggleStarred(*s, p);
            }
        } else if (hit.region == ui::HitTestResult::RowNewTab && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                const std::wstring p = EntryFullPath(*tab, hit.index);
                if (!p.empty()) NewTab(*s, p);
            }
        } else if (hit.region == ui::HitTestResult::RowMore && hit.index >= 0) {
            if (app::Tab* tab = ActiveTab(*s)) {
                if (!tab->IsSelected(hit.index)) tab->SelectOnly(hit.index);
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                ShowItemContextMenu(*s, point);
            }
        } else if (hit.region == ui::HitTestResult::PaneEmptyNewFolder) {
            CreateNewItem(*s, true);
        } else if (hit.region == ui::HitTestResult::DetailsOpen) {
            OpenSelected(*s);
        } else if (hit.region == ui::HitTestResult::DetailsStar) {
            const std::wstring p = SelectedFullPath(*s);
            if (!p.empty()) ToggleStarred(*s, p);
        } else if (hit.region == ui::HitTestResult::DetailsMore) {
            if (EnsureMenu(*s)) {
                std::vector<ui::FluentMenuItem> items;
                ui::FluentMenuItem terminal;
                terminal.command = app::CmdOpenTerminal;
                terminal.text = L"在终端打开";
                terminal.glyph = L"\xE756";
                items.push_back(std::move(terminal));
                if (vm.details.is_dir) {
                    ui::FluentMenuItem size;
                    size.command = app::CmdDetailsComputeSize;
                    size.text = L"计算大小";
                    size.glyph = L"\xE8EF";
                    items.push_back(std::move(size));
                }
                ui::FluentMenuItem props;
                props.command = app::CmdProperties;
                props.text = L"属性";
                props.glyph = L"\xE946";
                props.separator_after = true;
                items.push_back(std::move(props));
                ui::FluentMenuItem shell;
                shell.command = app::CmdDetailsShellMenu;
                shell.text = L"系统菜单…";
                shell.glyph = L"\xE712";
                items.push_back(std::move(shell));
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                const int cmd = s->menu->TrackPopup(point, std::move(items));
                if (cmd == app::CmdOpenTerminal) {
                    std::wstring p = vm.details.path;
                    if (!p.empty() && !vm.details.is_dir) p = fs::ParentPath(p);
                    if (!p.empty()) s->ops.OpenTerminal(ClipboardPath(p));
                } else if (cmd == app::CmdDetailsComputeSize) {
                    if (!vm.details.path.empty()) StartDetailsSizeWalk(*s, vm.details.path);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (cmd == app::CmdProperties) {
                    DispatchMenuCommand(*s, app::CmdProperties);
                } else if (cmd == app::CmdDetailsShellMenu) {
                    ShowItemContextMenu(*s, point);
                }
            }
        } else if (hit.region == ui::HitTestResult::DetailsNewTab) {
            if (!vm.details.path.empty()) {
                const std::wstring target = vm.details.path;
                if (vm.details.is_dir) {
                    NewTab(*s, target);
                } else {
                    NewTab(*s, fs::ParentPath(target));
                    if (app::Tab* tab = ActiveTab(*s)) {
                        std::wstring leaf = target;
                        if (leaf.starts_with(L"\\\\?\\UNC\\")) leaf = L"\\\\" + leaf.substr(8);
                        else if (leaf.starts_with(L"\\\\?\\")) leaf = leaf.substr(4);
                        const auto slash = leaf.find_last_of(L"\\/");
                        if (slash != std::wstring::npos) leaf = leaf.substr(slash + 1);
                        tab->pending_selected_name = leaf;
                        tab->pending_selected_names = { leaf };
                    }
                }
            }
        } else if (hit.region == ui::HitTestResult::DetailsCopyPath) {
            if (!vm.details.path.empty())
                ops::WriteClipboardText(ClipboardPath(vm.details.path));
        } else if (hit.region == ui::HitTestResult::DetailsSection) {
            if (hit.index >= 0 && hit.index < 32) {
                s->detailsCollapsedMask ^= (1u << hit.index);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::DetailsAttrToggle) {
            if (hit.index == 2) {
                DispatchMenuCommand(*s, app::CmdProperties);
            } else if (!vm.details.path.empty() &&
                       (hit.index == 0 || hit.index == 1)) {
                const DWORD flag = hit.index == 0 ? FILE_ATTRIBUTE_READONLY
                                                  : FILE_ATTRIBUTE_HIDDEN;
                DWORD attrs = GetFileAttributesW(vm.details.path.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES) {
                    attrs = (attrs & flag) ? (attrs & ~flag) : (attrs | flag);
                    if (SetFileAttributesW(vm.details.path.c_str(), attrs))
                        RefreshActiveTab(*s);
                }
            }
        } else if (hit.region == ui::HitTestResult::DetailsSecurityChange) {
            DispatchMenuCommand(*s, app::CmdProperties);
        } else if (hit.region == ui::HitTestResult::DetailsRename) {
            ShowRenameOverlay(*s);
        } else if (hit.region == ui::HitTestResult::DetailsTagAdd) {
            POINT point{ mx, my };
            ClientToScreen(hwnd, &point);
            ShowTagPicker(*s, point);
        } else if (hit.region == ui::HitTestResult::DetailsPresetTag) {
            if (hit.index >= 0 && hit.index < static_cast<int>(s->places.tags.size()) &&
                !vm.details.path.empty()) {
                const bool assigned = s->places.PathHasTag(vm.details.path, hit.index);
                std::vector<app::TagAdsUpdate> ads_updates;
                s->places.SetTaggedBatch(hit.index, { vm.details.path }, !assigned,
                                         &ads_updates);
                QueueTagAds(*s, std::move(ads_updates));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::SplitButton) {
            ShowSplitDropdown(*s);
        } else if (hit.region == ui::HitTestResult::DetailsToggle) {
            s->showDetailsPanel = !s->showDetailsPanel;
            s->renderer.SetDetailsPanelVisible(s->showDetailsPanel);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::PaneMediumIcons) {
            SetViewMode(*s, ui::ViewMode::MediumIcons);
        } else if (hit.region == ui::HitTestResult::PaneViewButton) {
            ShowViewDropdown(*s, hit.pane_index);
        } else if (hit.region == ui::HitTestResult::FilterBox) {
            ShowFilterEditor(*s);
        } else if (hit.region == ui::HitTestResult::AddressBar) {
            ShowOmnibar(*s, OmnibarMode::Path);
        } else if (hit.region == ui::HitTestResult::ColumnHeader) {
            SortBy(*s, hit.column);
        } else if (hit.region == ui::HitTestResult::SidebarHeaderAction) {
            // Sidebar groups are pushed in a fixed order; group 4 is 网络位置.
            if (hit.index == 4) {
                app::Tab* tab = ActiveTab(*s);
                if (tab && fs::IsUncPath(tab->current_path)) {
                    s->places.PinNetwork(tab->current_path, L"");
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else {
                POINT point{ mx, my };
                ClientToScreen(hwnd, &point);
                ShowTagPicker(*s, point);
            }
        } else if (hit.region == ui::HitTestResult::SidebarHeader) {
            if (hit.index >= 0 && hit.index < 32) {
                s->sidebarCollapsedMask ^= (1u << hit.index);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (hit.region == ui::HitTestResult::SidebarItemAction) {
            std::wstring kind, rest;
            if (app::ParsePulsePath(hit.path, &kind, &rest) && kind == L"workspace")
                s->places.UnpinWorkspace(_wtoi(rest.c_str()));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (hit.region == ui::HitTestResult::SidebarItem) {
            if (hit.path.starts_with(L"pulse:tag:")) {
                // Tags defer navigation to release; a press may become a reorder drag.
                s->tagDragPending = true;
                s->tagDragStartPt = POINT{ mx, my };
                s->tagDragPath = hit.path;
                std::wstring kind, rest;
                app::ParsePulsePath(hit.path, &kind, &rest);
                s->tagDragTag = s->places.FindTagIndex(s->places.ResolveTagRef(rest));
                SetCapture(hwnd);
            } else if (hit.path.starts_with(L"pulse:workspace:")) {
                std::wstring kind, rest;
                app::ParsePulsePath(hit.path, &kind, &rest);
                OpenWorkspace(*s, _wtoi(rest.c_str()));
            } else if (!hit.path.empty()) {
                NavigateTo(*s, hit.path);
            }
        } else if (hit.region == ui::HitTestResult::StatusBar) {
            const ops::OpStatus status = s->ops.Status();
            if (s->operationWindow &&
                (status.active || !status.summary.empty() || !status.last_error.empty())) {
                s->operationWindow->Update(status);
                s->operationWindow->Show(true);
            }
        } else if (!IsSettingsTab(ActiveTab(*s)) &&
                   (hit.region == ui::HitTestResult::Pane ||
                   (PointInList(*s, mx, my) && hit.region == ui::HitTestResult::None))) {
            app::Tab* tab = ActiveTab(*s);
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (tab && !ctrl) tab->ClearSelection();
            s->marqueePending = true;
            s->marqueeActive = false;
            s->marqueeAdditive = ctrl;
            s->marqueeStart = s->marqueeCur = POINT{ mx, my };
            s->marqueeBase.clear();
            if (tab && ctrl) {
                tab->MaterializeSelection();
                s->marqueeBase = tab->selected;
            }
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        if (!s) break;
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        CancelRenameClick(*s);
        s->dragPending = false;
        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        if (hit.region == ui::HitTestResult::SidebarItem &&
            hit.path.starts_with(L"pulse:tag:")) {
            s->tagDragPending = false;
            s->tagDragActive = false;
            s->tagDragTag = -1;
            s->tagDragPath.clear();
            if (GetCapture() == hwnd) ReleaseCapture();
            std::wstring kind, rest;
            app::ParsePulsePath(hit.path, &kind, &rest);
            ShowTagRenameOverlay(*s, s->places.ResolveTagRef(rest));
        } else if (hit.region == ui::HitTestResult::Row && hit.index >= 0) {
            app::Tab* tab = ActiveTab(*s);
            if (tab) tab->SelectOnly(hit.index);
            OpenSelected(*s);
        } else if (hit.region == ui::HitTestResult::AddressBar ||
                   hit.region == ui::HitTestResult::BreadcrumbSegment) {
            ShowOmnibar(*s, OmnibarMode::Path);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (s) {
            if (s->tagDragPending || s->tagDragActive) {
                const bool wasActive = s->tagDragActive;
                const std::wstring path = s->tagDragPath;
                if (wasActive && s->tagDragTag >= 0 &&
                    s->tagDragTag < static_cast<int>(s->places.tags.size())) {
                    // Slide the dragged tag home; duration scales with distance
                    // (250 ms per slot height, snap when < 50 ms), as in TabBar.
                    int cur = -1;
                    for (int p = 0; p < static_cast<int>(s->tagOrder.size()); ++p)
                        if (s->tagOrder[p] == s->tagDragTag) { cur = p; break; }
                    if (cur >= 0 && s->tagPitch > 0.0f) {
                        const float targetTop = s->tagFlowTop + cur * s->tagPitch;
                        const float dist = std::abs(s->tagDragFloatTop - targetTop);
                        const int raw = static_cast<int>(
                            dist * 250.0f / std::max(1.0f, s->tagSlotH));
                        // Snap tiny gaps; otherwise spring home with enough time
                        // for the overshoot to read (Pivot/SegmentedWidget curve).
                        const int dur = raw < 120 ? 0 : std::clamp(raw, 280, 500);
                        if (dur > 0) {
                            const std::wstring label =
                                s->places.tags[static_cast<size_t>(s->tagDragTag)].name;
                            const float start =
                                (s->tagDragFloatTop - targetTop) / s->tagPitch;
                            s->tagTracks[label] = AppState::TagTrack{
                                start, dur, std::chrono::steady_clock::now() };
                            s->tagOffsets[label] = start;
                        }
                    }
                }
                if (wasActive && s->tagOrder.size() == s->places.tags.size() &&
                    !s->tagOrder.empty()) {
                    bool changed = false;
                    for (size_t i = 0; i < s->tagOrder.size(); ++i)
                        if (s->tagOrder[i] != static_cast<int>(i)) { changed = true; break; }
                    if (changed) {
                        std::vector<app::ColorTag> reordered;
                        reordered.reserve(s->places.tags.size());
                        for (int idx : s->tagOrder)
                            if (idx >= 0 && idx < static_cast<int>(s->places.tags.size()))
                                reordered.push_back(s->places.tags[static_cast<size_t>(idx)]);
                        if (reordered.size() == s->places.tags.size()) {
                            s->places.tags = std::move(reordered);
                            s->places.TagsReordered();
                        }
                    }
                }
                s->tagDragPending = false;
                s->tagDragActive = false;
                s->tagDragTag = -1;
                s->tagDragPath.clear();
                s->tagOrder.clear();
                if (GetCapture() == hwnd) ReleaseCapture();
                if (!wasActive && !path.empty()) NavigateTo(*s, path); // plain click
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->tabDragPending || s->tabDragging) {
                const bool wasActive = s->tabDragging;
                if (wasActive && s->pane && s->tabDragIndex >= 0 &&
                    s->tabDragIndex < static_cast<int>(s->pane->tabs.size())) {
                    int cur = -1;
                    for (int p = 0; p < static_cast<int>(s->tabOrder.size()); ++p)
                        if (s->tabOrder[p] == s->tabDragIndex) { cur = p; break; }
                    if (cur >= 0 && s->tabPitch > 0.0f) {
                        // Collapsed chip drag: the drop target is the chip's
                        // new rest rect, and the settle animation runs on the
                        // px chip channel (hidden members can't carry it).
                        const bool chipDrop = s->tabDragFromChip && s->tabDragBlockW > 0.0f;
                        float targetLeft = s->tabFlowLeft + static_cast<float>(cur) * s->tabPitch;
                        {
                            ui::WindowViewModel vmDrop = BuildVm(*s);
                            const float wwDrop = static_cast<float>(s->compositor.Width());
                            if (chipDrop) {
                                for (int gi = 0;
                                     gi < static_cast<int>(vmDrop.tab_groups.size()); ++gi) {
                                    if (vmDrop.tab_groups[static_cast<size_t>(gi)].id
                                            != s->tabDragGroupId)
                                        continue;
                                    D2D1_RECT_F chipRc{};
                                    if (s->renderer.TabGroupChipRect(vmDrop, wwDrop, gi, &chipRc))
                                        targetLeft = chipRc.left;
                                    break;
                                }
                            } else {
                                D2D1_RECT_F curRc{};
                                if (s->renderer.TabItemRect(vmDrop, wwDrop, cur, &curRc))
                                    targetLeft = curRc.left;
                            }
                        }
                        const float dist = std::abs(s->tabDragFloatLeft - targetLeft);
                        // chipDrop: tabSlotW holds the chip width, so scale the
                        // duration by the uniform tab pitch instead.
                        const float durSlot = chipDrop ? s->tabPitch : s->tabSlotW;
                        const int dur = static_cast<int>(
                            dist * 250.0f / std::max(1.0f, durSlot));
                        if (dur >= 50) {
                            if (chipDrop) {
                                const float start = s->tabDragFloatLeft - targetLeft; // px
                                s->chipTracks[s->tabDragGroupId] = AppState::TabTrack{
                                    start, dur, std::chrono::steady_clock::now() };
                                s->chipOffsets[s->tabDragGroupId] = start;
                            } else {
                                const app::Tab* key =
                                    s->pane->tabs[static_cast<size_t>(s->tabDragIndex)].get();
                                const float start = (s->tabDragFloatLeft - targetLeft) / s->tabPitch;
                                s->tabTracks[key] = AppState::TabTrack{
                                    start, dur, std::chrono::steady_clock::now() };
                                s->tabOffsets[key] = start;
                            }
                        }
                    }
                    if (s->tabOrder.size() == s->pane->tabs.size() && !s->tabOrder.empty()) {
                        bool changed = false;
                        for (size_t i = 0; i < s->tabOrder.size(); ++i)
                            if (s->tabOrder[i] != static_cast<int>(i)) { changed = true; break; }
                        if (changed) {
                            std::vector<std::unique_ptr<app::Tab>> reordered;
                            reordered.reserve(s->pane->tabs.size());
                            size_t newActive = s->pane->active_tab;
                            for (int i = 0; i < static_cast<int>(s->tabOrder.size()); ++i) {
                                if (s->tabOrder[static_cast<size_t>(i)] ==
                                    static_cast<int>(s->pane->active_tab))
                                    newActive = static_cast<size_t>(i);
                            }
                            for (int idx : s->tabOrder) {
                                if (idx >= 0 && idx < static_cast<int>(s->pane->tabs.size()))
                                    reordered.push_back(
                                        std::move(s->pane->tabs[static_cast<size_t>(idx)]));
                            }
                            if (reordered.size() == s->pane->tabs.size()) {
                                s->pane->tabs = std::move(reordered);
                                s->pane->active_tab = newActive;
                            }
                        }
                    }
                    // Browser-style grouping: a tab dropped inside a
                    // same-group run (or against its end) joins it; a grouped
                    // tab dropped away from every member leaves the group.
                    // Chip drags move the whole group by construction, so
                    // these rules only apply to member-tab drags.
                    if (!s->tabDragFromChip && cur >= 0 && cur < static_cast<int>(s->pane->tabs.size())) {
                        app::Tab* moved = s->pane->tabs[static_cast<size_t>(cur)].get();
                        const int prevG = cur > 0
                            ? s->pane->tabs[static_cast<size_t>(cur - 1)]->tab_group : 0;
                        const int nextG = cur + 1 < static_cast<int>(s->pane->tabs.size())
                            ? s->pane->tabs[static_cast<size_t>(cur + 1)]->tab_group : 0;
                        int joined = 0;
                        if (prevG != 0 && prevG == nextG) joined = prevG;
                        else if (moved->tab_group == 0) {
                            if (nextG == 0) joined = prevG;      // run's trailing edge
                            else if (prevG == 0) joined = nextG; // run's leading edge
                        }
                        if (joined != 0) moved->tab_group = joined;
                        else if (moved->tab_group != 0 &&
                                 moved->tab_group != prevG && moved->tab_group != nextG)
                            moved->tab_group = 0;
                        // Groups with no members left disappear.
                        auto& groups = s->pane->tab_groups;
                        for (auto git = groups.begin(); git != groups.end();) {
                            bool used = false;
                            for (const auto& t : s->pane->tabs)
                                if (t->tab_group == git->id) { used = true; break; }
                            if (used) ++git; else git = groups.erase(git);
                        }
                    }
                }
                // Plain chip click (press without drag): toggle collapse.
                if (s->tabDragFromChip && !wasActive && s->tabDragGroupId != 0)
                    ToggleTabGroupCollapse(*s, s->tabDragGroupId);
                s->tabDragPending = false;
                s->tabDragging = false;
                s->tabDragIndex = -1;
                s->tabDragRunPos = 0;
                s->tabDragRunLen = 1;
                s->tabDragFromChip = false;
                s->tabDragGroupId = 0;
                s->tabDragSlots = 1.0f;
                s->tabOrder.clear();
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (s->marqueeActive || s->marqueePending) {
                if (s->marqueeActive) ApplyMarqueeSelection(*s);
                ResetMarquee(*s);
            } else if (s->clickCollapseIndex >= 0) {
                app::Tab* tab = ActiveTab(*s);
                if (tab) tab->SelectOnly(s->clickCollapseIndex);
            }
            if (s->renameClickCandidate && s->renameClickDue == 0) {
                const int mx = GET_X_LPARAM(lParam);
                const int my = GET_Y_LPARAM(lParam);
                ui::WindowViewModel vm = BuildVm(*s);
                D2D1_RECT_F rect = D2D1::RectF(
                    0, 0, static_cast<float>(s->compositor.Width()),
                    static_cast<float>(s->compositor.Height()));
                const ui::HitTestResult hit = s->renderer.HitTest(
                    vm, rect, static_cast<float>(mx), static_cast<float>(my));
                app::Tab* tab = ActiveTab(*s);
                const bool valid = s->pane == s->renameClickPane &&
                    tab == s->renameClickTab && tab && tab->SelectedCount() == 1 &&
                    tab->selected_index == s->renameClickIndex &&
                    hit.index == s->renameClickIndex &&
                    PointInHitItemName(*s, vm, hit, static_cast<float>(mx), static_cast<float>(my));
                if (valid) {
                    s->renameClickDue = GetTickCount64() + GetDoubleClickTime();
                } else {
                    CancelRenameClick(*s);
                }
            }
            s->clickCollapseIndex = -1;
            s->dragPending = false;
            s->scrollbarDragging = false;
            s->scrollbarHorizontal = false;
            s->splitterDragging = false;
            s->detailsPanelResizing = false;
            s->detailsPreviewPanning = false;
            s->columnResizing = false;
            s->columnResizeIndex = -1;
            s->columnResizePane = -1;
            s->splitterDragIndex = -1;
            s->tabDragPending = false;
            s->tabDragging = false;
            s->tabDragIndex = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (s) {
            s->columnResizing = false;
            s->columnResizeIndex = -1;
            s->columnResizePane = -1;
            if (s->renameClickCandidate && s->renameClickDue == 0)
                CancelRenameClick(*s);
            if (s->tagDragPending || s->tagDragActive) {
                // Capture lost mid-gesture: cancel the reorder, keep places.tags.
                s->tagDragPending = false;
                s->tagDragActive = false;
                s->tagDragTag = -1;
                s->tagDragPath.clear();
                s->tagOrder.clear();
                s->tagTracks.clear();
                s->tagOffsets.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (s->tabDragPending || s->tabDragging) {
                s->tabDragPending = false;
                s->tabDragging = false;
                s->tabDragIndex = -1;
                s->tabOrder.clear();
                s->tabTracks.clear();
                s->tabOffsets.clear();
                s->chipTracks.clear();
                s->chipOffsets.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (s->marqueeActive) ApplyMarqueeSelection(*s);
            ResetMarquee(*s);
            s->clickCollapseIndex = -1;
            s->dragPending = false;
            s->scrollbarDragging = false;
            s->scrollbarHorizontal = false;
            s->splitterDragging = false;
            s->splitterDragIndex = -1;
            s->tabDragPending = false;
            s->tabDragging = false;
            s->tabDragIndex = -1;
        }
        return 0;

    case WM_RBUTTONDOWN: {
        if (!s) break;
        CancelRenameClick(*s);
        // Prefetch the Explorer verbs for the menu that WM_RBUTTONUP will
        // open: pulse_shell builds the COM menu during the press + fade-in.
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        app::Pane* pane = hit.pane_index >= 0 ? PaneAtSlot(*s, hit.pane_index) : s->pane;
        app::Tab* tab = pane ? pane->ActiveTab() : nullptr;
        if (hit.region == ui::HitTestResult::Row && hit.index >= 0 && tab) {
            // Selection changes on WM_RBUTTONUP; predict what it will be.
            std::vector<std::wstring> paths;
            std::wstring ext;
            if (tab->IsSelected(hit.index)) {
                paths = SelectedFullPaths(*tab);
                ext = CommonExtension(*tab, tab->SelectedIndices());
            } else {
                std::wstring one = EntryFullPath(*tab, hit.index);
                if (!one.empty()) paths.push_back(std::move(one));
                ext = CommonExtension(*tab, { hit.index });
            }
            if (!paths.empty()) StartCtxQuery(*s, std::move(paths), false, ext);
        } else if (tab && !tab->current_path.empty() &&
                   !fs::IsVirtualPath(tab->current_path) &&
                   (hit.region == ui::HitTestResult::Pane ||
                    hit.region == ui::HitTestResult::None)) {
            D2D1_RECT_F content = s->renderer.ContentRect(rect.right, rect.bottom);
            if (hit.pane_index >= 0 ||
                (mx >= content.left && mx < content.right && my >= content.top && my < content.bottom)) {
                StartCtxQuery(*s, { tab->current_path }, true, L"");
            }
        }
        break;
    }

    case WM_RBUTTONUP: {
        if (!s) break;
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        ui::WindowViewModel vm = BuildVm(*s);
        D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, rect, (float)mx, (float)my);
        if (hit.pane_index >= 0) {
            if (app::Pane* p = PaneAtSlot(*s, hit.pane_index)) FocusPane(*s, p);
        }
        POINT sp{ mx, my };
        ClientToScreen(hwnd, &sp);
        bool shown = false;
        if (hit.region == ui::HitTestResult::TabGroup && hit.index >= 0 &&
            hit.index < static_cast<int>(vm.tab_groups.size())) {
            // Group chip right-click: the Edge-style editor bubble.
            ShowTabGroupMenu(*s, vm.tab_groups[static_cast<size_t>(hit.index)].id, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::Tab && hit.index >= 0) {
            // Every tab gets the Edge-style tab menu; group editing lives on
            // the chip (right-click) and in the editor bubble.
            ShowTabContextMenu(*s, hit.index, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::Row && hit.index >= 0) {
            app::Tab* tab = ActiveTab(*s);
            if (tab) {
                if (!tab->IsSelected(hit.index)) tab->SelectOnly(hit.index);
                else tab->selected_index = hit.index;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            ShowItemContextMenu(*s, sp);
            shown = true;
        } else if (hit.region == ui::HitTestResult::SidebarItem &&
                   hit.path.starts_with(L"pulse:tag:")) {
            std::wstring kind, rest;
            app::ParsePulsePath(hit.path, &kind, &rest);
            const app::TagId id = s->places.ResolveTagRef(rest);
            if (!id.empty()) ShowTagSidebarMenu(*s, id, sp);
        } else if (!IsSettingsTab(ActiveTab(*s)) &&
                   (hit.region == ui::HitTestResult::Pane ||
                   hit.region == ui::HitTestResult::FilterBox ||
                   hit.region == ui::HitTestResult::ColumnHeader ||
                   hit.region == ui::HitTestResult::None)) {
            D2D1_RECT_F content = s->renderer.ContentRect(rect.right, rect.bottom);
            if (hit.pane_index >= 0 ||
                (mx >= content.left && mx < content.right && my >= content.top && my < content.bottom)) {
                ShowBackgroundContextMenu(*s, sp);
                shown = true;
            }
        }
        // The press may have prefetched a session for a menu that never
        // opened (released over the sidebar, drag, …): free the host thread.
        if (!shown) CloseCtxSession(*s);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!s) break;
        CancelRenameClick(*s);
        if (IsSettingsTab(ActiveTab(*s))) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ui::WindowViewModel svm = BuildVm(*s);
            const float max_scroll = s->renderer.SettingsMaxScroll(
                svm, static_cast<float>(s->compositor.Width()),
                static_cast<float>(s->compositor.Height()));
            s->settingsScroll = std::clamp(
                s->settingsScroll - static_cast<float>(delta) / 120.0f * 48.0f * s->scale,
                0.0f, max_scroll);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        ui::WindowViewModel wheelVm = BuildVm(*s);
        D2D1_RECT_F wheelRect = D2D1::RectF(0, 0, (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult wheelHit = s->renderer.HitTest(wheelVm, wheelRect, (float)pt.x, (float)pt.y);
        const D2D1_RECT_F detailsRc = s->renderer.DetailsPanelRect(
            wheelRect.right, wheelRect.bottom);
        if (detailsRc.right > detailsRc.left && pt.x >= detailsRc.left &&
            pt.x < detailsRc.right && pt.y >= detailsRc.top && pt.y < detailsRc.bottom) {
            const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                                static_cast<float>(WHEEL_DELTA);
            if (wheelVm.details.multi_count <= 1 && !wheelVm.details.is_dir &&
                wheelHit.region == ui::HitTestResult::DetailsPreview) {
                s->detailsPreviewScroll = std::clamp(
                    s->detailsPreviewScroll - steps * 48.0f, 0.0f, 4000.0f);
            } else {
                const float visibleDip = (detailsRc.bottom - detailsRc.top) / s->scale;
                const float contentDip = s->renderer.DetailsContentHeightDip(
                    wheelVm, wheelRect.right, wheelRect.bottom);
                s->detailsScroll = std::clamp(s->detailsScroll - steps * 48.0f,
                    0.0f, std::max(0.0f, contentDip - visibleDip));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // Tray deck: wheel over the panel pages the icon window.
        if (TrayItemTotalCount(s->tray) > static_cast<int>(kTrayDeckCap)) {
            const D2D1_RECT_F tray_rc = s->renderer.StagingTrayRect(
                wheelVm, wheelRect.right, wheelRect.bottom);
            if (pt.x >= tray_rc.left && pt.x < tray_rc.right &&
                pt.y >= tray_rc.top && pt.y < tray_rc.bottom) {
                const int wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
                s->trayWheelAccum += wheel_delta;
                int steps = 0;
                while (s->trayWheelAccum <= -WHEEL_DELTA) { s->trayWheelAccum += WHEEL_DELTA; ++steps; }
                while (s->trayWheelAccum >= WHEEL_DELTA) { s->trayWheelAccum -= WHEEL_DELTA; --steps; }
                if (steps != 0) {
                    const int max_off = std::max(0, TrayItemTotalCount(s->tray) -
                        static_cast<int>(kTrayDeckCap));
                    const int next = std::clamp(s->trayDeckOffset + steps, 0, max_off);
                    if (next != s->trayDeckOffset) {
                        s->trayDeckOffset = next;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                return 0;
            }
        }
        if (wheelHit.pane_index >= 0) {
            if (app::Pane* p = PaneAtSlot(*s, wheelHit.pane_index)) FocusPane(*s, p);
        }
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        app::Tab* wheelTab = ActiveTab(*s);
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0) {
            if (wheelTab) {
                const int direction = delta > 0 ? -1 : 1;
                const int next = std::clamp(ui::ViewModeIndex(wheelTab->view_mode) + direction, 0, 7);
                SetViewMode(*s, ui::ViewModeFromIndex(next));
            }
            return 0;
        }
        if (wheelTab && wheelTab->view_mode == ui::ViewMode::List) {
            ui::PaneViewModel pane;
            app::FillPaneViewModel(pane, *s->pane, &s->places);
            const float maxX = s->renderer.MaxScrollXForPane(pane, FocusedPaneRect(*s));
            wheelTab->scroll_x = std::clamp(wheelTab->scroll_x -
                (static_cast<float>(delta) / WHEEL_DELTA) * 220.0f * s->scale,
                0.0f, maxX);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        UINT wheelLines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
        float distance = 0.0f;
        if (wheelLines == WHEEL_PAGESCROLL) {
            D2D1_RECT_F list = ListRect(*s);
            distance = (list.bottom - list.top) * 0.9f;
        } else {
            distance = s->renderer.RowHeight() * static_cast<float>(wheelLines);
        }
        StartSmoothScroll(*s,
            -(static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA)) * distance);
        return 0;
    }

    case WM_SYSKEYDOWN:
        if (s && wParam == L'D' && (GetKeyState(VK_MENU) & 0x8000)) {
            ShowOmnibar(*s, OmnibarMode::Path);
            return 0;
        }
        break;

    case WM_KEYDOWN: {
        if (!s) break;
        CancelRenameClick(*s);
        app::Tab* tab = ActiveTab(*s);
        if (!tab) break;
        ui::WindowViewModel vm = BuildVm(*s);
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool handled = true;

        if (ctrl && wParam == L'T') {
            NewTab(*s, NewTabPath(*s));
        } else if (ctrl && wParam == L'K') {
            ShowOmnibar(*s, OmnibarMode::Command);
        } else if (ctrl && wParam == L'P') {
            ShowOmnibar(*s, OmnibarMode::Project);
        } else if (ctrl && shift && wParam >= L'1' && wParam <= L'7') {
            app::Tab* t = ActiveTab(*s);
            const int tag_index = static_cast<int>(wParam - L'1');
            if (t && tag_index < static_cast<int>(s->places.tags.size()))
                ToggleTagForSelection(*s, s->places.tags[static_cast<size_t>(tag_index)].id,
                                      SelectedFullPaths(*t));
        } else if (ctrl && wParam == L'F') {
            ShowFilterEditor(*s);
        } else if (ctrl && wParam == L'D') {
            MarkTargetPane(*s);
        } else if (wParam == VK_F6) {
            CycleFocus(*s);
        } else if (ctrl && wParam == L'1') {
            ApplyLayoutPreset(*s, app::LayoutPreset::Single);
        } else if (ctrl && wParam == L'2') {
            ApplyLayoutPreset(*s, app::LayoutPreset::TwoVertical);
        } else if (ctrl && wParam == L'3') {
            ApplyLayoutPreset(*s, app::LayoutPreset::Three);
        } else if (ctrl && wParam == L'4') {
            ApplyLayoutPreset(*s, app::LayoutPreset::FourGrid);
        } else if (ctrl && alt && wParam == L'C') {
            TransferToTarget(*s, false);
        } else if (ctrl && alt && wParam == L'X') {
            TransferToTarget(*s, true);
        } else if (ctrl && wParam == L'W') {
            CloseActiveTab(*s);
        } else if (ctrl && wParam == VK_TAB) {
            if (!s->pane->tabs.empty()) {
                SwitchTab(*s, (s->pane->active_tab + 1) % s->pane->tabs.size());
            }
        } else if (ctrl && wParam == L'L') {
            ShowOmnibar(*s, OmnibarMode::Path);
        } else if (wParam == VK_F4) {
            ShowOmnibar(*s, OmnibarMode::Path);
        } else if (ctrl && shift && wParam == L'C') {
            CopySelectedPath(*s);
        } else if (ctrl && wParam == L'C') {
            CollectToTray(*s, false);
        } else if (ctrl && wParam == L'X') {
            CollectToTray(*s, true);
        } else if (ctrl && wParam == L'V') {
            PasteIntoCurrent(*s);
        } else if (ctrl && wParam == L'Z') {
            if (s->ops.CanUndo()) s->ops.Undo();
            else InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == VK_F2) {
            ShowRenameOverlay(*s);
        } else if (wParam == VK_F7) {
            CreateNewItem(*s, true); // 新建文件夹并进入重命名（ui.md §7.9）
        } else if (alt && wParam == VK_RETURN) {
            DispatchMenuCommand(*s, app::CmdProperties);
        } else if (wParam == VK_DELETE) {
            DeleteSelected(*s, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        } else if (wParam == VK_RETURN) {
            OpenSelected(*s);
        } else if (wParam == VK_BACK || (alt && wParam == VK_UP)) {
            GoUp(*s);
        } else if (alt && wParam == VK_LEFT) {
            GoBack(*s);
        } else if (alt && wParam == VK_RIGHT) {
            GoForward(*s);
        } else if (wParam == VK_F5) {
            RefreshActiveTab(*s);
        } else if (wParam == VK_F1) {
            s->showFps = !s->showFps;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wParam == VK_SPACE) {
            ToggleTheme(*s);
        } else if (ctrl && wParam == L'A') {
            tab->SelectAll();
        } else if (wParam == VK_ESCAPE) {
            tab->ClearSelection();
            ResetMarquee(*s);
            s->clickCollapseIndex = -1;
        } else if (wParam == VK_DOWN || wParam == VK_UP ||
                   wParam == VK_LEFT || wParam == VK_RIGHT) {
            CancelScrollAnimation(*s);
            {
                const int n = static_cast<int>(vm.pane.EntryCount());
                if (n > 0) {
                    int view = vm.pane.ViewIndex(tab->selected_index);
                    if (view < 0) view = 0;
                    const int dx = wParam == VK_LEFT ? -1 : (wParam == VK_RIGHT ? 1 : 0);
                    const int dy = wParam == VK_UP ? -1 : (wParam == VK_DOWN ? 1 : 0);
                    const int next = s->renderer.MoveViewIndex(vm.pane, FocusedPaneRect(*s),
                                                                view, dx, dy);
                    tab->MoveFocus(vm.pane.SourceIndex(next), shift);
                    EnsureRowVisible(*s, *tab, tab->selected_index);
                }
            }
        } else if (wParam == VK_NEXT) {
            CancelScrollAnimation(*s);
            {
                const int n = static_cast<int>(vm.pane.EntryCount());
                const int page = s->renderer.PageDelta(vm.pane, FocusedPaneRect(*s));
                if (n > 0) {
                    int view = vm.pane.ViewIndex(tab->selected_index);
                    if (view < 0) view = 0;
                    const int next = std::min(n - 1, view + page);
                    tab->MoveFocus(vm.pane.SourceIndex(next), shift);
                    EnsureRowVisible(*s, *tab, tab->selected_index);
                }
            }
        } else if (wParam == VK_PRIOR) {
            CancelScrollAnimation(*s);
            {
                const int n = static_cast<int>(vm.pane.EntryCount());
                const int page = s->renderer.PageDelta(vm.pane, FocusedPaneRect(*s));
                if (n > 0) {
                    int view = vm.pane.ViewIndex(tab->selected_index);
                    if (view < 0) view = 0;
                    const int next = std::max(0, view - page);
                    tab->MoveFocus(vm.pane.SourceIndex(next), shift);
                    EnsureRowVisible(*s, *tab, tab->selected_index);
                }
            }
        } else if (wParam == VK_HOME) {
            CancelScrollAnimation(*s);
            if (vm.pane.EntryCount() > 0) {
                tab->MoveFocus(vm.pane.SourceIndex(0), shift);
                tab->scroll_y = 0;
            }
        } else if (wParam == VK_END) {
            CancelScrollAnimation(*s);
            if (vm.pane.EntryCount() > 0) {
                tab->MoveFocus(vm.pane.SourceIndex(static_cast<int>(vm.pane.EntryCount()) - 1), shift);
                EnsureRowVisible(*s, *tab, tab->selected_index);
                s->scrollTargetY = tab->scroll_y;
                MaybePrefetchSearchPage(*s);
            }
        } else {
            handled = false;
        }

        if (handled) {
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_COMMAND: {
        if (s && wParam == 1001) {
            RefreshActiveTab(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_WORKER_RESULT: {
        if (s) {
            ProcessPendingResults(*s);
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_OPS_NOTIFY: {
        if (s) {
            ops::OpStatus st = s->ops.Status();
            UpdateOperationWindow(*s, true);
            if (s->ops.TakeCtxInvokeDone()) {
                if (app::Tab* tab = ActiveTab(*s)) s->store.MarkDirty(tab->current_path);
                RefreshActiveTab(*s);
            }
            if (st.completed_ops != s->opsCompleted) {
                s->opsCompleted = st.completed_ops;
                std::vector<std::wstring> tag_metadata_paths;
                for (const auto& completed : s->ops.DrainCompletions()) {
                    // Invalidate both sides of every successful mutation. The
                    // focused tab refreshes below; background tabs must not
                    // reuse a stale snapshot when they are shown later.
                    for (const auto& source : completed.sources) {
                        const std::wstring parent = fs::ParentPath(source);
                        if (!parent.empty()) s->store.MarkDirty(parent);
                    }
                    for (const auto& destination : completed.destinations) {
                        const std::wstring parent = fs::ParentPath(destination);
                        if (!parent.empty()) s->store.MarkDirty(parent);
                    }
                    if (completed.type == ops::OpType::Copy) {
                        for (size_t i = 0; i < completed.sources.size() &&
                                           i < completed.destinations.size(); ++i) {
                            s->places.CloneAssignments(completed.sources[i],
                                                       completed.destinations[i]);
                            tag_metadata_paths.push_back(completed.destinations[i]);
                        }
                    } else if (completed.type == ops::OpType::Move ||
                               completed.type == ops::OpType::Rename) {
                        for (size_t i = 0; i < completed.sources.size() &&
                                           i < completed.destinations.size(); ++i) {
                            s->places.RemapPaths(completed.sources[i],
                                                 completed.destinations[i]);
                            tag_metadata_paths.push_back(completed.destinations[i]);
                        }
                    } else if (completed.type == ops::OpType::RealDelete) {
                        for (const auto& source : completed.sources)
                            s->places.RemoveAssignments(source, true);
                    }
                }
                if (!tag_metadata_paths.empty())
                    QueueTagAds(*s, BuildTagAdsUpdates(s->places, tag_metadata_paths, true));
                // An op finished: refresh the view (watcher also fires, this is immediate).
                app::Tab* tab = ActiveTab(*s);
                if (tab) {
                    s->store.MarkDirty(tab->current_path);
                    RefreshActiveTab(*s);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_SHELLCTX_ITEMS: {
        auto* items = reinterpret_cast<std::vector<ops::ShellMenuItem>*>(lParam);
        const uint32_t token = static_cast<uint32_t>(wParam);
        if (s && items) {
            if (token == s->ctxToken) {
                s->ctxComItems = std::move(*items);
                s->ctxComReady = true;
                s->shellComCache[ComCacheKey(s->ctxBackground, s->ctxStaticExt)] = s->ctxComItems;
                RefreshOpenCtxMenu(*s);
            } else {
                // Result for a session we already abandoned; free the host thread.
                s->ops.CloseShellMenu(token);
            }
        }
        delete items;
        return 0;
    }

    case WM_SHELL_VERBS: {
        auto* result = reinterpret_cast<ShellVerbsResult*>(lParam);
        if (s && result) {
            s->shellVerbPending.erase(result->ext);
            s->shellVerbCache[result->ext] = result->verbs;
            if (s->ctxStaticExt == result->ext && s->ctxStaticVerbs.empty()) {
                s->ctxStaticVerbs = std::move(result->verbs);
                // Apply on the next open; do not grow an already-visible menu.
            }
        }
        delete result;
        return 0;
    }

    case WM_DETAILS_META: {
        auto* result = reinterpret_cast<DetailsMetaResult*>(lParam);
        if (s && result && result->path == s->detailsSelPath) {
            s->detailsMetaPath = result->path;
            s->detailsOwner = std::move(result->meta.owner);
            s->detailsPermissions = std::move(result->meta.permissions);
            s->detailsDrive = std::move(result->meta.drive);
            s->detailsFileSystem = std::move(result->meta.file_system);
            s->detailsFreeSpace = std::move(result->meta.free_space);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_INDEX_NOTIFY: {
        if (s) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_INDEX_SEARCH: {
        if (!s) return 0;
        const uint32_t id = static_cast<uint32_t>(wParam);
        index::SearchResult result;
        if (!s->index.TakeResult(id, result)) return 0;
        if (id == s->paletteSearchId) {
            s->paletteHits = std::move(result.hits);
            s->paletteTotal = result.total;
            s->paletteSearching = false;
            if (s->menu && s->menu->IsOpen()) s->menu->RequestFilterRefresh();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        for (auto& pane : s->panes) {
            for (auto& owned : pane->tabs) {
                app::Tab* tab = owned.get();
                if (!tab || tab->pending_generation != id) continue;
                std::wstring kind, rest;
                app::ParsePulsePath(tab->current_path, &kind, &rest);
                if (kind != L"search") continue;
                ApplySearchHits(*tab, rest, std::move(result));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        return 0;
    }

    case WM_NET_PROBE: {
        auto* result = reinterpret_cast<fs::UncProbeResult*>(lParam);
        if (s && result) {
            s->probeBusy = false;
            s->places.SetNetworkStatus(result->unc, result->status, result->rtt_ms);
            if (result->status == fs::NetStatus::Offline) {
                for (auto& pane : s->panes) {
                    for (auto& owned : pane->tabs) {
                        app::Tab* tab = owned.get();
                        if (!tab || tab->current_path != result->unc) continue;
                        if (tab->snapshot) {
                            tab->net_readonly = true;
                            tab->banner_title = L"离线";
                            tab->banner_message = L"只读浏览上次快照";
                        }
                    }
                }
            }
            PumpUncProbe(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_TAG_ADS_WARNING: {
        std::unique_ptr<std::vector<std::wstring>> volumes(
            reinterpret_cast<std::vector<std::wstring>*>(lParam));
        if (s && volumes) {
            for (const auto& volume : *volumes) {
                if (!s->tagFallbackVolumes.insert(volume).second) continue;
                if (app::Tab* tab = ActiveTab(*s)) {
                    tab->banner_title = L"标签已保存在本机";
                    tab->banner_message = volume + L" 不支持文件标签元数据；换电脑后可能不可见。";
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_TAG_ADS_DISCOVERED: {
        std::unique_ptr<std::vector<TagAdsDiscovery>> discoveries(
            reinterpret_cast<std::vector<TagAdsDiscovery>*>(lParam));
        if (s && discoveries) {
            const uint64_t before = s->places.TagRevision();
            for (const auto& discovery : *discoveries) {
                const std::wstring key = TagDiscoveryKey(discovery.path);
                s->tagAdsDiscoveryQueued.erase(key);
                s->tagAdsDiscoveryChecked.insert(key);
                s->places.MergeAdsRecords(discovery.path, discovery.records,
                                          discovery.legacy_names);
            }
            if (s->places.TagRevision() != before)
                InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DESTROY: {
        if (s) {
            if (s->watcher) s->watcher->Stop();
            s->index.Stop();
            s->worker.Stop();
            ShutdownDetailsSizeWalk(*s);

            if (s->dropTarget) {
                RevokeDragDrop(hwnd);
                s->dropTarget->Release();
                s->dropTarget = nullptr;
            }
            s->menu.reset();
            s->operationWindow.reset();
            if (s->hwndAddressEdit) {
                DestroyWindow(s->hwndAddressEdit);
                s->hwndAddressEdit = nullptr;
            }
            if (s->hwndRenameEdit) {
                DestroyWindow(s->hwndRenameEdit);
                s->hwndRenameEdit = nullptr;
            }
            if (s->hwndTagRenameEdit) {
                DestroyWindow(s->hwndTagRenameEdit);
                s->hwndTagRenameEdit = nullptr;
            }
            if (s->hwndFilterEdit) {
                DestroyWindow(s->hwndFilterEdit);
                s->hwndFilterEdit = nullptr;
            }
            if (s->editFont) {
                DeleteObject(s->editFont);
                s->editFont = nullptr;
            }
            if (s->editBrush) {
                DeleteObject(s->editBrush);
                s->editBrush = nullptr;
            }
            // Visual-regression runs must never overwrite the user's real
            // window, path, tray, or undo session.
            if (!s->shot.active && !s->menushot) {
                app::SessionSnapshot snap;
                WINDOWPLACEMENT wp{ sizeof(wp) };
                if (GetWindowPlacement(hwnd, &wp)) {
                    snap.window_rect = wp.rcNormalPosition;
                    snap.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
                }
                snap.dark = s->darkMode;
                app::Tab* tab = ActiveTab(*s);
                if (tab) snap.active_path = tab->current_path;
                snap.layout = static_cast<int>(s->layout);
                snap.focused_pane = std::max(0, SlotIndexOf(*s, s->pane));
                snap.target_pane = SlotIndexOf(*s, s->targetPane);
                if (s->root) {
                    std::vector<app::Pane*> vis;
                    s->root->CollectPanes(vis);
                    for (app::Pane* p : vis) {
                        app::Tab* t = p ? p->ActiveTab() : nullptr;
                        snap.pane_paths.push_back(t ? t->current_path : L"");
                        snap.pane_views.push_back(t ? t->view_mode : ui::ViewMode::Details);
                        snap.pane_column_dividers.push_back(
                            t ? t->details_column_dividers : std::array<float, 3>{});
                        // Full tab/group state (version 5).
                        app::PaneSessionSnapshot ps;
                        if (p) {
                            ps.active = static_cast<int>(std::min(
                                p->active_tab, p->tabs.empty() ? size_t(0) : p->tabs.size() - 1));
                            for (const auto& g : p->tab_groups) {
                                app::GroupSessionSnapshot gs;
                                gs.id = g.id;
                                gs.name = g.name;
                                gs.color_rgb = g.color_rgb;
                                gs.collapsed = g.collapsed;
                                ps.groups.push_back(std::move(gs));
                            }
                            for (const auto& owned : p->tabs) {
                                app::TabSessionSnapshot ts;
                                if (owned) {
                                    ts.path = owned->current_path;
                                    ts.pinned = owned->pinned;
                                    ts.group = owned->tab_group;
                                    ts.view = owned->view_mode;
                                }
                                ps.tabs.push_back(std::move(ts));
                            }
                        }
                        snap.pane_tabs.push_back(std::move(ps));
                    }
                }
                snap.tray = s->tray;
                snap.undo_json = s->ops.UndoToJson();
                snap.sidebar_collapsed = static_cast<int>(s->sidebarCollapsedMask);
                snap.details_panel = s->showDetailsPanel;
                snap.details_panel_width = static_cast<int>(std::lround(s->detailsPanelWidth));
                app::SaveSession(snap);
                s->places.Save();
                s->ctxMenuPrefs.Save();
                s->appPrefs.Save();
            }

            EnsureTrayIcon(*s, false);
            s->ops.Stop();

            s->renderer.SetIconNotifyWindow(nullptr);
            s->renderer.SetCompositor(nullptr);
            s->compositor.Shutdown();
            s->hwnd = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    __try {
        return WndProcImpl(hwnd, msg, wParam, lParam);
    } __except (CrashLog(GetExceptionCode(), "wndproc", msg,
                         GetExceptionInformation() && GetExceptionInformation()->ExceptionRecord
                             ? GetExceptionInformation()->ExceptionRecord->ExceptionAddress : nullptr),
                EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool WaitForShotReady(AppState& s) {
    auto deadline = s.shot.start + std::chrono::seconds(5);
    MSG msg{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ProcessPendingResults(s);
        app::Tab* tab = ActiveTab(s);
        if (tab && !tab->loading && tab->snapshot) {
            return true;
        }
        if (s.hwnd) {
            RedrawWindow(s.hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_INTERNALPAINT);
        }
        Sleep(20);
    }
    return false;
}

// No C++ objects with destructors here: SEH (__try/__except) forbids unwinding.
static int ShotModeMain(AppState& state, HWND hwnd) {
    bool ok = false;
    __try {
        WaitForShotReady(state);
        if (state.shot_details && state.pane && state.pane->ActiveTab() &&
            state.pane->ActiveTab()->snapshot &&
            !state.pane->ActiveTab()->snapshot->empty()) {
            // Select rows after enumeration so the panel has deterministic data.
            state.pane->ActiveTab()->SelectOnly(0);
            if (state.shot_details_multi) {
                const int count = static_cast<int>(state.pane->ActiveTab()->snapshot->size());
                for (int i = 1; i < std::min(3, count); ++i)
                    state.pane->ActiveTab()->ToggleSelect(i);
            }
        }
        MSG msg{};
        for (int i = 0; i < 20; ++i) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(40);
        }
        Render(state);
        ok = state.compositor.SaveSnapshot(state.shot.output.c_str());
    } __except (CrashLog(GetExceptionCode(), "shot"), EXCEPTION_EXECUTE_HANDLER) {
        return 2;
    }
    if (ok) {
        // Drop timing numbers next to the shot for the phase report.
        wchar_t timingPath[MAX_PATH]{};
        wcsncpy_s(timingPath, state.shot.output.c_str(), _TRUNCATE);
        if (wchar_t* dot = wcsrchr(timingPath, L'.')) *dot = L'\0';
        wcscat_s(timingPath, L".timing.txt");
        if (HANDLE f = CreateFileW(timingPath, GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); f != INVALID_HANDLE_VALUE) {
            char buf[256];
            int n = snprintf(buf, sizeof(buf),
                "path=%ls\nfirst_frame_ms=%.2f\nenum_sort_done_ms=%.2f\n",
                state.pane && state.pane->ActiveTab() ? state.pane->ActiveTab()->current_path.c_str() : L"",
                state.timing.first_frame_ms, state.timing.sort_done_ms);
            if (n > 0) {
                DWORD written = 0;
                WriteFile(f, buf, (DWORD)n, &written, nullptr);
            }
            CloseHandle(f);
        }
    }
    DestroyWindow(hwnd);
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return ok ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // OLE init (drag & drop + clipboard); implies STA COM init.
    OleInitialize(nullptr);
    std::set_terminate([] {
        CrashLog(0xE0000001, "terminate");
        // Best effort: flush and die.
        _exit(3);
    });

    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--material-selftest") == 0) {
            const int rc = ui::RunMaterialSelfTest();
            OleUninitialize();
            return rc;
        }
    }

    AppState state;

#ifdef PULSE_WITH_SELFTEST
    // Optional headless suite: excluded from production builds together with
    // the in-process index engine it exercises.
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--selftest") == 0) {
            int rc = app::RunSelfTest1B2();
            OleUninitialize();
            return rc;
        }
    }
#endif

    // Load previous session before parsing overrides.
    app::SessionSnapshot session;
    if (app::LoadSession(session)) {
        state.session_path = session.active_path;
        state.session_pane_paths = session.pane_paths;
        state.session_pane_views = session.pane_views;
        state.session_pane_columns = session.pane_column_dividers;
        state.session_pane_tabs = std::move(session.pane_tabs);
        state.session_layout = session.layout;
        state.session_focused = session.focused_pane;
        state.session_target = session.target_pane;
        state.tray = session.tray;
        state.darkMode = session.dark;
        state.sidebarCollapsedMask = static_cast<uint32_t>(session.sidebar_collapsed);
        state.pending_undo_json = session.undo_json;
        state.showDetailsPanel = session.details_panel;
        state.detailsPanelWidth = static_cast<float>(session.details_panel_width);
        state.renderer.SetDetailsPanelWidth(state.detailsPanelWidth);
        if (state.darkMode) state.themeOverride = ui::ThemeMode::Dark;
    }

    // Parse command line.
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--shot") == 0 && i + 1 < __argc) {
            state.shot.active = true;
            state.shot.output = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--menushot") == 0 && i + 1 < __argc) {
            state.menushot = true;
            state.menushot_out = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--shot-tray") == 0) {
            state.shot_tray = true;
            // Optional item count: --shot-tray 1 stages a single file.
            if (i + 1 < __argc && __wargv[i + 1][0] >= L'0' && __wargv[i + 1][0] <= L'9')
                state.shot_tray_count = std::max(1, _wtoi(__wargv[++i]));
        } else if (wcscmp(__wargv[i], L"--shot-tab-colors") == 0) {
            state.shot_tab_colors = true;
        } else if (wcscmp(__wargv[i], L"--shot-details") == 0) {
            state.shot_details = true;
        } else if (wcscmp(__wargv[i], L"--shot-details-multi") == 0) {
            state.shot_details = true;
            state.shot_details_multi = true;
        } else if (wcscmp(__wargv[i], L"--shot-scale") == 0 && i + 1 < __argc) {
            state.shot_scale_override = std::clamp(
                static_cast<float>(_wtof(__wargv[++i])), 1.0f, 2.0f);
        } else if (wcscmp(__wargv[i], L"--shot-high-contrast") == 0) {
            state.shot_high_contrast = true;
        } else if (wcscmp(__wargv[i], L"--dark") == 0) {
            state.shot.force_dark = true;
            state.themeOverride = ui::ThemeMode::Dark;
        } else if (wcscmp(__wargv[i], L"--light") == 0) {
            state.shot.force_dark = false;
            state.themeOverride = ui::ThemeMode::Light;
        } else if (wcscmp(__wargv[i], L"--fps") == 0) {
            state.showFps = true;
        } else if (wcscmp(__wargv[i], L"--view") == 0 && i + 1 < __argc) {
            state.shot.view_mode = ui::ParseViewMode(__wargv[++i]);
        } else if (wcscmp(__wargv[i], L"--size") == 0 && i + 1 < __argc) {
            int requestedWidth = 0;
            int requestedHeight = 0;
            if (swscanf_s(__wargv[++i], L"%dx%d", &requestedWidth, &requestedHeight) == 2) {
                state.shot.width = std::max(320, requestedWidth);
                state.shot.height = std::max(240, requestedHeight);
            }
        } else if (i == __argc - 1) {
            state.shot.path = __wargv[i];
        } else if (__wargv[i][0] != L'-' && state.shot.path.empty()) {
            state.shot.path = __wargv[i]; // tolerate path not being the last argument
        }
    }
    if (state.shot.active && state.shot.path.empty()) {
        state.shot.path = L"C:\\";
    }
    state.shot.start = std::chrono::steady_clock::now();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_PULSE));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_PULSE), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PulseMainWindow";
    RegisterClassExW(&wc);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = (int)(1600 * state.scale), h = (int)(960 * state.scale);
    if (session.window_rect.right > session.window_rect.left) {
        x = session.window_rect.left;
        y = session.window_rect.top;
        w = session.window_rect.right - session.window_rect.left;
        h = session.window_rect.bottom - session.window_rect.top;
    }
    if (state.shot.active && state.shot.width > 0 && state.shot.height > 0) {
        w = state.shot.width;
        h = state.shot.height;
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName,
        L"Pulse",
        // Pulse paints the entire title bar. WS_POPUP prevents Win32 from
        // restoring an overlapped caption, while the remaining styles retain
        // resizing, the system menu, min/max and Snap Layout behavior.
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        x, y, w, h,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) return 1;
    if (wc.hIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    if (wc.hIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    if (session.maximized) nCmdShow = SW_SHOWMAXIMIZED;
    ShowWindow(hwnd, state.shot.active ? SW_SHOWNORMAL : nCmdShow);
    UpdateWindow(hwnd);

    if (state.menushot) {
        // Render the built-in item menu to a PNG (GUI verification for 1B-2).
        ui::FluentMenu m;
        bool ok = m.Create(hwnd, &state.compositor, state.scale);
        if (ok) {
            m.SetTheme(state.darkMode, state.accentColor);
            auto debug_items = BuildFinderItemMenu(state, true, L"撤销移动 a.txt");
            for (auto& item : debug_items) {
                if (item.quick_swatches.empty()) continue;
                item.quick_swatches.front().checked = true;
                if (item.quick_swatches.size() > 1) item.quick_swatches[1].mixed = true;
                break;
            }
            // Sample Explorer section: a flat verb plus a software-owned
            // flyout group (renders the › chevron in the shortcut column).
            app::ShellMenuEntry group;
            group.text = L"Bandizip";
            group.children = { { app::CmdShellComBase + 1, L"压缩为 zip", true },
                               { app::CmdShellComBase + 2, L"用 Bandizip 打开", true } };
            app::AppendShellSection(debug_items,
                { { app::CmdShellStaticBase + 0, L"打印", true }, group });
            ok = m.SaveDebugSnapshot(state.menushot_out.c_str(), std::move(debug_items));
        }
        DestroyWindow(hwnd);
        OleUninitialize();
        return ok ? 0 : 1;
    }

    if (state.shot.active) {
        if (state.shot_tray) {
            // Stage a few real files from the shot folder so the fan deck is
            // visible in the verification screenshot. Drop any restored
            // session tray first so the shot is deterministic.
            state.tray.Clear();
            std::vector<std::wstring> staged;
            const std::wstring root = fs::NormalizePath(state.shot.path);
            WIN32_FIND_DATAW fd{};
            HANDLE hFind = FindFirstFileW((root + L"\\*").c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                    if (fd.cFileName[0] == L'.') continue;
                    staged.push_back(root + L"\\" + fd.cFileName);
                    if (staged.size() >= static_cast<size_t>(state.shot_tray_count)) break;
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
            if (!staged.empty()) state.tray.Collect(staged, false);
        }
        if (state.shot_tab_colors && state.pane) {
            NewTab(state, state.shot.path);
            NewTab(state, state.shot.path);
            if (state.pane->tabs.size() >= 3) {
                // Group 1: tabs 0-1, red, named. Group 2: tab 2, blue, unnamed.
                app::TabGroup g1; g1.id = 1; g1.name = L"设计"; g1.color_rgb = 0xE74856;
                app::TabGroup g2; g2.id = 2; g2.color_rgb = 0x0078D4;
                state.pane->tab_groups.push_back(g1);
                state.pane->tab_groups.push_back(g2);
                state.pane->next_tab_group_id = 3;
                state.pane->tabs[0]->tab_group = 1;
                state.pane->tabs[1]->tab_group = 1;
                state.pane->tabs[2]->tab_group = 2;
                state.pane->active_tab = 1; // active grouped tab in the middle
            }
        }
        if (state.shot_details) {
            state.showDetailsPanel = true;
            state.renderer.SetDetailsPanelVisible(true);
        }
        int rc = ShotModeMain(state, hwnd);
        OleUninitialize();
        return rc;
    }

    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    return (int)msg.wParam;
}
