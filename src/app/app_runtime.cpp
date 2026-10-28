// app_runtime.cpp — extracted from app_main.cpp.
#include "app_window_title.h"
#include "../fs/drive_kind.h"
#include "../fs/device_path.h"
#include "../common/shell_id.h"
#include "device_registry.h"
#include "folder_alias.h"
#include "app_wall_hint.h"
#include "app_internal.h"
#include "app_network_page.h"
#include "shortcut_page.h"
#include "keymap.h"
#include "../ui/ui_renderer.h"
#include <memory>
#include <thread>
#include <shlobj.h>
#include <unordered_map>
#include "system_ctx_model.h"
#include "pane_layout.h"
#include "tab_spring.h"
#include "vertical_tabs.h"
#include "app_column_view.h"
#include "git_ui.h"
#include "sync_status_ui.h"
#include "mtime_hint_prefs.h"
#include "app_details_tree.h"
#include "folder_sizes_ui.h"
#include "update_status.h"
#include "about_info.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/link_type_text.h"
#include "../ui/fluent_menu.h"
#include "../ui/preview_format_catalog.h"
#include "../ui/drag_drop.h"
#include "../ui/toolbar_picker.h"
#include "toolbar_apps.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../fs/cloud_state.h"
#include "cloud_download.h"
#include "cloud_roots.h"
#include "../ui/typography.h"
#include "../ui/ai_list_overlay.h"
#include "../ui/settings_page_ids.h"
#include "../ui/color_picker.h"
#include "filter_tag_complete.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/display_path.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "dir_notify_queue.h"
#include "text_diff.h"
#include "content_pack.h"
#include "content_pack_ui.h"
#include "ai_pack.h"
#include "ai_pack_ui.h"
#include "ai_settings_ui.h"
#include "ai_assistant.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "duplicate_scan.h"
#include "search_query.h"
#include "resource.h"
#include "pulse_version.h"
#include "tray_reveal.h"
#include "favorites_bar.h"
#include "default_file_manager.h"
#include "window_identity_ui.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include "settings_transfer_ui.h"
#include <thread>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <unordered_set>

using namespace pulse;

namespace {

struct DetailsMetaRequest {
    HWND hwnd = nullptr;
    std::wstring path;
};

class DetailsMetaWorker {
public:
    void Submit(HWND hwnd, std::wstring path) {
        if (!hwnd || path.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = DetailsMetaRequest{hwnd, std::move(path)};
            if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
        }
        cv_.notify_one();
    }

private:
    void Run() {
        for (;;) {
            DetailsMetaRequest request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return pending_.has_value(); });
                request = std::move(*pending_);
                pending_.reset();
            }
            auto* result = new DetailsMetaResult{};
            result->path = request.path;
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(request.path.c_str(), GetFileExInfoStandard, &fad)) {
                result->attrs_valid = true;
                result->created = fad.ftCreationTime;
                result->modified = fad.ftLastWriteTime;
                result->accessed = fad.ftLastAccessTime;
            }
            SHFILEINFOW sfi{};
            const std::wstring shell_path = path::StripExtendedPathPrefix(request.path);
            if (SHGetFileInfoW(shell_path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME) &&
                sfi.szTypeName[0]) {
                result->type_name = sfi.szTypeName;
            }
            result->meta = app::FetchDetailsMeta(request.path);
            if (!PostMessageW(request.hwnd, WM_DETAILS_META, 0, reinterpret_cast<LPARAM>(result)))
                delete result;
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<DetailsMetaRequest> pending_;
    std::thread thread_;
};

DetailsMetaWorker& GetDetailsMetaWorker() {
    // The worker is intentionally process-lifetime: Windows may leave a Shell
    // metadata call blocked on an offline provider during window teardown.
    static auto* worker = new DetailsMetaWorker();
    return *worker;
}

void RefreshDuplicateGroupViews(AppState& s) {
    constexpr size_t kMaxGroups = 80;
    constexpr size_t kMaxFiles = 40;
    s.dup_view_cache.clear();
    const size_t group_n = (std::min)(s.duplicateScan.groups.size(), kMaxGroups);
    s.dup_view_cache.reserve(group_n);
    for (size_t g = 0; g < group_n; ++g) {
        const auto& group = s.duplicateScan.groups[g];
        ui::DuplicateGroupView view;
        wchar_t title[128]{};
        swprintf_s(title, l10n::Get(l10n::StringId::DupGroupFormat).c_str(),
                   format::ByteSize(group.size).c_str(),
                   static_cast<int>(group.files.size()));
        view.title = title;
        const size_t file_n = (std::min)(group.files.size(), kMaxFiles);
        view.files.reserve(file_n);
        for (size_t f = 0; f < file_n; ++f) {
            ui::DuplicateFileView file;
            file.name = group.files[f].name;
            file.path = group.files[f].path;
            FILETIME time{};
            time.dwLowDateTime = static_cast<DWORD>(group.files[f].modified);
            time.dwHighDateTime = static_cast<DWORD>(group.files[f].modified >> 32);
            file.detail = group.files[f].path + L" · " + format::LocalFileTime(time);
            file.keep = f == group.keep_index;
            view.files.push_back(std::move(file));
        }
        s.dup_view_cache.push_back(std::move(view));
    }
    s.dup_view_epoch = s.duplicateScan.result_epoch;
    s.dup_view_language = s.appPrefs.language;
}

} // namespace

namespace pulse {
namespace {
app::UpdateProgress UpdateProgressForView(const AppState& s) {
    if (s.shot.active) {
        using app::UpdatePhase;
        const auto& phase = s.shot.update_state;
        if (phase == L"connecting") return {UpdatePhase::Connecting};
        if (phase == L"downloading") return {UpdatePhase::Downloading, 3 * 1024 * 1024, 8 * 1024 * 1024};
        if (phase == L"downloading-unknown") return {UpdatePhase::Downloading, 3 * 1024 * 1024, 0};
        if (phase == L"verifying") return {UpdatePhase::Verifying};
        if (phase == L"waiting") return {UpdatePhase::WaitingOperations};
        if (phase == L"launching") return {UpdatePhase::Launching};
        if (phase == L"installing") return {UpdatePhase::Installing};
        return {};
    }
    return s.update_installer.Progress();
}
}

void PrefetchDetailsMeta(HWND hwnd, const std::wstring& path) {
    GetDetailsMetaWorker().Submit(hwnd, path);
}
void RememberLayoutFocus(AppState& s) {
    for (auto& owned : s.window_tabs.items) {
        if (!owned) continue;
        int focused = owned->focused_index;
        bool saw_focus = false;
        int target_here = -1;
        for (size_t i = 0; i < owned->panes.size(); ++i) {
            if (owned->panes[i].get() == s.pane) {
                focused = static_cast<int>(i);
                saw_focus = true;
            }
            if (owned->panes[i].get() == s.targetPane)
                target_here = static_cast<int>(i);
        }
        if (saw_focus) owned->focused_index = focused;
        if (s.targetPane) {
            if (target_here >= 0) owned->target_index = target_here;
        } else if (saw_focus) {
            owned->target_index = -1;
        }
    }
}

std::vector<std::wstring> VisibleFolderPaths(const AppState& s) {
    std::vector<std::wstring> paths;
    const app::LayoutTab* tab = s.window_tabs.Active();
    if (!tab) return paths;
    std::vector<app::Pane*> vis;
    if (tab->root) tab->root->CollectPanes(vis);
    else {
        vis.reserve(tab->panes.size());
        for (const auto& pane : tab->panes) {
            if (pane) vis.push_back(pane.get());
        }
    }
    for (app::Pane* pane : vis) {
        const app::Tab* view = pane ? pane->ActiveTab() : nullptr;
        if (!view || view->current_path.empty() || fs::IsVirtualPath(view->current_path))
            continue;
        paths.push_back(fs::NormalizePath(view->current_path));
        AppendDetailsTreeWatchPaths(*view, paths);
    }
    return paths;
}

void SyncVisibleWatches(AppState& s) {
    // The watches are shared: they cover what every window shows (M6e), and
    // report to the shared queue, not to a window that may close first.
    std::vector<std::wstring> paths;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        for (std::wstring& path : VisibleFolderPaths(window)) paths.push_back(std::move(path));
    });
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    AppCore& core = s.core;
    const HWND service = ServiceWindow(s);
    s.watches.Sync(paths, [&core, service](const std::wstring& path, bool overflow, std::vector<fs::DirNotifyEvent> events) {
        std::lock_guard<std::mutex> lock(core.notify_mu);
        const bool first = core.notify_queue.empty();
        app::QueueDirNotify(core.notify_queue, path, overflow, std::move(events));
        // The tick drains the whole queue, so one wake per batch is enough.
        if (first) PostMessageW(service, WM_UI_KICK, 0, 0);
    });
}

void BindCurrentLayout(AppState& s) {
    // Settings has no tab in the strip (its title-bar button stands in): once
    // another tab is active the settings tab goes away. Page, scroll and
    // expanded cards live in s.settings, so reopening comes back the same.
    if (!s.tabDragging) {
        for (size_t i = s.window_tabs.items.size(); i-- > 0;) {
            app::LayoutTab& layout = *s.window_tabs.items[i];
            if (i == s.window_tabs.active || layout.pinned || layout.panes.size() != 1 ||
                s.window_tabs.items.size() <= 1 || !IsSettingsTab(layout.ActiveFolder()))
                continue;
            s.window_tabs.CloseTab(i);
        }
    }
    app::LayoutTab* tab = s.window_tabs.Active();
    if (!tab || tab->panes.empty()) {
        s.pane = nullptr;
        s.targetPane = nullptr;
        SyncVisibleWatches(s);
        SyncTaskbarWindowTitle(s);
        return;
    }
    tab->focused_index = std::clamp(tab->focused_index, 0,
                                    static_cast<int>(tab->panes.size()) - 1);
    s.pane = tab->panes[static_cast<size_t>(tab->focused_index)].get();
    if (tab->target_index >= 0 &&
        tab->target_index < static_cast<int>(tab->panes.size())) {
        s.targetPane = tab->panes[static_cast<size_t>(tab->target_index)].get();
    } else {
        s.targetPane = nullptr;
    }
    if (!tab->root) app::RebuildLayoutRoot(*tab);
    for (size_t i = 0; i < tab->panes.size(); ++i) {
        tab->panes[i]->focused = (static_cast<int>(i) == tab->focused_index);
        tab->panes[i]->target = (tab->panes[i].get() == s.targetPane);
    }
    if (app::Tab* view = s.pane->ActiveTab()) {
        s.scrollTargetY = view->scroll_y;
        s.scrollAnimating = false;
    }
    SyncVisibleWatches(s);
    SyncTaskbarWindowTitle(s);
}
// Live text of the rename field; icon grids size the field to wrap it.
// Inline rename (P1f): the note under the field and the width it has grown to.
static void FillRenameHint(const AppState& s, ui::PaneViewModel& pane) {
    pane.rename_field_w = s.renameIndex >= 0 ? s.renameFieldW : 0.0f;
    const ULONGLONG now = GetTickCount64();
    if (s.renameIndex >= 0 && s.renameShakeAt != 0 && now - s.renameShakeAt < 360)
        pane.rename_shake_ms = static_cast<int>(now - s.renameShakeAt);
    if (s.renameIndex < 0 || s.renameHint.empty()) return;
    if (s.renameHintUntil != 0 && now >= s.renameHintUntil) return;
    pane.rename_hint = s.renameHint;
    pane.rename_hint_ms = s.renameHintUntil == 0 ? -1 : static_cast<int>(s.renameHintUntil - now);
}

static std::wstring RenameEditText(const AppState& s) {
    if (s.renameIndex < 0 || !s.hwndRenameEdit || !IsWindow(s.hwndRenameEdit)) return {};
    const int len = GetWindowTextLengthW(s.hwndRenameEdit);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    text.resize(static_cast<size_t>(std::max(0, GetWindowTextW(s.hwndRenameEdit, text.data(), len + 1))));
    return text;
}
// Whether a launch argument names a file, without stalling the UI thread on
// a share (or mapped drive) that does not answer: GetFileAttributes on an unreachable server
// blocks for ~13 s, and startup asked twice (ResolveOpenFolderPath, then
// SelectLaunchedFile), so the window took ~27 s to appear. A share gets a
// second; past that the path opens as a folder and the listing says why.
static DWORD QueryLaunchArgumentAttributes(const std::wstring& path);

// One external open asks up to three times (OpenExternalFolder, then
// OpenFolderInNewTab and SelectLaunchedFile): the answer is kept for a few
// seconds so a share that does not answer costs one wait, not three.
// A drive root is a folder without asking: the first access to a sleeping
// disk waits for it to spin up.
static DWORD LaunchArgumentAttributes(const std::wstring& path) {
    if (path.empty()) return INVALID_FILE_ATTRIBUTES;
    if ((path.size() == 2 || (path.size() == 3 && (path[2] == L'\\' || path[2] == L'/'))) && path[1] == L':' &&
        (path[0] | 0x20) >= L'a' && (path[0] | 0x20) <= L'z')
        return FILE_ATTRIBUTE_DIRECTORY;
    struct Cached { std::wstring path; DWORD attrs = INVALID_FILE_ATTRIBUTES; ULONGLONG tick = 0; };
    thread_local Cached cached;
    const ULONGLONG now = GetTickCount64();
    if (cached.tick && now - cached.tick < 3000 && cached.path == path) return cached.attrs;
    const DWORD attrs = QueryLaunchArgumentAttributes(path);
    cached = {path, attrs, GetTickCount64()};
    return attrs;
}

static DWORD QueryLaunchArgumentAttributes(const std::wstring& path) {
    const bool remote = fs::IsUncPath(path) || fs::IsRemoteLikeDrive(path);
    if (!remote) return GetFileAttributesW(path.c_str());
    struct Query {
        std::wstring path;
        DWORD attrs = INVALID_FILE_ATTRIBUTES;
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Query() { if (done) CloseHandle(done); }
    };
    auto query = std::make_shared<Query>();
    query->path = path;
    if (!query->done) return INVALID_FILE_ATTRIBUTES;
    try {
        std::thread([query] {
            query->attrs = GetFileAttributesW(query->path.c_str());
            SetEvent(query->done);
        }).detach();
    } catch (...) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return WaitForSingleObject(query->done, 1000) == WAIT_OBJECT_0 ? query->attrs : INVALID_FILE_ATTRIBUTES;
}

std::wstring ResolveOpenFolderPath(std::wstring path) {
    while (!path.empty() && (path.front() == L'"' || path.back() == L'"')) {
        if (path.front() == L'"') path.erase(path.begin());
        if (!path.empty() && path.back() == L'"') path.pop_back();
    }
    if (path.empty()) return {};
    const DWORD attrs = LaunchArgumentAttributes(path);
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        path = fs::ParentPath(path);
    }
    return fs::NormalizePath(path);
}

void SelectLaunchedFile(AppState& s, const std::wstring& raw) {
    std::wstring path = raw;
    while (!path.empty() && (path.front() == L'"' || path.back() == L'"')) {
        if (path.front() == L'"') path.erase(path.begin());
        if (!path.empty() && path.back() == L'"') path.pop_back();
    }
    const DWORD attrs = LaunchArgumentAttributes(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) return;
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (leaf.empty()) return;
    if (app::Tab* tab = ActiveTab(s)) SelectNameInTab(s, *tab, leaf);
}

void OpenFolderInNewTab(AppState& s, const std::wstring& raw) {
    if (app::IsThisPcArgument(raw)) {
        // The This PC verb (设为默认文件管理器): This PC is the empty path,
        // which only OpenTabAt keeps (NewTab turns it into C:\). FindFolderTab
        // skips it, so each request opens a tab, as Explorer opens a window.
        if (!IsWindowVisible(s.hwnd) && TakeFreshStart(s)) StartFreshAt(s, L"");
        else OpenTabAt(s, L"");
        RevealWindow(s);
        return;
    }
    const std::wstring path = ResolveOpenFolderPath(raw);
    // Back from the tray with "open the default location" at startup: an
    // explicit folder starts over on its own, as launching with it would.
    if (!path.empty() && !IsWindowVisible(s.hwnd) && TakeFreshStart(s)) StartFreshAt(s, path);
    RevealWindow(s);
    if (!path.empty() && !ActivateExistingFolderTab(s, path)) NewTab(s, path);
    else InvalidateRect(s.hwnd, nullptr, FALSE);
    // A file (Pulse as a file's default app, a launcher's "open containing
    // folder") shows up selected instead of silently opening its folder (#40).
    if (!path.empty()) SelectLaunchedFile(s, raw);
}

void PostWorkerResult(AppState& s, app::WorkResult res) {
    {
        std::lock_guard<std::mutex> lock(s.resultMutex);
        s.results.push(std::move(res));
    }
    PostMessageW(ServiceWindow(s), WM_WORKER_RESULT, 0, 0);
}
D2D1_RECT_F FocusedPaneRect(const AppState& s) {
    D2D1_RECT_F content = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    return app::FocusedWindowPaneRect(Root(s).get(), s.pane, content, s.scale);
}

app::Pane* PaneAtSlot(AppState& s, int index) {
    if (!Root(s)) return s.pane;
    std::vector<app::Pane*> visible;
    Root(s)->CollectPanes(visible);
    if (index >= 0 && index < static_cast<int>(visible.size())) return visible[static_cast<size_t>(index)];
    return s.pane;
}

// Defined further below; the details-panel fill in BuildVm needs it.
std::wstring EntryFullPath(const app::Tab& tab, int index);

D2D1_RECT_F ListRect(const AppState& s) {
    D2D1_RECT_F pane = FocusedPaneRect(s);
    float extra = 0.0f;
    const app::Tab* tab = s.pane ? s.pane->ActiveTab() : nullptr;
    if (tab && !tab->banner_message.empty()) {
        // The strip grows to two lines when its message is long.
        const float drawn = s.renderer.FocusedPaneBannerHeight();
        extra = drawn > 0.0f ? drawn : 36.0f * s.scale;
    }
    std::wstring virtual_kind;
    if (tab && app::ParsePulsePath(tab->current_path, &virtual_kind, nullptr) &&
        virtual_kind == L"recent") {
        extra += 40.0f * s.scale;
    }
    if (virtual_kind == L"changes") extra += 36.0f * s.scale;
    const ui::ViewMode mode = tab ? tab->view_mode : ui::ViewMode::Details;
    if (tab && tab->column_layout) {
        ui::PaneViewModel strip;
        app::FillColumnStripView(strip.column_strip, *tab);
        pane = s.renderer.PaneBodyBounds(strip, pane);
    }
    pane.top += s.renderer.PaneHeaderHeight() + extra +
                (ui::ShowsColumnHeader(mode) ? s.renderer.ColumnHeaderHeight() : 0.0f);
    return pane;
}
bool ScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
                              D2D1_RECT_F& track, D2D1_RECT_F& thumb, float& maxScroll) {
    return s.renderer.PaneScrollbarGeometry(pane, FocusedPaneRect(s), track, thumb, maxScroll);
}

bool HorizontalScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
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

void RememberPath(AppState& s, const std::wstring& path) {
    if (path.empty() || fs::IsVirtualPath(path)) return;
    std::wstring n = fs::NormalizePath(path);
    if (n.empty()) return;
    s.places.RecordVisit(n);
}

void RecordRecentOpen(AppState& s, const std::wstring& path,
                             app::PlaceItemKind kind) {
    s.places.RecordRecent(path, kind);
}
// Drag spring-loading: how far the 800 ms rest on the folder row under the
// drag has come (the ring around its icon); <0 when no folder is arming.
static float DropSpringProgress(const AppState& s) {
    if (s.springRow < 0 || s.springRow != s.dropRow || s.springEntered || !s.springStart) return -1.0f;
    return (std::min)(1.0f, static_cast<float>(GetTickCount64() - s.springStart) / 800.0f);
}

// Landing: rows a drag just dropped into `dir`, hidden while its cards fly
// to them. Time-bounded, so rows that show up late are simply shown.
static void FillLandingRows(const AppState& s, const app::Pane* owner, const std::wstring& dir,
                            ui::PaneViewModel& pane) {
    const auto& l = s.dragLanding;
    if (l.names.empty() || dir.empty() || _wcsicmp(dir.c_str(), l.dir.c_str()) != 0) return;
    if (l.pane && l.pane != owner) return;   // another pane showing the same folder
    const uint64_t now = ui::motion::NowMs();
    if (l.reveal_at ? now > l.reveal_at + 200 : now - l.since > 750) return;
    pane.landing_names.insert(l.names.begin(), l.names.end());
    pane.landing_reveal_at = l.reveal_at;
}

// Motion 5b: rows of `dir` that are on their way to the Recycle Bin.
static void FillLeavingRows(const AppState& s, const std::wstring& dir, ui::PaneViewModel& pane) {
    if (s.leavingPaths.empty() || dir.empty()) return;
    const uint64_t now = ui::motion::NowMs();
    if (s.leavingDoneAt ? now - s.leavingDoneAt > 800 : now - s.leavingSince > 8000) return;
    for (const auto& path : s.leavingPaths) {
        if (fs::ParentPath(path) != dir) continue;
        const size_t slash = path.find_last_of(L"\\/");
        pane.leaving_names.insert(slash == std::wstring::npos ? path : path.substr(slash + 1));
    }
    if (!pane.leaving_names.empty()) pane.leaving_since = s.leavingSince;
}

// Motion v2 "layout": panes added or removed by a layout change glide from
// where they were drawn (a new pane grows in from its outer edge). The same
// content area only: a window resize or sidebar/details motion snaps.
static void AnimatePaneLayout(AppState& s, const D2D1_RECT_F& content,
                              std::vector<std::pair<app::Pane*, D2D1_RECT_F>>& laid,
                              std::vector<app::SplitterLayout>& splitters) {
    auto& m = s.paneLayoutMotion;
    const auto same_rect = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
        return std::fabs(a.left - b.left) < 0.5f && std::fabs(a.top - b.top) < 0.5f &&
               std::fabs(a.right - b.right) < 0.5f && std::fabs(a.bottom - b.bottom) < 0.5f;
    };
    const uint64_t now = ui::motion::NowMs();
    if (laid.size() != m.last.size() && !m.last.empty() && same_rect(content, m.content) && !s.splitterDragging &&
        ui::motion::SystemAnimationsEnabled()) {
        m.from = m.last;
        m.start = now;
    }
    if (!same_rect(content, m.content)) m.start = 0;
    m.content = content;
    if (m.start) {
        const float ms = static_cast<float>(now - m.start);
        if (ms >= static_cast<float>(ui::motion::SpringSettleMs(ui::motion::spring::Smooth))) {
            m.start = 0;
        } else {
            const float p = ui::motion::SpringProgress(ui::motion::spring::Smooth, ms);
            const D2D1_RECT_F first_final = laid.empty() ? D2D1_RECT_F{} : laid[0].second;
            for (auto& [pane, r] : laid) {
                D2D1_RECT_F f = r;
                bool found = false;
                for (const auto& [key, rect] : m.from)
                    if (key == pane) { f = rect; found = true; break; }
                if (!found) {
                    if (r.left > content.left + 1.0f) f = D2D1::RectF(r.right, r.top, r.right, r.bottom);
                    else if (r.top > content.top + 1.0f) f = D2D1::RectF(r.left, r.bottom, r.right, r.bottom);
                    else f = D2D1::RectF(r.left, r.top, r.left, r.bottom);
                }
                r = D2D1::RectF(std::round(f.left + (r.left - f.left) * p), std::round(f.top + (r.top - f.top) * p),
                                std::round(f.right + (r.right - f.right) * p),
                                std::round(f.bottom + (r.bottom - f.bottom) * p));
            }
            // One splitter between two panes follows the first pane's edge;
            // anything more involved hides its splitters until settled.
            if (splitters.size() == 1 && laid.size() == 2) {
                auto& sp = splitters[0];
                const bool vertical = sp.orientation == app::SplitOrientation::Vertical;
                const float d = vertical ? laid[0].second.right - first_final.right
                                         : laid[0].second.bottom - first_final.bottom;
                if (vertical) { sp.hit_rect.left += d; sp.hit_rect.right += d; }
                else { sp.hit_rect.top += d; sp.hit_rect.bottom += d; }
            } else {
                splitters.clear();
            }
        }
    }
    m.last.clear();
    for (const auto& [pane, r] : laid) m.last.emplace_back(pane, r);
}

void FillPaneSlots(AppState& s, ui::WindowViewModel& vm) {
    D2D1_RECT_F content = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    std::vector<std::pair<app::Pane*, D2D1_RECT_F>> laid;
    std::vector<app::SplitterLayout> splitters;
    if (Root(s)) app::LayoutWindowPanes(*Root(s), content, s.scale, laid, &splitters);
    AnimatePaneLayout(s, content, laid, splitters);
    vm.pane_slots.clear();
    vm.splitters.clear();
    s.splitterNodes.clear();
    vm.filter_editing = s.filterEditing;
    if (app::Tab* tab = ActiveTab(s)) {
        std::wstring kind, rest;
        if (app::ParsePulsePath(tab->current_path, &kind, &rest) && kind == L"settings") {
            vm.settings_open = true;
            vm.settings_page = app::SettingsController::PageFromName(rest);
            vm.settings_scroll = s.settings.scroll();
            vm.settings_launch_on_startup = s.appPrefs.launch_on_startup;
            vm.settings_start_in_tray = s.appPrefs.start_in_tray;
            vm.settings_keep_running = s.appPrefs.keep_running_on_close;
            vm.settings_show_hidden_files = s.appPrefs.show_hidden_files;
            vm.settings_show_protected_os_files = s.appPrefs.show_protected_os_files;
            vm.settings_search_pinyin = s.appPrefs.search_pinyin;
            vm.settings_search_enabled = s.appPrefs.search_enabled;
            vm.settings_global_search_enabled = s.appPrefs.global_search_enabled;
            vm.settings_global_search_double_ctrl = s.appPrefs.global_search_double_ctrl;
            vm.settings_global_search_double_ctrl_ms = s.appPrefs.global_search_double_ctrl_ms;
            vm.settings_dialog_jump_bar = s.appPrefs.dialog_jump_bar;
            vm.settings_dialog_jump_auto = s.appPrefs.dialog_jump_auto;
            vm.settings_jump_history = s.appPrefs.jump_history;
            vm.settings_jump_history_empty = s.core.jumpHistory->Size() == 0;
            vm.settings_global_search_apps = s.appPrefs.global_search_apps;
            vm.settings_global_search_capturing = s.settings.global_search_hotkey_capturing();
            vm.settings_global_search_hotkey = s.settings.GlobalSearchHotkeyText();
            vm.settings_global_search_error = s.settings.global_search_error();
            vm.settings_content_status = ContentIndexStatusText(s);
            // Search off hides the index maintenance details on the search page.
            vm.settings_expanded = s.appPrefs.search_enabled ? s.settingsExpanded : (s.settingsExpanded & ~2u);
            FillShortcutsVm(s, vm);  // 快捷键: sidebar badge everywhere, the page view on page 7
            if (vm.settings_page == ui::kSettingsPageAi) pulse::FillAiSettingsVm(s, vm);   // 增强包 > AI 与 MCP
            if (vm.settings_page == 5) vm.settings_preview_codecs = ui::DetectPreviewCodecs(false, s.hwnd);
            if (vm.settings_page == 9) {
                const auto cache = s.settings.ThumbCache(s.hwnd);
                vm.settings_thumb_cache_phase = static_cast<uint8_t>(cache.phase);
                vm.settings_thumb_cache_bytes = cache.bytes;
                vm.settings_thumb_cache_cleared = cache.cleared;
            }
            if (vm.settings_page == 6) {
                // 系统右键菜单: the first visit starts a background scan.
                auto& sysctx = s.settings.SystemCtx();
                sysctx.SetNotifyWindow(s.hwnd);
                sysctx.EnsureLoaded();
                // 概览: Pulse's own handler timings, with the ones it skips.
                std::vector<ui::SysCtxHandlerTiming> timings;
                for (auto& [clsid, ms] : s.context_menu.ComHandlerTimes()) {
                    ui::SysCtxHandlerTiming timing;
                    timing.pulse_skipped = s.ctxMenuPrefs.ComDisabled(ipc::HandlerCatalogKey(clsid));
                    timing.clsid = std::move(clsid);
                    timing.ms = ms;
                    timings.push_back(std::move(timing));
                }
                sysctx.SetHandlerTimings(std::move(timings));
                // What each extension adds, so its row reads as menu text.
                sysctx.SetHandlerLabels(s.context_menu.ComHandlerLabels());
                // 只在 Pulse 隐藏: the rows' Pulse switch reads ContextMenuPrefs.
                sysctx.SetPulseHidden(s.ctxMenuPrefs.HiddenCatalogKeys());
                vm.settings_sysctx = sysctx.View();
            }
            {
                // 增强包 nav badge, on every page (the controller caches 2 s).
                const auto& packs = s.settings.Packs();
                vm.settings_pack_updates = int(!packs.update_version.empty()) + int(!packs.images_update_version.empty()) +
                                           int(!packs.raw_update_version.empty()) + int(!packs.archive_update_version.empty());
            }
            if (vm.settings_page == 8) {
                // 内容搜索 module: its install directory is read at most every 2 s.
                static uint64_t checked_at = 0;
                static int content_state = static_cast<int>(app::ContentPackState::Missing);
                const uint64_t now = GetTickCount64();
                if (!checked_at || now - checked_at >= 2000) {
                    content_state = static_cast<int>(app::QueryContentPackState());
                    checked_at = now;
                }
                vm.settings_content_pack_state = content_state;
                // The AI pack likewise, and again as soon as a download ends.
                static uint64_t ai_checked_at = 0;
                static bool ai_was_installing = false;
                static int ai_state = static_cast<int>(app::AiPackState::Missing);
                const bool ai_installing = AiPackInstalling();
                if (!ai_checked_at || now - ai_checked_at >= 2000 || ai_installing != ai_was_installing) {
                    ai_state = static_cast<int>(app::QueryAiPackState());
                    ai_checked_at = now;
                    ai_was_installing = ai_installing;
                }
                vm.settings_ai_pack_state = ai_state;
                vm.settings_ai_pack_installing = ai_installing;
                vm.settings_ai_pack_progress = ai_installing ? AiPackInstallProgress() : 0.0f;
            }
            if (vm.settings_page == 5 || vm.settings_page == 8) {
                // Cached in the controller: the disk is read at most every 2 s.
                const auto& packs = s.settings.Packs();
                vm.settings_pack_ffmpeg = packs.ffmpeg;
                vm.settings_pack_media_available = packs.media_available;
                vm.settings_pack_images_available = packs.images_available;
                vm.settings_pack_media_installed = packs.media_installed;
                vm.settings_pack_ffmpeg_enabled = packs.enabled;
                vm.settings_pack_use_custom = packs.use_custom;
                vm.settings_pack_remove_on_uninstall = packs.remove_on_uninstall;
                vm.settings_pack_auto_update = packs.auto_update;
                vm.settings_pack_installed = packs.installed;
                vm.settings_pack_bytes = packs.bytes;
                vm.settings_pack_version = packs.version;
                vm.settings_pack_custom_path = packs.custom_path;
                vm.settings_pack_detected_path = packs.detected_path;
                vm.settings_pack_root = packs.root;
                vm.settings_pack_notice = packs.notice;
                vm.settings_pack_installing = packs.installing;
                vm.settings_pack_progress = packs.progress;
                vm.settings_pack_update_version = packs.update_version;
                vm.settings_pack_images_installed = packs.images_installed;
                vm.settings_pack_images_enabled = packs.images_enabled;
                vm.settings_pack_images_version = packs.images_version;
                vm.settings_pack_images_notice = packs.images_notice;
                vm.settings_pack_images_installing = packs.images_installing;
                vm.settings_pack_images_progress = packs.images_progress;
                vm.settings_pack_images_update_version = packs.images_update_version;
                vm.settings_pack_raw_available = packs.raw_available;
                vm.settings_pack_raw_installed = packs.raw_installed;
                vm.settings_pack_raw_enabled = packs.raw_enabled;
                vm.settings_pack_raw_version = packs.raw_version;
                vm.settings_pack_raw_notice = packs.raw_notice;
                vm.settings_pack_raw_installing = packs.raw_installing;
                vm.settings_pack_raw_progress = packs.raw_progress;
                vm.settings_pack_raw_update_version = packs.raw_update_version;
                vm.settings_pack_archive_available = packs.archive_available;
                vm.settings_pack_archive_installed = packs.archive_installed;
                vm.settings_pack_archive_enabled = packs.archive_enabled;
                vm.settings_pack_archive_version = packs.archive_version;
                vm.settings_pack_archive_notice = packs.archive_notice;
                vm.settings_pack_archive_installing = packs.archive_installing;
                vm.settings_pack_archive_progress = packs.archive_progress;
                vm.settings_pack_archive_update_version = packs.archive_update_version;
                // Screenshot fixture for the downloading state (percent).
                wchar_t fake_progress[8]{};
                if (s.shot.active && s.isolatedTest &&
                    GetEnvironmentVariableW(L"PULSE_TEST_PACK_PROGRESS", fake_progress, ARRAYSIZE(fake_progress))) {
                    vm.settings_pack_installing = vm.settings_pack_images_installing =
                        vm.settings_pack_raw_installing = vm.settings_pack_archive_installing = true;
                    vm.settings_pack_progress = vm.settings_pack_images_progress =
                        vm.settings_pack_raw_progress = vm.settings_pack_archive_progress =
                        static_cast<float>(_wtoi(fake_progress)) / 100.0f;
                }
            }
            vm.settings_theme = s.themeOverride == ui::ThemeMode::Light ? 1 : s.themeOverride == ui::ThemeMode::Dark ? 2 : 0;
            const auto content_config = s.contentSearch.GetConfig();
            const auto content_status = s.contentSearch.GetStatus();
            vm.settings_content_paused = content_status.paused;
            vm.settings_content_instant = s.contentSearch.InstantMode();
            wchar_t content_summary[160]{};
            swprintf_s(content_summary,l10n::Get(l10n::StringId::SettingsContentSummaryFormat).c_str(),
                static_cast<unsigned long long>(content_config.roots.size()),static_cast<unsigned long long>(content_status.indexed_files));
            vm.settings_content_summary=content_summary;
            if (vm.settings_content_instant) vm.settings_content_summary = l10n::Get(l10n::StringId::ContentInstantReady);
            // Unscoped searches cover the user folders unless all drives was chosen (content_scope_mode.h).
            if (vm.settings_content_instant && content_config.shared_scope) {
                const bool all_drives = s.appPrefs.content_scope == 2;
                vm.settings_content_summary += l10n::IsChinese()
                    ? l10n::HantText(all_drives ? L"；范围：所有驱动器" : L"；范围：个人文件夹")
                    : std::wstring(all_drives ? L"; scope: all drives" : L"; scope: personal folders");
            }
            for (const auto& root : content_config.roots) {
                ui::WindowViewModel::ContentFolderView folder;
                folder.path = root.path;
                folder.error=content_status.error!=0;
                folder.status = l10n::Get(folder.error ? l10n::StringId::SettingsUnavailable :
                    content_status.paused ? l10n::StringId::ContentIndexPaused : l10n::StringId::ContentIndexBuilding);
                for (const auto& status : content_status.root_status) if (status.path == root.path) {
                    folder.error = status.error != 0 || !status.available;
                    folder.status = l10n::Get(folder.error ? l10n::StringId::SettingsUnavailable :
                        content_status.paused ? l10n::StringId::ContentIndexPaused :
                        status.indexing ? l10n::StringId::ContentIndexBuilding : l10n::StringId::ContentIndexReady);
                    if (status.error) folder.status += L" (" + std::to_wstring(status.error) + L")";
                    break;
                }
                if (vm.settings_content_instant) { folder.error = false; folder.status = l10n::Get(l10n::StringId::ContentInstantReady); }
                vm.settings_content_folders.push_back(std::move(folder));
            }
            vm.settings_open_folders = s.appPrefs.open_folders_in_pulse;
            vm.settings_win_e = s.appPrefs.take_over_win_e;
            vm.settings_this_pc = s.appPrefs.take_over_this_pc;
            vm.settings_recycle_bin = s.appPrefs.take_over_recycle_bin;
            vm.settings_explorer_windows = s.appPrefs.take_over_explorer_windows;
            vm.settings_default_manager =
                static_cast<int>(app::DefaultFileManagerState(s.appPrefs));
            vm.settings_default_manager_desc = app::DefaultFileManagerSummary(s.appPrefs);
            vm.settings_integration_enabled = s.appPrefs.integration_enabled;
            vm.settings_integration_folders = s.appPrefs.integration_folders;
            vm.settings_integration_win_e = s.appPrefs.integration_win_e;
            vm.settings_integration_this_pc = s.appPrefs.integration_this_pc;
            vm.settings_integration_recycle_bin = s.appPrefs.integration_recycle_bin;
            vm.settings_integration_experimental = s.appPrefs.take_over_explorer_windows;
            vm.settings_integration_folders_compat = s.appPrefs.integration_folders_compat;
            vm.settings_integration_state = s.settings.IntegrationState();
            vm.settings_integration_summary = s.settings.IntegrationSummary();
            vm.settings_integration_can_retry = s.settings.IntegrationCanRetry() ||
                vm.settings_integration_state == 2;
            vm.settings_integration_can_restore = s.settings.IntegrationCanRestore();
            vm.settings_integration_folders_active = s.appPrefs.open_folders_in_pulse;
            vm.settings_integration_this_pc_active = s.appPrefs.take_over_this_pc;
            vm.settings_integration_win_e_active = s.appPrefs.take_over_win_e;
            vm.settings_integration_just_enabled = s.settings.IntegrationJustEnabled();
            vm.settings_integration_other_exe = s.appPrefs.integration_other_exe;
            vm.settings_shell_tags = s.appPrefs.shell_tag_menu;
            vm.settings_alias_mark = app::AliasStore::Instance().ShowMark();
            vm.settings_ai_marks = s.settings.AssistantMarks();
            // Settings moves right of the docked 助手 like the panes do, so its
            // result card (撤销) stays beside the rows it changed.
            RECT client{};
            if (s.hwnd && GetClientRect(s.hwnd, &client))
                vm.settings_inset_left = s.renderer.AiDockWidth(static_cast<float>(client.right));
            vm.settings_blank_click_action = s.appPrefs.blank_click_action;
            vm.settings_preview_trigger = s.appPrefs.preview_trigger;
            vm.settings_hover_delay_ms = s.appPrefs.hover_preview_delay_ms;
            vm.settings_hover_card_size = s.appPrefs.hover_card_size;
            vm.settings_change_tracking = s.appPrefs.change_tracking_enabled;
            vm.settings_change_days = s.appPrefs.change_tracking_days;
            vm.settings_row_height = s.appPrefs.row_height;
            vm.settings_tray_icon = s.appPrefs.tray_icon_size;
            vm.settings_language = s.appPrefs.language == L"zh-CN" ? 1
                : s.appPrefs.language == L"zh-TW" ? 2
                : s.appPrefs.language == L"en-US" ? 3 : 0;
            wchar_t version_text[128]{};
            swprintf_s(version_text,
                l10n::Get(l10n::StringId::VersionFormat).c_str(), PULSE_VERSION_STRING);
            vm.settings_version = version_text;
            wchar_t build_text[256]{};
            swprintf_s(build_text,
                l10n::Get(l10n::StringId::BuildIdFormat).c_str(), PULSE_BUILD_ID);
            vm.settings_build_id = build_text;
            vm.settings_update_enabled = app::UpdateChecker::Enabled() ||
                s.shot.update_available;
            vm.settings_update_auto = s.appPrefs.auto_check_updates;
            vm.settings_update_checking = s.update_checker.checking();
            vm.settings_update_downloading = s.update_installer.Progress().active() &&
                !s.update_installer.installing();
            vm.settings_update_installing = s.update_installer.installing();
            DWORD update_install_error = s.update_install_error;
            if (s.shot.active) {
                const auto progress = UpdateProgressForView(s);
                vm.settings_update_downloading |= progress.active() && progress.phase != app::UpdatePhase::Installing;
                vm.settings_update_installing |= s.shot.update_state == L"installing";
                if (s.shot.update_state == L"cancelled") update_install_error = ERROR_CANCELLED;
                if (s.shot.update_state == L"failed") update_install_error = ERROR_CRC;
            }
            vm.settings_update_available = s.update_result_ready &&
                s.update_result.update_available;
            vm.settings_update_version = s.update_result.version;
            vm.settings_diagnostics_exporting = s.settings.diagnostics_pending();
            vm.settings_show_performance = s.appPrefs.show_status_performance;
            if (vm.settings_update_installing) {
                vm.settings_update_status = l10n::Get(l10n::StringId::InstallingUpdate);
            } else if (vm.settings_update_downloading) {
                vm.settings_update_status = app::UpdateProgressText(UpdateProgressForView(s));
                if (vm.settings_update_status.empty())
                    vm.settings_update_status = l10n::Get(l10n::StringId::DownloadingUpdate);
            } else if (update_install_error != ERROR_SUCCESS) {
                const auto message = update_install_error == ERROR_CANCELLED ? l10n::StringId::UpdateCancelled :
                    update_install_error == ERROR_BUSY ? l10n::StringId::UpdateBusy : l10n::StringId::UpdateInstallFailed;
                vm.settings_update_status = l10n::Get(message);
            } else if (vm.settings_update_checking) {
                vm.settings_update_status =
                    l10n::Get(l10n::StringId::CheckingUpdates);
            } else if (!vm.settings_update_enabled) {
                vm.settings_update_status = l10n::Get(l10n::StringId::UpdateDisabled);
            } else if (!s.update_result_ready) {
                vm.settings_update_status = l10n::Get(s.appPrefs.auto_check_updates ?
                    l10n::StringId::UpdateDesc : l10n::StringId::UpdateDescManual);
            } else if (s.update_result.error == app::UpdateError::UnsupportedWindows) {
                vm.settings_update_status =
                    l10n::Get(l10n::StringId::UpdateUnsupportedWindows);
            } else if (s.update_result.error != app::UpdateError::None) {
                vm.settings_update_status = l10n::Get(l10n::StringId::UpdateFailed);
            } else if (s.update_result.update_available) {
                wchar_t available[160]{};
                swprintf_s(available,
                    l10n::Get(l10n::StringId::UpdateAvailableFormat).c_str(),
                    s.update_result.version.c_str());
                vm.settings_update_status = available;
            } else {
                vm.settings_update_status = l10n::Get(l10n::StringId::UpdateUpToDate);
            }
            vm.settings_bloom = &s.bloom_accent;
            vm.settings_index_service = s.index.Connected() && s.index.ServiceMode();
            vm.settings_index_installed = s.settings.service_installed();
            // Index and network-index texts are Simplified (most come from
            // Pulse.Index.exe); localize them here, at the display boundary.
            vm.settings_index_status = l10n::ServiceText(s.index.Status());
            vm.settings_index_migrating = s.settings.migration_pending();
            if (vm.settings_page == 3) {
                const SettingsTransferView transfer = GetSettingsTransferView(s);
                vm.settings_transfer_busy = transfer.busy;
                vm.settings_transfer_pending = transfer.pending;
                vm.settings_transfer_backup = transfer.has_backup;
                vm.settings_transfer_backup_time = transfer.backup_time;
                vm.settings_about_rows = app::BuildAboutRows(vm.settings_index_service,
                                                             vm.settings_index_installed, s.scale);
                vm.settings_release_notes = &app::EmbeddedReleaseNotes();
                vm.settings_release_expanded = s.settingsReleaseExpanded;
            }
            if (s.shot.active) {
                wchar_t simulated[2]{};
                if (GetEnvironmentVariableW(L"PULSE_TEST_INDEX_MIGRATING", simulated, 2) == 1 &&
                    simulated[0] == L'1') vm.settings_index_migrating = true;
            }
            if (vm.settings_index_migrating)
                vm.settings_index_status = l10n::Get(l10n::StringId::IndexMigrating);
            vm.settings_index_path = s.index.IndexPath();
            vm.settings_index_error = l10n::ServiceText(s.settings.error());
            vm.settings_index_volumes.clear();
            vm.settings_index_excluded_paths = s.index.ExcludedPaths();
            {
                bool exclude_system = false;
                std::vector<std::wstring> groups;
                vm.settings_index_system_known = s.index.SystemExclusion(exclude_system, groups);
                vm.settings_index_exclude_system = exclude_system;
                vm.settings_index_system_groups = 0;
                for (const auto& group : groups)
                    for (size_t i = 0; i < std::size(index::kSystemExclusionGroups); ++i)
                        if (group == index::kSystemExclusionGroups[i])
                            vm.settings_index_system_groups |= 1u << i;
                vm.settings_index_system_expanded = s.settings.system_exclusion_expanded();
                wchar_t shot_expanded[4]{};
                const bool shot_force_expanded = s.shot.active &&
                    GetEnvironmentVariableW(L"PULSE_SHOT_SYSTEM_EXPANDED", shot_expanded, 4) &&
                    shot_expanded[0] == L'1';
                if (s.shot.active && vm.settings_page == 1 &&
                    (!vm.settings_index_system_known || shot_force_expanded)) {
                    // Shots show the defaults; PULSE_SHOT_SYSTEM_EXPANDED=1 opens the list
                    // even when a running service reports its own configuration.
                    vm.settings_index_system_known = true;
                    vm.settings_index_exclude_system = true;
                    vm.settings_index_system_groups = 0x17u;  // windows, temp, old, node_modules
                    vm.settings_index_system_expanded = shot_force_expanded;
                }
            }
            vm.settings_network_roots.clear();
            auto index_volumes = s.index.Volumes();
            if (s.shot.active && vm.settings_page == 1 && index_volumes.empty()) {
                index::IndexConfig defaults;
                index_volumes = index::EnumerateLocalVolumes(defaults);
                if (!vm.settings_index_installed) vm.settings_index_service = true;
                vm.settings_index_path = L"C:\\ProgramData\\Pulse\\Index";
            }
            for (const auto& volume : index_volumes) {
                ui::IndexVolumeRowView row;
                row.id = volume.id;
                row.title = volume.label.empty()
                    ? l10n::Get(l10n::StringId::LocalDisk) : volume.label;
                if (!volume.mount_point.empty()) {
                    row.title += L" (" + volume.mount_point.substr(0, 2) + L")";
                }
                row.detail = volume.file_system.empty() ? L"NTFS" : volume.file_system;
                row.detail += L" · " + l10n::Get(volume.kind == index::VolumeKind::Removable
                    ? l10n::StringId::RemovableDisk : l10n::StringId::FixedDisk);
                if (volume.indexed_items) {
                    wchar_t count[64]{};
                    swprintf_s(count, l10n::Get(l10n::StringId::ItemsCountFormat).c_str(),
                               static_cast<int>(volume.indexed_items));
                    row.detail += L" · ";
                    row.detail += count;
                }
                row.raw_state = volume.state;
                row.state = l10n::ServiceText(volume.state);
                row.checked = volume.enabled;
                row.enabled = vm.settings_index_service && volume.supported;
                row.pending = s.settings.VolumePending(volume.id);
                row.progress = volume.progress;
                vm.settings_index_volumes.push_back(std::move(row));
            }
            const auto network_roots = s.networkIndex.Roots();
            vm.settings_network_roots.reserve(network_roots.size());
            for (const auto& root : network_roots) {
                ui::NetworkRootRowView row;
                row.path = root.path;
                row.state = l10n::ServiceText(root.state);
                row.detail = l10n::ServiceText(root.error);
                row.online = root.online;
                row.building = root.building;
                vm.settings_network_roots.push_back(std::move(row));
            }
            if (s.shot.active && vm.settings_page == 1 && vm.settings_network_roots.empty()) {
                ui::NetworkRootRowView row;
                row.path = L"\\\\fileserver\\projects\\设计资料";
                row.state = l10n::ServiceText(L"已同步 · 128,420 项");
                row.online = true;
                vm.settings_network_roots.push_back(std::move(row));
            }
            static constexpr ipc::CtxMenuGroup kGroups[] = {
                ipc::CtxMenuGroup::Software, ipc::CtxMenuGroup::OpenWith,
                ipc::CtxMenuGroup::Share, ipc::CtxMenuGroup::System, ipc::CtxMenuGroup::Print
            };
            for (int g = 0; g < 5; ++g)
                vm.settings_group_on[g] = s.ctxMenuPrefs.GroupEnabled(kGroups[g]);
            vm.settings_items.clear();
            vm.settings_items.reserve(s.ctxMenuPrefs.seen.size() + app::kBuiltinMenuItemCount);
            for (const auto& seen : s.ctxMenuPrefs.seen) {
                ui::SettingsRowView row;
                row.key = seen.key;
                // Pulse's own compress row is stored in Simplified; shell rows
                // already use the system language.
                row.text = seen.key == ipc::CompressCatalogKey()
                    ? std::wstring(l10n::Cn(ipc::CompressCatalogText())) : seen.text;
                row.group = static_cast<int>(ipc::GroupOf(seen.category));
                row.on = s.ctxMenuPrefs.RowEnabled(seen.key, seen.category, seen.from_com);
                // A row the slow-extension auto-disable turned off (#77) shows
                // why it is missing from the menu; toggling it recovers.
                row.slow_disabled = s.ctxMenuPrefs.ComDisabled(seen.key);
                vm.settings_items.push_back(std::move(row));
            }
            // Pulse's own commands: card 5, after the seen catalog (see
            // SettingsController::ToggleUi).
            vm.settings_builtin_first = vm.settings_items.size();
            vm.settings_builtin_hidden = s.ctxMenuPrefs.builtin_hidden;
            vm.settings_context_tab = s.settings.ContextTab();
            static const int shot_context_tab = [] {
                wchar_t v[4]{};
                const DWORD n = GetEnvironmentVariableW(L"PULSE_SHOT_CONTEXT_TAB", v, 4);
                return n == 1 && v[0] >= L'0' && v[0] <= L'2' ? v[0] - L'0' : -1;
            }();
            if (shot_context_tab >= 0) vm.settings_context_tab = shot_context_tab;
            for (int i = 0; i < static_cast<int>(app::BuiltinMenuSurface::Count); ++i)
                vm.settings_builtin_order[i] = s.ctxMenuPrefs.BuiltinOrder(static_cast<app::BuiltinMenuSurface>(i));
            if (s.settings.menu_drag_live()) {
                vm.settings_menu_drag = s.settings.menu_drag_item();
                vm.settings_menu_drag_pos = s.settings.menu_drag_pos();
                vm.settings_menu_drag_grab = s.settings.menu_drag_grab();
            }
            for (int i = 0; i < app::kBuiltinMenuItemCount; ++i) {
                const auto item = static_cast<app::BuiltinMenuItem>(i);
                ui::SettingsRowView row;
                row.key = app::BuiltinMenuKey(item);
                row.text = app::BuiltinMenuLabel(item);
                row.group = 5;
                row.on = s.ctxMenuPrefs.BuiltinVisible(item);
                vm.settings_items.push_back(std::move(row));
            }
            if (vm.status.status_text.empty())
                vm.status.status_text = l10n::Get(l10n::StringId::Settings);

            vm.dup_scope = static_cast<int>(s.duplicateScan.scope);
            vm.dup_folder = s.duplicateScan.folder_path;
            vm.dup_min_size = s.duplicateScan.minimum_file_bytes <= 1024 ? 0
                : s.duplicateScan.minimum_file_bytes <= 1024ull * 1024ull ? 1 : 2;
            vm.dup_scanning = s.duplicateScan.scanning;
            vm.dup_hint = l10n::Get(l10n::StringId::DupHint);
            if (vm.settings_page == 4) {
            RequestDuplicateVolumeCache(s);
            const auto& volumes = s.dup_volume_cache;
            vm.dup_drives.clear();
            vm.dup_drives.reserve(volumes.size());
            std::wstring selected_drive =
                app::DuplicateScanSession::NormalizeDriveRoot(s.duplicateScan.drive_root);
            if (selected_drive.empty()) {
                for (const auto& volume : volumes) {
                    if (volume.mount_point.empty()) continue;
                    selected_drive =
                        app::DuplicateScanSession::NormalizeDriveRoot(volume.mount_point);
                    break;
                }
            }
            for (const auto& volume : volumes) {
                if (volume.mount_point.empty()) continue;
                ui::DuplicateDriveView row;
                row.root = app::DuplicateScanSession::NormalizeDriveRoot(volume.mount_point);
                row.label = row.root.size() >= 2 ? row.root.substr(0, 2) : row.root;
                if (!volume.label.empty()) {
                    row.label += L" ";
                    row.label += volume.label;
                }
                row.selected = CompareStringOrdinal(row.root.c_str(), -1,
                    selected_drive.c_str(), -1, TRUE) == CSTR_EQUAL;
                vm.dup_drives.push_back(std::move(row));
            }
            const auto roots = app::DuplicateScanSession::ResolveRoots(
                s.duplicateScan.scope, s.duplicateScan.folder_path,
                s.duplicateScan.drive_root.empty() ? selected_drive : s.duplicateScan.drive_root,
                volumes);
            vm.dup_can_scan = s.duplicateScan.scope == app::DuplicateScanScope::Folder ||
                              !roots.empty();
            vm.dup_show_progress = s.duplicateScan.scanning;
            vm.dup_progress_indeterminate =
                s.duplicateScan.phase == index::ContentSearchPhase::Enumerating ||
                s.duplicateScan.total_files == 0;
            vm.dup_progress_value = s.duplicateScan.total_files
                ? static_cast<float>(s.duplicateScan.scanned_files) /
                  static_cast<float>(s.duplicateScan.total_files)
                : 0.0f;
            vm.dup_animation = static_cast<float>(
                std::fmod(static_cast<double>(GetTickCount64()), 1952.0) / 1952.0);
            std::wstring root_label = s.duplicateScan.current_root;
            if (root_label.size() >= 2 && root_label[1] == L':')
                root_label = root_label.substr(0, 2);
            if (root_label.empty() && s.duplicateScan.scope == app::DuplicateScanScope::Drive)
                root_label = s.duplicateScan.drive_root.size() >= 2
                    ? s.duplicateScan.drive_root.substr(0, 2) : s.duplicateScan.drive_root;
            wchar_t status[192]{};
            if (s.duplicateScan.phase == index::ContentSearchPhase::Hashing &&
                s.duplicateScan.total_files) {
                swprintf_s(status, l10n::Get(l10n::StringId::DupHashingFormat).c_str(),
                           format::GroupedInt(s.duplicateScan.scanned_files).c_str(),
                           format::GroupedInt(s.duplicateScan.total_files).c_str());
            } else {
                swprintf_s(status, l10n::Get(l10n::StringId::DupListingFormat).c_str(),
                           root_label.empty() ? L"—" : root_label.c_str(),
                           format::GroupedInt(s.duplicateScan.scanned_files).c_str());
            }
            vm.dup_status = status;
            wchar_t speed[128]{};
            if (s.duplicateScan.phase == index::ContentSearchPhase::Hashing) {
                wchar_t mb[32]{};
                swprintf_s(mb, L"%.1f", s.duplicateScan.megabytes_per_second);
                swprintf_s(speed, l10n::Get(l10n::StringId::DupHashSpeedFormat).c_str(),
                           mb, format::GroupedInt(static_cast<uint64_t>(
                               s.duplicateScan.files_per_second + 0.5)).c_str());
            } else {
                swprintf_s(speed, l10n::Get(l10n::StringId::DupFilesPerSecFormat).c_str(),
                           format::GroupedInt(static_cast<uint64_t>(
                               s.duplicateScan.files_per_second + 0.5)).c_str());
            }
            vm.dup_speed = speed;
            vm.dup_empty.clear();
            if (!s.duplicateScan.scanning && s.duplicateScan.completed) {
                if (s.duplicateScan.groups.empty() &&
                    s.duplicateScan.error == ERROR_SUCCESS)
                    vm.dup_empty = l10n::Get(l10n::StringId::DupNoResults);
                if (s.duplicateScan.truncated)
                    vm.dup_empty = l10n::Get(l10n::StringId::DupTruncated);
                if (s.duplicateScan.error != ERROR_SUCCESS &&
                    s.duplicateScan.error != ERROR_CANCELLED) {
                    wchar_t error[64]{};
                    swprintf_s(error, l10n::Get(l10n::StringId::ErrorCodeFormat).c_str(),
                               s.duplicateScan.error);
                    vm.dup_empty = error;
                }
            }
            if (s.dup_view_epoch != s.duplicateScan.result_epoch ||
                s.dup_view_language != s.appPrefs.language) {
                RefreshDuplicateGroupViews(s);
            }
            vm.dup_groups = s.dup_view_cache;
            const size_t extras = s.duplicateScan.ExtraCount();
            vm.dup_show_delete_all = extras > 0 && !s.duplicateScan.scanning;
            if (vm.dup_show_delete_all) {
                vm.dup_delete_all = l10n::Get(l10n::StringId::DupDeleteAllExtras);
                vm.dup_delete_all += L" · ";
                vm.dup_delete_all += format::GroupedInt(extras);
            }
            }
        }
    }
    for (const auto& sp : splitters) {
        ui::SplitterView view;
        view.hit_rect = sp.hit_rect;
        view.parent_bounds = sp.parent_bounds;
        view.vertical = (sp.orientation == app::SplitOrientation::Vertical);
        view.ratio = sp.node ? sp.node->ratio : 0.5f;
        view.snapped = s.splitterDragging && s.splitterSnap > 0.0f &&
                       s.splitterDragIndex == static_cast<int>(vm.splitters.size());
        if (splitters.size() == 1 && laid.size() == 2) {
            // Level with the pane headers for left/right, centered for top/bottom.
            const float r = 16.0f * s.scale;
            const float cx = view.vertical ? (sp.hit_rect.left + sp.hit_rect.right) * 0.5f
                                           : (sp.parent_bounds.left + sp.parent_bounds.right) * 0.5f;
            const float cy = view.vertical ? sp.parent_bounds.top + 29.0f * s.scale
                                           : (sp.hit_rect.top + sp.hit_rect.bottom) * 0.5f;
            view.swap_rect = D2D1::RectF(cx - r, cy - r, cx + r, cy + r);
        }
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
            slot.pane.staged = StagedHashes(s);
            if (app::Tab* tab = p->ActiveTab()) {
                FillChangePane(s, *tab, slot.pane, slot.rect);
                FillGitPane(s, *tab, slot.pane);
                FillSyncPane(s, *tab, slot.pane);   // SVN marks (outside Git) and OneDrive states
                if (slot.focused) {
                    vm.pane.git_marks = slot.pane.git_marks;
                    vm.pane.onedrive_status = slot.pane.onedrive_status;
                    AppendGitStatusText(*tab, vm.status.status_text);
                    AppendSvnStatusText(s, *tab, vm.status.status_text);
                }
                for (const auto& cutPath : s.cutPaths) {
                    if (fs::ParentPath(cutPath) != tab->current_path) continue;
                    const size_t slash = cutPath.find_last_of(L"\\/");
                    slot.pane.cut_names.insert(slash == std::wstring::npos
                        ? cutPath : cutPath.substr(slash + 1));
                }
                if (!slot.focused) FillLeavingRows(s, tab->current_path, slot.pane);
                slot.pane.ai_list = AiListOverlayFor(tab->current_path);   // Pulse 助手 at work here
                if (!slot.focused) FillLandingRows(s, p, tab->current_path, slot.pane);
            }
            if (static_cast<int>(i) == s.hoverPaneIndex) slot.pane.hover_index = s.hoverRow;
            // Hover shots stage the row on "the focused pane" (-1).
            else if (s.shot.active && s.hoverPaneIndex < 0 && slot.focused) slot.pane.hover_index = s.hoverRow;
            if (s.stripResizing && static_cast<int>(i) == s.stripResizePane)
                slot.pane.column_strip.resize_column = s.stripResizeColumn;
            if (s.stripHScrolling && static_cast<int>(i) == s.stripResizePane)
                slot.pane.column_strip.hscroll_pressed = true;
            if (static_cast<int>(i) == s.dropPaneIndex) {
                slot.pane.drop_target_index = s.dropRow;
                slot.pane.header_drop = s.dropHeader;
                slot.pane.drop_spring_progress = DropSpringProgress(s);
            }
            if (slot.focused) {
                slot.pane.rename_index = s.renameIndex;
                slot.pane.rename_text = RenameEditText(s);
                FillRenameHint(s, slot.pane);
                if (s.marqueeActive) {
                    slot.pane.marquee_active = true;
                    slot.pane.marquee_rect = MarqueeDisplayRect(s);
                }
            }
        }
        vm.pane_slots.push_back(std::move(slot));
    }
}
// ---------------------------------------------------------------------------
// Staging tray card deck: display entries (newest batch first) + eased poses.
// ---------------------------------------------------------------------------

// Deck window size: as many cards as the tray panel width fits at the
// current icon size without breaking the max-50%-overlap rule; anything
// beyond that stays behind the +N overflow and the wheel paged window.
int TrayDeckCap(const AppState& s) {
    return s.renderer.TrayDeckCapacity(static_cast<float>(s.compositor.Width()));
}


// Cyclic window into the newest-first item list: the card at `offset` is on
// top, the following ones sit underneath, wrapping around the end so the
// stack can be flipped through endlessly. A path staged more than once (copied
// again, or once to copy and once to move) shows once per window: its cards
// would share the path's animation entry and pull it toward two depths on
// every tick, so the stack never settled and its text shimmered.
std::vector<TrayDeckEntry> TrayDeckEntries(const app::StagingTray& tray, size_t offset,
                                                  size_t cap) {
    std::vector<TrayDeckEntry> all;
    const auto& batches = tray.batches();
    for (int b = static_cast<int>(batches.size()) - 1; b >= 0; --b) {
        const auto& items = batches[static_cast<size_t>(b)].items;
        for (int k = 0; k < static_cast<int>(items.size()); ++k)
            all.push_back({ b, k, &items[static_cast<size_t>(k)] });
    }
    std::vector<TrayDeckEntry> out;
    if (all.empty()) return out;
    const size_t n = all.size();
    const size_t start = offset % n;
    for (size_t i = 0; i < n && out.size() < cap; ++i) {
        const TrayDeckEntry& entry = all[(start + i) % n];
        const bool shown = std::any_of(out.begin(), out.end(),
            [&entry](const TrayDeckEntry& e) { return e.item->path == entry.item->path; });
        if (!shown) out.push_back(entry);
    }
    return out;
}

int TrayItemTotalCount(const app::StagingTray& tray) {
    int n = 0;
    for (const auto& b : tray.batches()) n += static_cast<int>(b.items.size());
    return n;
}

// Extended-length prefixes leak into tooltips otherwise: \\?\C:\x -> C:\x.
std::wstring TrayDisplayPath(const std::wstring& path) {
    return pulse::path::FriendlyPathText(path);
}

std::wstring TrayItemName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Display index of the tray card under the cursor. Hovering a card's × badge
// (TrayItemRemove) still counts as hovering that card, so the raise pose and
// the badge stay put instead of oscillating.
int TrayDeckHoverIndex(const AppState& s) {
    if (s.hoverRegion == static_cast<int>(ui::HitTestResult::TrayCard))
        return s.hoverControlIndex;
    if (s.hoverRegion == static_cast<int>(ui::HitTestResult::TrayIntent))
        return s.hoverSubIndex;
    if (s.hoverRegion == static_cast<int>(ui::HitTestResult::TrayItemRemove)) {
        const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(TrayStackTop(s)),
                                             static_cast<size_t>(TrayDeckCap(s)));
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
// Starred projects are path records in places.json, not copies or .lnk shortcuts.
void RefreshStarredViews(AppState& s) {
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab) return;
        std::wstring kind;
        if (app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"starred")
            LoadVirtualView(s, *tab, tab->current_path);
    });
}

void RefreshRecentViews(AppState& s) {
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!tab) return;
        std::wstring kind;
        if (app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"recent")
            LoadVirtualView(s, *tab, tab->current_path);
    });
}

bool IsRecycleTab(const app::Tab* tab) {
    std::wstring kind;
    return tab && app::ParsePulsePath(tab->current_path, &kind, nullptr) && kind == L"recycle";
}

std::wstring RecycleOccupancyText(const fs::RecycleBinInfo& info) {
    if (!info.valid) return {};
    wchar_t buf[96]{};
    swprintf_s(buf, l10n::Get(l10n::StringId::RecycleOccupancyFormat).c_str(),
               std::to_wstring(info.items).c_str(),
               pulse::format::ByteSize(info.bytes).c_str());
    return buf;
}

void ApplyRecycleOccupancy(AppState& s) {
    const std::wstring detail = RecycleOccupancyText(s.recycle_info);
    for (auto& entry : s.sidebar.quick_access) {
        if (entry.path == app::MakeRecyclePath()) {
            entry.detail = detail;
            break;
        }
    }
    ForEachPane(s, [&](app::Pane& pane) {
        app::Tab* tab = pane.ActiveTab();
        if (!IsRecycleTab(tab)) return;
        tab->banner_title = l10n::Get(l10n::StringId::RecycleBin);
        tab->banner_message = detail;
    });
}

void RequestRecycleOccupancy(AppState& s) {
    const HWND hwnd = ServiceWindow(s);
    s.worker.EnqueueIo([hwnd] {
        auto* info = new fs::RecycleBinInfo{};
        fs::QueryRecycleBinInfo(*info);
        if (hwnd && PostMessageW(hwnd, WM_RECYCLE_INFO, 0, reinterpret_cast<LPARAM>(info)))
            return;
        delete info;
    });
}

bool ApplyQueriedRecycleInfo(AppState& s, const fs::RecycleBinInfo& info) {
    if (!info.valid) return false;
    // Shell often reports the pre-mutation count for a second or two. Keep the
    // optimistic occupancy until a query actually moves off that baseline.
    if (s.recycle_info_guard &&
        info.items == s.recycle_ignore_items &&
        s.recycle_info.valid &&
        s.recycle_info.items != s.recycle_ignore_items)
        return false;
    s.recycle_info = info;
    if (info.items != s.recycle_ignore_items)
        s.recycle_info_guard = false;
    ApplyRecycleOccupancy(s);
    return true;
}

void RefreshRecycleViews(AppState& s, bool query_occupancy) {
    RefreshPath(s, app::MakeRecyclePath());
    if (query_occupancy) RequestRecycleOccupancy(s);
}

void ScheduleRecycleRefresh(AppState& s) {
    s.recycle_ignore_items = s.recycle_info.valid ? s.recycle_info.items : 0;
    s.recycle_info_guard = true;
    s.recycle_refresh_left = 3;
    const ULONGLONG now = GetTickCount64();
    if (s.recycle_refresh_at == 0)
        s.recycle_refresh_at = now + 300;
}

void BumpRecycleOccupancy(AppState& s, int64_t delta) {
    if (!s.recycle_info.valid) {
        s.recycle_info.valid = true;
        s.recycle_info.items = 0;
        s.recycle_info.bytes = 0;
    }
    if (delta > 0) {
        s.recycle_info.items += static_cast<uint64_t>(delta);
    } else if (delta < 0) {
        const uint64_t sub = static_cast<uint64_t>(-delta);
        s.recycle_info.items = s.recycle_info.items > sub ? s.recycle_info.items - sub : 0;
        if (s.recycle_info.items == 0) s.recycle_info.bytes = 0;
    }
    ApplyRecycleOccupancy(s);
}

void ClearRecycleOccupancy(AppState& s) {
    s.recycle_info.valid = true;
    s.recycle_info.items = 0;
    s.recycle_info.bytes = 0;
    ApplyRecycleOccupancy(s);
}

void ApplyEmptyRecycleFinished(AppState& s, bool emptied) {
    // The bin is emptied in place, so the Shell count is already final: drop
    // the stale-count guard and query now. A cancelled or partial empty keeps
    // its old figure until that query reports what is left.
    s.recycle_info_guard = false;
    if (emptied) ClearRecycleOccupancy(s);
    RefreshRecycleViews(s, true);
}

bool PumpRecycleRefresh(AppState& s, ULONGLONG now) {
    if (s.recycle_refresh_left <= 0 || s.recycle_refresh_at == 0 || now < s.recycle_refresh_at)
        return false;
    RefreshRecycleViews(s, true);
    --s.recycle_refresh_left;
    if (s.recycle_refresh_left > 0)
        s.recycle_refresh_at = now + (s.recycle_refresh_left == 2 ? 900ull : 1600ull);
    else
        s.recycle_refresh_at = 0;
    return true;
}

bool ToggleStarred(AppState& s, const std::wstring& target,
                          app::PlaceItemKind kind) {
    if (target.empty() || fs::IsVirtualPath(target)) return false;
    const bool on = s.places.ToggleStarred(target, kind);
    s.detailsStarred = on;
    POINT cursor{};
    RECT client{};
    if (on && s.hwnd && GetCursorPos(&cursor) && ScreenToClient(s.hwnd, &cursor) &&
        GetClientRect(s.hwnd, &client) && PtInRect(&client, cursor))
        s.renderer.LaunchStarFlight(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
    RefreshStarredViews(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    return on;
}

void StopDetailsSizeWalk(AppState& s) {
    s.detailsSizeGeneration.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        s.detailsSizeRequested.clear();
        s.detailsSize = AppState::DetailsSizeResult{};
    }
    s.detailsSizeCv.notify_one();
}

void StartDetailsSizeWalk(AppState& s, const std::wstring& path) {
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

void ShutdownDetailsSizeWalk(AppState& s) {
    s.detailsSizeGeneration.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(s.detailsSizeMutex);
        s.detailsSizeStop = true;
        s.detailsSizeRequested.clear();
    }
    s.detailsSizeCv.notify_one();
    if (s.detailsSizeThread.joinable()) s.detailsSizeThread.join();
}

std::wstring DetailsAttributeText(DWORD attrs) {
    struct AttributeName { DWORD bit; const wchar_t* name; };
    const AttributeName values[] = {
        { FILE_ATTRIBUTE_READONLY, pulse::l10n::Get(pulse::l10n::StringId::AttrReadOnly).c_str() },
        { FILE_ATTRIBUTE_HIDDEN, pulse::l10n::Get(pulse::l10n::StringId::AttrHidden).c_str() },
        { FILE_ATTRIBUTE_SYSTEM, pulse::l10n::Get(pulse::l10n::StringId::AttrSystem).c_str() },
        { FILE_ATTRIBUTE_COMPRESSED, pulse::l10n::Get(pulse::l10n::StringId::AttrCompressed).c_str() },
        { FILE_ATTRIBUTE_ENCRYPTED, pulse::l10n::Get(pulse::l10n::StringId::AttrEncrypted).c_str() },
        { FILE_ATTRIBUTE_REPARSE_POINT, pulse::l10n::Get(pulse::l10n::StringId::AttrReparse).c_str() },
    };
    std::wstring result;
    for (const auto& value : values) {
        if (!(attrs & value.bit)) continue;
        if (!result.empty()) result += pulse::l10n::IsChinese() ? L"、" : L", ";
        result += value.name;
    }
    return result.empty() ? pulse::l10n::Get(pulse::l10n::StringId::AttrNormal).c_str() : result;
}

namespace {
// CSS-style cubic-bezier easing (x1, y1, x2, y2) evaluated at time t.
float CubicBezier(float x1, float y1, float x2, float y2, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto bx = [&](float u) {
        const float v = 1.0f - u;
        return 3.0f * v * v * u * x1 + 3.0f * v * u * u * x2 + u * u * u;
    };
    auto by = [&](float u) {
        const float v = 1.0f - u;
        return 3.0f * v * v * u * y1 + 3.0f * v * u * u * y2 + u * u * u;
    };
    float lo = 0.0f, hi = 1.0f, u = t;
    for (int i = 0; i < 24; ++i) {
        u = (lo + hi) * 0.5f;
        if (bx(u) < t) lo = u; else hi = u;
    }
    return by(u);
}

constexpr float kTrayThrowOutMs = 220.0f;
constexpr float kTrayThrowBackMs = 460.0f;
constexpr float kTraySpringMs = 420.0f;
constexpr float kTrayTumbleMs = 560.0f;
constexpr float kTrayPuffMs = 620.0f;
constexpr float kTrayFadeMs = 140.0f;

void StartTrayMotion(AppState::TrayCardAnim& a, AppState::TrayMotion motion, ULONGLONG now) {
    a.motion = motion;
    a.motion_started = now;
    a.from_fly = a.fly;
    a.from_dx = a.dx;
    a.from_dy = a.dy;
    a.from_angle = a.angle;
}

// Advance throw / spring motion. Returns true while still moving.
bool AdvanceTrayMotion(AppState::TrayCardAnim& a, ULONGLONG now) {
    using M = AppState::TrayMotion;
    const float ms = static_cast<float>(now - a.motion_started);
    switch (a.motion) {
    case M::None:
        return false;
    case M::ThrowOut: {
        const float t = ms / kTrayThrowOutMs;
        const float e = CubicBezier(0.3f, 0.6f, 0.4f, 1.0f, t);
        a.fly = a.from_fly + (a.to_fly - a.from_fly) * e;
        a.dx = a.from_dx * (1.0f - e);
        a.dy = a.from_dy + (a.to_dy - a.from_dy) * e;
        a.angle = a.from_angle + (a.to_angle - a.from_angle) * e;
        if (t >= 1.0f) StartTrayMotion(a, M::ThrowBack, now);
        return true;
    }
    case M::ThrowBack:
    case M::Spring: {
        const bool back = a.motion == M::ThrowBack;
        const float t = ms / (back ? kTrayThrowBackMs : kTraySpringMs);
        const float e = back ? CubicBezier(0.2f, 0.9f, 0.25f, 1.08f, t)
                             : CubicBezier(0.2f, 1.4f, 0.4f, 1.0f, t);
        a.fly = a.from_fly * (1.0f - e);
        a.dx = a.from_dx * (1.0f - e);
        a.dy = a.from_dy * (1.0f - e);
        a.angle = a.from_angle * (1.0f - e);
        if (t >= 1.0f) {
            a.motion = M::None;
            a.fly = a.dx = a.dy = a.angle = 0.0f;
            return false;
        }
        return true;
    }
    }
    return false;
}

std::wstring TrayFolderOf(const std::wstring& path) {
    const std::wstring shown = TrayDisplayPath(path);
    const size_t slash = shown.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    if (slash == 2 && shown.size() > 1 && shown[1] == L':') return shown.substr(0, 3);
    return shown.substr(0, slash);
}
} // namespace

int TrayStackTop(const AppState& s) {
    const int total = TrayItemTotalCount(s.tray);
    return total > 0 ? ((s.trayDeckOffset % total) + total) % total : 0;
}

// Send the top card to the back of the stack: it flies out toward `dir`
// (+1 right, -1 left) from its current gesture pose, then settles under the
// stack. dx/dy are the release offset in DIPs (0 for wheel / button).
void ThrowTrayTop(AppState& s, float dir, float dx, float dy) {
    const int total = TrayItemTotalCount(s.tray);
    if (total < 2) return;
    const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(TrayStackTop(s)), 1);
    if (entries.empty()) return;
    const ULONGLONG now = ui::motion::NowMs();
    AppState::TrayCardAnim& a = s.trayCards[entries.front().item->path];
    a.dx = dx;
    a.dy = dy;
    a.angle = dx * 0.07f;
    StartTrayMotion(a, AppState::TrayMotion::ThrowOut, now);
    a.to_fly = (dir < 0.0f ? -1.0f : 1.0f) * 1.18f;
    a.to_dy = dy;
    a.to_angle = (dir < 0.0f ? -1.0f : 1.0f) * 16.0f;
    s.trayDeckOffset = (TrayStackTop(s) + 1) % total;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// End a press-and-hold on the top card. A far or fast enough fling sends
// the card to the back; anything else springs it home. commit=false (lost
// capture) always springs back.
void ReleaseTrayDrag(AppState& s, bool commit) {
    AppState::TrayDrag drag = s.trayDrag;
    s.trayDrag = AppState::TrayDrag{};
    if (!drag.active) return;
    float vx = drag.vx;
    if (ui::motion::NowMs() - drag.last_t > 80) vx = 0.0f; // stopped before letting go
    const bool fling = std::abs(drag.dx) > 70.0f || std::abs(vx) > 0.55f;
    if (commit && fling && TrayItemTotalCount(s.tray) > 1) {
        const float dir = std::abs(drag.dx) > 8.0f ? (drag.dx < 0.0f ? -1.0f : 1.0f)
                                                    : (vx < 0.0f ? -1.0f : 1.0f);
        ThrowTrayTop(s, dir, drag.dx, drag.dy * 0.35f);
        return;
    }
    const auto found = s.trayCards.find(drag.path);
    if (found != s.trayCards.end()) {
        AppState::TrayCardAnim& a = found->second;
        a.dx = drag.dx;
        a.dy = drag.dy * 0.35f;
        a.angle = drag.dx * 0.07f;
        StartTrayMotion(a, AppState::TrayMotion::Spring, ui::motion::NowMs());
    }
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Bring the last card back on top: it drops in from above.
void TrayStepBack(AppState& s) {
    const int total = TrayItemTotalCount(s.tray);
    if (total < 2) return;
    s.trayDeckOffset = (TrayStackTop(s) + total - 1) % total;
    const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset), 1);
    if (!entries.empty()) s.trayRaisePath = entries.front().item->path;
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Smoke burst at the top card's close badge (renderer anchors it).
void SpawnTrayPuffs(AppState& s) {
    const ULONGLONG now = ui::motion::NowMs();
    uint32_t seed = static_cast<uint32_t>(now * 2654435761u);
    auto frand = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return static_cast<float>(seed & 0xFFFFFF) / 16777216.0f;
    };
    for (int i = 0; i < 10; ++i) {
        AppState::TrayPuff p;
        p.start = now + static_cast<ULONGLONG>(frand() * 60.0f);
        p.angle = static_cast<float>(i) / 10.0f * 6.2831853f + frand() * 0.5f;
        p.dist = 16.0f + frand() * 16.0f;
        p.size = 0.8f + frand() * 0.9f;
        s.trayPuffs.push_back(p);
    }
}

// Tell the next tick how the currently visible cards of `paths` should
// leave: tumble off (dismiss / clear / release), staggered by stack order.
void MarkTrayExit(AppState& s, const std::vector<std::wstring>& paths, bool stagger) {
    const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(TrayStackTop(s)), 4);
    ULONGLONG delay = 0;
    for (const auto& e : entries) {
        if (std::find(paths.begin(), paths.end(), e.item->path) == paths.end()) continue;
        s.trayExitHints[e.item->path] = { AppState::TrayExit::Tumble, delay };
        if (stagger) delay += 60;
    }
}

// Release mode the shelf shows right now: the tray-wide mode, flipped while
// Shift is held (pulse_tray_redesign.html effMode).
bool TrayEffectiveMove(const AppState& s) {
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    return s.trayMoveMode != shift;
}

// Hashes of every staged path for the "已暂存" row pill, rebuilt only when
// the tray changes.
std::shared_ptr<const std::unordered_set<uint64_t>> StagedHashes(AppState& s) {
    uint64_t sig = 1469598103934665603ull;
    size_t n = 0;
    for (const auto& b : s.tray.batches())
        for (const auto& item : b.items) {
            sig = (sig ^ item.id) * 1099511628211ull;
            sig = (sig ^ std::hash<std::wstring>{}(item.path)) * 1099511628211ull;
            ++n;
        }
    if (n == 0) {
        s.trayShelf.staged.reset();
        s.trayShelf.staged_sig = 0;
        return nullptr;
    }
    if (s.trayShelf.staged && s.trayShelf.staged_sig == sig) return s.trayShelf.staged;
    auto set = std::make_shared<std::unordered_set<uint64_t>>();
    set->reserve(n);
    for (const auto& b : s.tray.batches())
        for (const auto& item : b.items) set->insert(ui::StagedPathHash(item.path));
    s.trayShelf.staged = std::move(set);
    s.trayShelf.staged_sig = sig;
    return s.trayShelf.staged;
}

// Del in the open view: rows slide out for 320 ms, then leave the tray.
static void TickTrayOpenRemoval(AppState& s, ULONGLONG now) {
    auto& v = s.trayView;
    if (v.removing.empty() || now - v.remove_at < 320) return;
    const std::vector<uint64_t> ids = std::move(v.removing);
    v.removing.clear();
    RemoveTrayItems(s, ids);
}

// Shelf layers and the sprung panel height (Open 480/38 growing, Close
// 700/52 shrinking), integrated in fixed 4 ms steps.
static bool TickTrayShelf(AppState& s, float elapsed) {
    auto& a = s.trayShelf;
    bool dirty = false;
    const ULONGLONG now = ui::motion::NowMs();
    const int total = TrayItemTotalCount(s.tray);
    int stage = static_cast<int>(ui::TrayStage::Strip);
    if (total == 0 && s.trayView.open) s.trayView = {};
    if (s.dragFilesOver && !s.trayDragOut) stage = static_cast<int>(ui::TrayStage::Drop);
    else if (total > 0) stage = static_cast<int>(s.trayView.open ? ui::TrayStage::Open : ui::TrayStage::Shelf);
    if (stage != static_cast<int>(ui::TrayStage::Open)) s.trayView.compare = false;
    TickTrayOpenRemoval(s, now);
    const bool animate = ui::motion::SystemAnimationsEnabled();
    if (stage != a.stage) {
        a.prev_stage = a.initialized && animate ? a.stage : -1;
        if (stage == static_cast<int>(ui::TrayStage::Drop)) a.drop_from = a.height;
        a.stage = stage;
        a.swap_at = now;
        dirty = true;
    }
    // The Ctrl+C offer adds the 收入 row to the shelf (and pops in).
    const bool clip = TrayClipLive(s);
    if (clip && now - s.trayClip.at < 460) dirty = true;
    float target = ui::TrayShelfStageHeightDip(stage, clip);
    // Setting: an empty tray folds away; a Ctrl+C offer or a drag brings it back.
    if (stage == static_cast<int>(ui::TrayStage::Strip) && !clip && s.appPrefs.tray_hide_when_empty) target = 0.0f;
    if (stage == static_cast<int>(ui::TrayStage::Drop)) target = std::max(target, a.drop_from);
    if (!a.initialized || !animate) {
        if (a.height != target) dirty = true;
        a.height = target;
        a.velocity = 0.0f;
        a.initialized = true;
    } else if (std::abs(target - a.height) > 0.05f || std::abs(a.velocity) > 0.5f) {
        const ui::motion::SpringSpec spec = target > a.height - 0.5f && a.velocity >= -1.0f
            ? ui::motion::spring::Open : ui::motion::spring::Close;
        float left = std::min(elapsed, 64.0f);
        while (left > 0.0f) {
            const float dt = std::min(left, 4.0f) * 0.001f;
            const float acc = spec.k * (target - a.height) - spec.c * a.velocity;
            a.velocity += acc * dt;
            a.height += a.velocity * dt;
            left -= 4.0f;
        }
        if (std::abs(target - a.height) <= 0.05f && std::abs(a.velocity) <= 0.5f) {
            a.height = target;
            a.velocity = 0.0f;
        }
        dirty = true;
    }
    if (a.prev_stage >= 0 && now - a.swap_at < 420) dirty = true;
    else if (a.prev_stage >= 0) { a.prev_stage = -1; dirty = true; }

    auto ease = [&dirty, elapsed](float& cur, float to, float k) {
        const float timed_k = 1.0f - std::pow(1.0f - k, std::min(elapsed, 100.0f) / 16.0f);
        const float next = cur + (to - cur) * timed_k;
        if (std::abs(next - cur) > 0.002f) { cur = next; dirty = true; }
        else if (cur != to) { cur = to; dirty = true; }
    };
    const bool move = stage == static_cast<int>(ui::TrayStage::Drop) ? s.dropTrayMove : TrayEffectiveMove(s);
    if (move != a.last_move) {
        a.last_move = move;
        if (stage == static_cast<int>(ui::TrayStage::Shelf) && animate) a.label_at = now;
        dirty = true;
    }
    ease(a.mode_t, move ? 1.0f : 0.0f, animate ? 0.22f : 1.0f);
    const bool fan_hot = s.hoverRegion == static_cast<int>(ui::HitTestResult::TrayShelf) &&
        s.hoverControlIndex == static_cast<int>(ui::TrayShelfHit::Fan);
    ease(a.fan_spread, fan_hot ? 1.0f : 0.0f, animate ? 0.2f : 1.0f);
    if (a.last_total >= 0 && total > a.last_total && animate) a.bump_at = now;
    a.last_total = total;
    if (a.bump_at && now - a.bump_at < 520) dirty = true;
    if (a.label_at && now - a.label_at < 340) dirty = true;
    if (a.flash_at && now - a.flash_at < 920) dirty = true;
    if (s.trayZip.id) dirty = true;  // progress ring
    // Open view: row stagger, Del slide-out, compare card entrance.
    if (s.trayView.open && s.trayView.opened_at && now - s.trayView.opened_at < 1000) dirty = true;
    if (!s.trayView.removing.empty()) dirty = true;
    if (s.trayView.compare && now - s.trayView.compare_at < 470) dirty = true;
    return dirty;
}

// Per-frame stack animation on the 16 ms UI timer. Returns true while
// anything is still moving (caller invalidates).
bool TickTrayDeck(AppState& s) {
    using M = AppState::TrayMotion;
    using X = AppState::TrayExit;
    bool dirty = false;
    const ULONGLONG now = ui::motion::NowMs();
    // After a quiet spell the UI timer polls every 100-500 ms: motion that
    // starts then begins with one frame's step, not a jump of several.
    float elapsed = s.trayLastTick ? static_cast<float>(now - s.trayLastTick) : 16.0f;
    if (elapsed > 50.0f) elapsed = 16.0f;
    s.trayLastTick = now;
    auto ease = [&dirty, elapsed](float& cur, float target, float k) {
        const float timed_k = 1.0f - std::pow(1.0f - k, std::min(elapsed, 100.0f) / 16.0f);
        const float next = cur + (target - cur) * timed_k;
        if (std::abs(next - cur) > 0.0015f) { cur = next; dirty = true; }
        else if (cur != target) { cur = target; dirty = true; }
    };

    ease(s.trayOpen, s.dropTray ? 1.0f : 0.0f, 0.20f);
    if (TickTrayShelf(s, elapsed)) dirty = true;

    // A fresh collect always brings the newest item to the top.
    const int total = TrayItemTotalCount(s.tray);
    const int top = TrayStackTop(s);
    if (top != s.trayDeckOffset) { s.trayDeckOffset = top; dirty = true; }
    const bool grew = total > static_cast<int>(s.trayDeckLastTotal);
    if (grew && s.trayDeckOffset != 0) { s.trayDeckOffset = 0; dirty = true; }
    s.trayDeckLastTotal = static_cast<size_t>(total);

    const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset),
                                         static_cast<size_t>(TrayDeckCap(s)));
    const int n = static_cast<int>(entries.size());
    const int hovered = TrayDeckHoverIndex(s);
    const bool stackHot = hovered >= 0 || s.trayDrag.active;
    ease(s.traySpread, stackHot ? 1.0f : 0.0f, 0.20f);

    std::unordered_set<std::wstring> live;
    live.reserve(entries.size() * 2);
    for (int i = 0; i < n; ++i) {
        const TrayDeckEntry& entry = entries[static_cast<size_t>(i)];
        const app::TrayItem& item = *entry.item;
        live.insert(item.path);
        auto [it, inserted] = s.trayCards.try_emplace(item.path);
        AppState::TrayCardAnim& a = it->second;
        if (!inserted && a.ghost && a.exit != X::Return) {
            a = AppState::TrayCardAnim{};
            inserted = true;
        }
        if (inserted || a.ghost) {
            a.ghost = false;
            a.depth = static_cast<float>(i);
            a.appear = 1.0f;
            // A fresh collect drops in on top; cards revealed underneath
            // start hidden behind the stack (depth 3) and rise into place.
            if (i > 0) a.depth = 3.0f;
        }
        // A fresh collect drops in on top - even when the path still has a
        // settled anim left over from an earlier stay in the tray.
        if (i == 0 && grew && !(s.trayDrag.active && s.trayDrag.path == item.path)) {
            a.appear = 0.0f;
            a.depth = 0.0f;
            // Motion v2 "droptray": the new card falls in on a spring - it
            // lands with a small bounce and its tilt straightens out.
            if (ui::motion::SystemAnimationsEnabled()) {
                a.dy = -22.0f;
                a.angle = (total & 1) ? -6.0f : 6.0f;
                StartTrayMotion(a, M::Spring, now);
            }
        }
        if (item.path == s.trayRaisePath) {
            a.appear = 0.0f;
            a.depth = 0.0f;
            a.motion = M::None;
            a.fly = a.dx = a.dy = a.angle = 0.0f;
        }
        if (a.name.empty()) a.name = TrayItemName(item.path);
        if (a.folder.empty()) a.folder = TrayFolderOf(item.path);
        a.attrs = item.attrs;
        a.is_dir = item.is_dir;
        a.missing = !item.exists;
        a.size = item.size;
        a.cut = entry.batch >= 0 && entry.batch < static_cast<int>(s.tray.batches().size()) &&
                s.tray.batches()[static_cast<size_t>(entry.batch)].move_intent;
        a.shrink = 1.0f;
        if (s.trayDrag.active && s.trayDrag.path == item.path) {
            a.motion = M::None;
            a.fly = 0.0f;
            a.dx = s.trayDrag.dx;
            a.dy = s.trayDrag.dy * 0.35f;
            a.angle = s.trayDrag.dx * 0.07f;
            dirty = true;
        } else if (AdvanceTrayMotion(a, now)) {
            dirty = true;
        }
        if (a.motion != M::ThrowOut) ease(a.depth, static_cast<float>(i), 0.20f);
        ease(a.appear, 1.0f, 0.16f);
        ease(a.hover, i == 0 && hovered == 0 && !s.trayDrag.active ? 1.0f : 0.0f, 0.28f);
        ease(a.opacity, 1.0f, 0.25f);
    }
    s.trayRaisePath.clear();

    // Cards that left the window play their exit, then are dropped.
    size_t ghosts = 0;
    for (auto it = s.trayCards.begin(); it != s.trayCards.end();) {
        if (live.count(it->first)) { ++it; continue; }
        AppState::TrayCardAnim& a = it->second;
        if (!a.ghost) {
            a.ghost = true;
            a.exit_started = now;
            a.exit_opacity = a.opacity;
            a.exit_delay = 0;
            const auto hint = s.trayExitHints.find(it->first);
            if (hint != s.trayExitHints.end()) {
                a.exit = hint->second.exit;
                a.exit_delay = hint->second.delay;
            } else {
                // Still staged but outside the window: it was sent to the back.
                bool staged = false;
                for (const auto& b : s.tray.batches())
                    for (const auto& item : b.items)
                        if (item.path == it->first) staged = true;
                a.exit = staged ? X::Return : X::Fade;
            }
            if (a.exit == X::Tumble) StartTrayMotion(a, M::None, now);
        }
        bool done = false;
        const float local = static_cast<float>(now) - static_cast<float>(a.exit_started)
                          - static_cast<float>(a.exit_delay);
        switch (a.exit) {
        case X::Fade: {
            const float t = std::clamp(local / kTrayFadeMs, 0.0f, 1.0f);
            a.opacity = a.exit_opacity * (1.0f - t) * (1.0f - t);
            done = t >= 1.0f;
            break;
        }
        case X::Tumble: {
            if (local < 0.0f) break; // staggered: waiting its turn
            const float t = std::clamp(local / kTrayTumbleMs, 0.0f, 1.0f);
            const float e = CubicBezier(0.45f, -0.25f, 0.85f, 0.55f, t);
            a.fly = a.from_fly + (1.25f - a.from_fly) * e;
            a.dx = a.from_dx * (1.0f - e);
            a.dy = a.from_dy + (40.0f - a.from_dy) * e;
            a.angle = a.from_angle + (32.0f - a.from_angle) * e;
            a.shrink = 1.0f - 0.08f * e;
            a.hover = 0.0f;
            const float ot = std::clamp((local - 60.0f) / 500.0f, 0.0f, 1.0f);
            a.opacity = a.exit_opacity * (1.0f - CubicBezier(0.42f, 0.0f, 1.0f, 1.0f, ot));
            done = t >= 1.0f;
            break;
        }
        case X::Return: {
            const bool moving = AdvanceTrayMotion(a, now);
            if (a.motion != M::ThrowOut) ease(a.depth, 3.0f, 0.20f);
            a.hover = 0.0f;
            done = !moving && a.depth >= 2.99f;
            if (local > 1200.0f) done = true;
            break;
        }
        }
        dirty = true;
        ++ghosts;
        if (done || ghosts > 8) it = s.trayCards.erase(it);
        else ++it;
    }
    s.trayExitHints.clear();

    const size_t puffs = s.trayPuffs.size();
    s.trayPuffs.erase(std::remove_if(s.trayPuffs.begin(), s.trayPuffs.end(),
        [now](const AppState::TrayPuff& p) {
            return now > p.start && static_cast<float>(now - p.start) >= kTrayPuffMs;
        }), s.trayPuffs.end());
    if (!s.trayPuffs.empty() || puffs != s.trayPuffs.size()) dirty = true;
    return dirty;
}

static bool HoverHintIsCommand(ui::HitTestResult::Region region) {
    using R = ui::HitTestResult;
    switch (region) {
    case R::None:
    case R::Row:
    case R::Pane:
    case R::Tab:
    case R::SidebarItem:
    case R::SidebarHeader:
    case R::TrayCard:
    case R::AddressBar:
    case R::ColumnHeader:
    case R::StatusBar:
    case R::StatusBarTask:
    case R::StatusHintAction:
        return false;
    default:
        return true;
    }
}

// The status-bar hint answers "what can I do right now?". Priority follows the
// approved mockup: drag > compare > multi-select > read-only > selection >
// search > two panes > idle. `action` receives an optional clickable follow-up.
static std::wstring ContextStatusHint(AppState& s, StatusHintAction& action) {
    using I = l10n::StringId;
    action = StatusHintAction::None;
    const app::Tab* tab = ActiveTab(s);
    if (!tab) return {};
    if (IsSettingsTab(tab)) return l10n::Get(I::Settings);
    if (!s.appPrefs.show_hints) return {};
    if (!s.dropBadge.empty())
        return l10n::Get(s.trayDragOut ? I::HintTrayDragOut : I::HintDrag);
    if (s.addressSearching) {
        // Typing a search: surface the keys the search box understands.
        const auto sep = L"  \u00B7  ";
        std::wstring hint = l10n::Get(I::HintKeyEnterSearch);
        if (s.addressSearchCurrent) hint += sep + l10n::Get(I::HintKeyShiftEnter);
        if (!s.addressSearchContent) hint += sep + l10n::Get(I::HintKeyAltEnter);
        hint += sep + l10n::Get(I::HintKeyTabScope);
        hint += sep + l10n::Get(IsAddressSearchResults(tab) ? I::HintKeyDownResults : I::HintKeyDownHistory);
        action = StatusHintAction::Advanced;
        return hint;
    }
    {
        using R = ui::HitTestResult;
        const int hr = s.hoverRegion;
        if (hr == static_cast<int>(R::TrayCard) || hr == static_cast<int>(R::TrayIntent) ||
            hr == static_cast<int>(R::TrayItemRemove) || hr == static_cast<int>(R::TrayPrev) ||
            hr == static_cast<int>(R::TrayNext))
            return l10n::Get(I::HintTrayMenu);
        if (hr == static_cast<int>(R::TrayDest)) return l10n::Get(I::HintTrayDest);
    }
    if (IsRecycleTab(tab)) return l10n::Get(I::StatusHintRecycle);
    const app::LayoutTab* layout = s.window_tabs.Active();
    if (layout && layout->compare) {
        action = layout->compare_diff_only ? StatusHintAction::ShowAll : StatusHintAction::DiffOnly;
        return l10n::Get(I::HintCompare);
    }
    std::wstring kind;
    app::ParsePulsePath(tab->current_path, &kind, nullptr);
    const int selected = tab->SelectedCount();
    if (s.filterEditing && !s.filterSelectMode) {
        // While typing a filter the selection may be hidden; explain the filter instead.
        if (s.appPrefs.search_enabled && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
            action = StatusHintAction::SearchSubfolders;
        return l10n::Get(I::HintFiltering);
    }
    if (selected >= 2) {
        action = StatusHintAction::Tray;
        return l10n::Get(I::HintMultiSel);
    }
    if (tab->net_readonly) return l10n::Get(I::HintReadonly2);
    if (selected == 1) {
        const int index = tab->selected_index;
        const bool folder = tab->snapshot && index >= 0 &&
            static_cast<size_t>(index) < tab->snapshot->size() && (*tab->snapshot)[index].is_dir;
        if (kind == L"search") {
            action = StatusHintAction::OpenPath;
            return l10n::Get(I::HintSearchResultSel);
        }
        return l10n::Get(folder ? I::HintFolderSel : I::StatusHintSelected);
    }
    if (!tab->filter_text.empty() && !s.filterSelectMode) {
        // Pane filter vs. search: say that subfolders are not included.
        if (s.appPrefs.search_enabled && !tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
            action = StatusHintAction::SearchSubfolders;
        return l10n::Get(I::HintFiltering);
    }
    if (tab->current_path == kNetworkLocationsPath) {
        // 网络位置: what discovery found and when, with 重新发现 (design §06).
        std::wstring lan = NetworkLanHint(s);
        if (!lan.empty()) {
            action = StatusHintAction::LanRescan;
            return lan;
        }
    }
    if (kind == L"search") {
        action = StatusHintAction::Advanced;
        return l10n::Get(I::HintSearchSmart);
    }
    if (layout && layout->panes.size() >= 2) {
        action = StatusHintAction::Compare;
        return l10n::Get(I::HintSplit);
    }
    action = StatusHintAction::Shortcuts;
    return l10n::Get(s.appPrefs.search_enabled ? I::StatusHintIdle : I::StatusHintIdleNoSearch);
}

static const std::wstring& StatusHintActionText(StatusHintAction action) {
    using I = l10n::StringId;
    switch (action) {
    case StatusHintAction::Tray: return l10n::Get(I::HintActTray);
    case StatusHintAction::Compare: return l10n::Get(I::HintActCompare);
    case StatusHintAction::DiffOnly: return l10n::Get(I::HintActDiffOnly);
    case StatusHintAction::ShowAll: return l10n::Get(I::HintActShowAll);
    case StatusHintAction::Advanced: return l10n::Get(I::HintActAdvanced);
    case StatusHintAction::Shortcuts: return l10n::Get(I::HintActShortcuts);
    case StatusHintAction::OpenPath: return l10n::Get(I::HintActOpenPath);
    case StatusHintAction::SearchSubfolders: return l10n::Get(I::HintActSearchSub);
    case StatusHintAction::LanRescan: return l10n::Get(I::HintActLanRescan);
    default: break;
    }
    static const std::wstring kEmpty;
    return kEmpty;
}

// ---- One-time teaching bubbles -------------------------------------------
// Tip ids double as tips_seen bits: 0 middle-click, 1 tray, 2 compare,
// 3 smart search, 4 collapsed sidebar, 5 quick preview.
namespace {
constexpr int kTeachCount = 6;
constexpr ULONGLONG kTeachHoldMs = 700;      // trigger must hold this long
constexpr ULONGLONG kTeachMinShowMs = 3000;  // before "moved on" dismissal
constexpr ULONGLONG kTeachMaxShowMs = 15000;

bool TeachBusy(const AppState& s) {
    return !s.dropBadge.empty() || s.renameIndex >= 0 || s.addressEditing || s.addressSearching ||
           s.filterEditing || !s.tagRenameId.empty() || s.marqueeActive || GetCapture() != nullptr ||
           GetForegroundWindow() != s.hwnd;
}

int TeachTrigger(AppState& s) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || IsSettingsTab(tab)) return -1;
    const app::LayoutTab* layout = s.window_tabs.Active();
    const int selected = tab->SelectedCount();
    std::wstring kind;
    app::ParsePulsePath(tab->current_path, &kind, nullptr);
    const bool compare = layout && layout->compare;
    if (selected >= 5) return 1;
    if (layout && layout->panes.size() >= 2 && !compare) return 2;
    // Only with results: on an empty page the bubble would cover the suggestion buttons.
    if (kind == L"search") return tab->snapshot && tab->snapshot->size() > 0 ? 3 : -1;
    if (s.appPrefs.sidebar_collapsed) return 4;
    if (selected == 1 && tab->snapshot && tab->selected_index >= 0 &&
        static_cast<size_t>(tab->selected_index) < tab->snapshot->size())
        return (*tab->snapshot)[tab->selected_index].is_dir ? 0 : 5;
    return -1;
}

void HideTeachTip(AppState& s) {
    s.teachTip = -1;
    s.teachCandidate = -1;
}
} // namespace

bool UpdateTeachTip(AppState& s) {
    const ULONGLONG now = GetTickCount64();
    if (s.teachTip >= 0) {
        const ULONGLONG shown = now - s.teachShownAt;
        const bool movedOn = shown >= kTeachMinShowMs && TeachTrigger(s) != s.teachTip;
        if (!s.appPrefs.show_hints || movedOn || shown >= kTeachMaxShowMs || !s.dropBadge.empty()) {
            HideTeachTip(s);
            return true;
        }
        return false;
    }
    if (!s.appPrefs.show_hints || s.teachShownThisSession || TeachBusy(s)) {
        s.teachCandidate = -1;
        return false;
    }
    const int tip = TeachTrigger(s);
    if (tip < 0 || tip >= kTeachCount || (s.appPrefs.tips_seen & (1u << tip))) {
        s.teachCandidate = -1;
        return false;
    }
    if (tip != s.teachCandidate) {
        s.teachCandidate = tip;
        s.teachCandidateSince = now;
        return false;
    }
    if (now - s.teachCandidateSince < kTeachHoldMs) return false;
    // Shown = seen: each tip appears once, even if the user just moves on.
    s.teachTip = tip;
    s.teachShownAt = now;
    s.teachShownThisSession = true;
    s.appPrefs.tips_seen |= 1u << tip;
    s.appPrefs.Save();
    return true;
}

void HandleTeachButton(AppState& s, int button) {
    const int tip = s.teachTip;
    HideTeachTip(s);
    if (button == 2) {
        s.appPrefs.show_hints = false;
        s.appPrefs.Save();
    } else if (button == 0 && tip == 2) {
        DispatchMenuCommand(s, app::CmdCompareToggle);
    }
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void SearchFilterInSubfolders(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return;
    std::wstring text = tab->filter_text;
    if (s.filterEditing && s.hwndFilterEdit) {
        wchar_t buf[512]{};
        GetWindowTextW(s.hwndFilterEdit, buf, ARRAYSIZE(buf));
        text = buf;
    }
    if (text.empty()) return;
    app::AdvancedSearchSpec spec;
    spec.name = text;
    spec.name_is_query = true;
    spec.location = app::LocationScope::CurrentFolder;
    spec.current_folder = path::StripExtendedPathPrefix(tab->current_path);
    ClearPaneFilter(s);
    NavigateTo(s, app::MakeSearchPath(app::CompileSearchQuery(spec)));
}

StatusHintAction CurrentStatusHintAction(AppState& s) {
    StatusHintAction action = StatusHintAction::None;
    ContextStatusHint(s, action);
    return action;
}

// View-model wrapper: builds the base VM and layers ops-layer status on top.
// probe_details=false: hit-test / input paths must not kick off selection
// probes (or mutate detailsSelPath); paint owns those side effects.
// Compact local time for the tray compare table ("09-30 14:02", other years by date).
static std::wstring TrayCmpTime(const FILETIME& ft) {
    FILETIME local{};
    SYSTEMTIME st{}, now{};
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
    GetLocalTime(&now);
    wchar_t buf[32]{};
    if (st.wYear == now.wYear)
        swprintf_s(buf, L"%02u-%02u %02u:%02u", st.wMonth, st.wDay, st.wHour, st.wMinute);
    else
        swprintf_s(buf, L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

// Two staged files: same rules as the dual-pane compare (mtime 2 s tolerance,
// then size); content only after the user asks (TrayStartContentCompare).
static void FillTrayCompare(AppState& s, const app::TrayItem* const (&items)[2],
                            ui::TrayCompareView& v) {
    const std::wstring paths[2]{items[0]->path, items[1]->path};
    if (!s.trayMetadata) s.trayMetadata = std::make_unique<app::TrayCompareMetadata>();
    const app::TrayMetadataKey key{{items[0]->id, items[1]->id}, {paths[0], paths[1]}};
    const auto metadata = s.trayMetadata->Read(key, s.hwnd);
    uint64_t t[2]{}, sz[2]{};
    std::wstring dir[2];
    for (int i = 0; i < 2; ++i) {
        const std::wstring shown = ClipboardPath(paths[i]);
        const size_t slash = shown.find_last_of(L"\\/");
        v.name[i] = slash == std::wstring::npos ? shown : shown.substr(slash + 1);
        dir[i] = slash == std::wstring::npos ? std::wstring() : shown.substr(0, slash);
        const size_t up = dir[i].find_last_of(L"\\/");
        v.where[i] = up == std::wstring::npos ? dir[i] : dir[i].substr(up + 1);
        v.size[i] = format::ByteSize(items[i]->size);
        if (metadata.status == app::TrayMetadataSnapshot::Status::Ready) {
            const auto& d = metadata.files[static_cast<size_t>(i)];
            t[i] = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) |
                   d.ftLastWriteTime.dwLowDateTime;
            sz[i] = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
            v.time[i] = TrayCmpTime(d.ftLastWriteTime);
            v.size[i] = format::ByteSize(sz[i]);
        }
    }
    // Same leaf folder name in different places: show the full folders.
    if (v.where[0] == v.where[1] && dir[0] != dir[1]) {
        v.where[0] = dir[0];
        v.where[1] = dir[1];
    }
    if (metadata.status != app::TrayMetadataSnapshot::Status::Ready) {
        v.newer = -1;
        v.content = metadata.status == app::TrayMetadataSnapshot::Status::Error ? 4 : 1;
        return;
    }
    constexpr uint64_t kTolerance = 2ull * 10000000ull; // 2 s in FILETIME units
    v.newer = t[0] > t[1] + kTolerance ? 0 : (t[1] > t[0] + kTolerance ? 1 : -1);
    v.size_differs = sz[0] != sz[1];
    if (v.size_differs)
        v.content = 3;
    else if (s.trayCmpJob && s.trayCmpJob->a == paths[0] && s.trayCmpJob->b == paths[1])
        v.content = s.trayCmpJob->state.load();
    else
        v.content = 0;
    if (v.content != 3) return;
    // Different: find out (once per pair, off the UI thread) whether both are text.
    const auto& probe = s.trayTextProbe;
    if (probe && probe->a == paths[0] && probe->b == paths[1]) {
        const int state = probe->state.load();
        v.text = state == 1 ? 1 : (state == 2 ? 0 : -1);
        return;
    }
    auto job = std::make_shared<TrayTextProbe>();
    job->a = paths[0];
    job->b = paths[1];
    s.trayTextProbe = job;
    const HWND hwnd = s.hwnd;
    std::thread([job, hwnd] {
        const bool text = diff::ProbeLooksText(job->a) && diff::ProbeLooksText(job->b);
        job->state.store(text ? 1 : 2);
        InvalidateRect(hwnd, nullptr, FALSE);
    }).detach();
}

// Type filter of the open view: 1 images, 2 documents, 3 other (folders too).
static int TrayItemKind(const std::wstring& name, bool is_dir) {
    if (is_dir) return 3;
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return 3;
    std::wstring ext = name.substr(dot + 1);
    for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    static const wchar_t* const kImages[] = {L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"heic", L"heif",
        L"tif", L"tiff", L"svg", L"ico", L"avif", L"jxl", L"raw", L"cr2", L"nef", L"arw", L"dng", L"psd"};
    static const wchar_t* const kDocs[] = {L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"txt",
        L"md", L"csv", L"rtf", L"odt", L"ods", L"odp", L"epub", L"pages", L"numbers", L"key", L"json",
        L"xml", L"html", L"htm", L"log", L"tsv"};
    for (const wchar_t* e : kImages) if (ext == e) return 1;
    for (const wchar_t* e : kDocs) if (ext == e) return 2;
    return 3;
}

// Open view rows: the filtered items grouped by source folder (staging order).
static void FillTrayOpen(AppState& s, ui::TrayDeckView& deck, ULONGLONG now) {
    auto& v = s.trayView;
    auto progress = [now](ULONGLONG at, float ms) {
        return now >= at ? std::clamp(static_cast<float>(now - at) / ms, 0.0f, 1.0f) : 0.0f;
    };
    // Checks follow the tray: removed or released items drop out.
    v.checked.erase(std::remove_if(v.checked.begin(), v.checked.end(), [&](uint64_t id) {
        for (const auto& b : s.tray.batches())
            for (const auto& item : b.items)
                if (item.id == id) return !item.exists;
        return true;
    }), v.checked.end());
    struct Group { std::wstring path; std::vector<const app::TrayItem*> items; };
    std::vector<Group> groups;
    for (const auto& b : s.tray.batches()) {
        for (const auto& item : b.items) {
            const std::wstring name = TrayItemName(item.path);
            const int kind = TrayItemKind(name, item.is_dir);
            ++deck.filter_counts[0];
            ++deck.filter_counts[kind];
            if (item.exists) ++deck.live_total;
            if (v.filter != 0 && kind != v.filter) continue;
            const std::wstring parent = fs::ParentPath(item.path);
            auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) {
                return _wcsicmp(x.path.c_str(), parent.c_str()) == 0; });
            if (g == groups.end()) { groups.push_back({parent, {}}); g = std::prev(groups.end()); }
            g->items.push_back(&item);
        }
    }
    if (v.filter != 0 && deck.filter_counts[v.filter] == 0) v.filter = 0;
    deck.filter = v.filter;
    deck.checked = static_cast<int>(v.checked.size());
    deck.target_count = deck.checked > 0 ? deck.checked : deck.live_total;
    int i = 0;
    for (const Group& g : groups) {
        bool first = true;
        for (const app::TrayItem* item : g.items) {
            ui::TrayOpenRow row;
            row.id = item->id;
            row.path = ClipboardPath(item->path);
            row.name = TrayItemName(item->path);
            row.is_dir = item->is_dir;
            row.attrs = item->attrs;
            row.exists = item->exists;
            row.size = item->is_dir ? std::wstring() : format::ByteSize(item->size);
            row.checked = std::find(v.checked.begin(), v.checked.end(), item->id) != v.checked.end();
            row.group_start = first;
            row.group_path = ClipboardPath(g.path);
            row.group = TrayItemName(g.path);
            if (row.group.empty()) row.group = row.group_path;
            first = false;
            // .oi.in: 60 + 40·i ms delay, 400 ms (the first dozen rows).
            row.in_t = v.opened_at ? progress(v.opened_at + 60 + 40 * static_cast<ULONGLONG>(std::min(i, 12)), 400.0f) : 1.0f;
            if (std::find(v.removing.begin(), v.removing.end(), item->id) != v.removing.end())
                row.out_t = std::max(0.001f, progress(v.remove_at, 320.0f));
            deck.open_rows.push_back(std::move(row));
            ++i;
        }
    }
    // Wheel clamp: headers 27 + rows 36 + 8 against the list (panel - 81 - 80).
    const float content = static_cast<float>(groups.size()) * 27.0f + static_cast<float>(i) * 36.0f + 8.0f;
    v.scroll = std::clamp(v.scroll, 0.0f, std::max(0.0f, content - (s.trayShelf.height - 161.0f)));
    deck.open_scroll = v.scroll;
    // 同名 on the main button counts only what it would release.
    if (deck.checked > 0) {
        int n = 0;
        for (const uint64_t id : s.trayShelf.conflict_ids)
            if (std::find(v.checked.begin(), v.checked.end(), id) != v.checked.end()) ++n;
        deck.conflicts = n;
    }
    // Compare card: exactly two checked existing files.
    deck.cmp_shown = false;
    if (v.compare && v.checked.size() == 2) {
        const app::TrayItem* pair[2]{};
        for (const auto& b : s.tray.batches())
            for (const auto& item : b.items)
                for (int k = 0; k < 2; ++k)
                    if (item.id == v.checked[static_cast<size_t>(k)]) pair[k] = &item;
        if (pair[0] && pair[1] && !pair[0]->is_dir && !pair[1]->is_dir && pair[0]->exists && pair[1]->exists) {
            deck.cmp_shown = true;
            deck.cmp_t = progress(v.compare_at, 450.0f);
            FillTrayCompare(s, pair, deck.compare);
        }
    }
    if (!deck.cmp_shown) v.compare = false;
}

// Shelf part of the tray view (pulse_tray_redesign.html).
static void FillTrayShelf(AppState& s, ui::TrayDeckView& deck, ULONGLONG now) {
    const auto& a = s.trayShelf;
    auto progress = [now](ULONGLONG at, float ms) {
        return at ? std::clamp(static_cast<float>(now - at) / ms, 0.0f, 1.0f) : 1.0f;
    };
    deck.stage = a.stage;
    deck.prev_stage = a.prev_stage;
    deck.swap = a.prev_stage >= 0 ? progress(a.swap_at, 410.0f) : 1.0f;
    deck.height_dip = a.height;
    deck.hide_empty = s.appPrefs.tray_hide_when_empty;
    deck.drop_move = s.dropTrayMove;
    deck.move_mode = TrayEffectiveMove(s);
    deck.mode_t = a.mode_t;
    deck.label_t = progress(a.label_at, 320.0f);
    deck.fan_spread = a.fan_spread;
    deck.bump = progress(a.bump_at, 500.0f);
    deck.zipping = s.trayZip.id != 0;
    deck.zip_progress = s.trayZip.progress ? s.trayZip.progress->load() : 0.0f;
    if (TrayClipLive(s)) {
        deck.clip_count = static_cast<int>(s.trayClip.paths.size());
        deck.clip_t = progress(s.trayClip.at, 450.0f);
    }

    // Target: the current folder, when files can land there.
    std::wstring current;
    if (app::Tab* tab = ActiveTab(s); tab && !tab->current_path.empty() &&
        !fs::IsVirtualPath(tab->current_path) && !tab->net_readonly) {
        current = tab->current_path;
        deck.target = TrayItemName(current);
        if (deck.target.empty()) deck.target = current;
    }
    // 送到 chips and the ▾ menu never offer the folder the main button covers.
    if (!current.empty()) {
        const std::wstring cur = ClipboardPath(fs::NormalizePath(current));
        deck.dests.erase(std::remove_if(deck.dests.begin(), deck.dests.end(),
            [&](const ui::TrayDestView& d) { return _wcsicmp(d.path.c_str(), cur.c_str()) == 0; }),
            deck.dests.end());
    }
    deck.flash_chip = -1;
    if (!a.flash_path.empty() && now - a.flash_at < 900) {
        for (size_t i = 0; i < deck.dests.size(); ++i)
            if (_wcsicmp(deck.dests[i].path.c_str(), a.flash_path.c_str()) == 0) deck.flash_chip = static_cast<int>(i);
        deck.flash_t = progress(a.flash_at, 900.0f);
    }

    // Sources ("来自 A、B 等 n 处") and the fan of the newest three items.
    std::vector<std::wstring> parents;
    std::vector<const app::TrayItem*> newest;
    size_t seen = 0;
    const auto& batches = s.tray.batches();
    for (auto b = batches.rbegin(); b != batches.rend(); ++b) {
        for (auto it = b->items.rbegin(); it != b->items.rend(); ++it) {
            if (it->exists && newest.size() < 3) newest.push_back(&*it);
            if (++seen > 400) continue;  // enough for the summary line
            const std::wstring parent = fs::ParentPath(it->path);
            if (std::none_of(parents.begin(), parents.end(), [&](const std::wstring& p) {
                    return _wcsicmp(p.c_str(), parent.c_str()) == 0; }))
                parents.push_back(parent);
        }
    }
    // Oldest staged source first, like the prototype.
    std::reverse(parents.begin(), parents.end());
    if (!parents.empty()) {
        std::wstring names;
        for (size_t i = 0; i < parents.size() && i < 2; ++i) {
            std::wstring name = TrayItemName(parents[i]);
            if (name.empty()) name = parents[i];
            if (!names.empty()) names += L"\u3001";
            names += name;
        }
        std::wstring text = l10n::Get(parents.size() > 2 ? l10n::StringId::TraySourcesMany
                                                         : l10n::StringId::TraySourcesFew);
        if (const size_t at = text.find(L"{a}"); at != std::wstring::npos) text.replace(at, 3, names);
        if (const size_t at = text.find(L"{n}"); at != std::wstring::npos)
            text.replace(at, 3, std::to_wstring(parents.size()));
        deck.sources_text = std::move(text);
    }
    for (auto it = newest.rbegin(); it != newest.rend(); ++it) {
        ui::TrayCardView card;
        card.path = (*it)->path;
        card.name = TrayItemName((*it)->path);
        card.is_dir = (*it)->is_dir;
        card.attrs = (*it)->attrs;
        card.size = (*it)->size;
        deck.fan.push_back(std::move(card));
    }

    // Same names already in the current folder (cached probe).
    auto& c = s.trayShelf;
    uint64_t sig = 1469598103934665603ull;
    for (const auto& b : batches)
        for (const auto& item : b.items) sig = (sig ^ item.id) * 1099511628211ull;
    if (current.empty()) {
        c.conflicts = 0;
        c.conflict_ids.clear();
    } else if (sig != c.conflict_sig || _wcsicmp(c.conflict_dir.c_str(), current.c_str()) != 0 ||
               now - c.conflict_at > 1500) {
        int n = 0, probed = 0;
        c.conflict_ids.clear();
        const bool remote = fs::IsUncPath(current);
        for (const auto& b : batches) {
            for (const auto& item : b.items) {
                if (remote || ++probed > 200) break;
                if (!item.exists) continue;
                const std::wstring parent = fs::ParentPath(item.path);
                if (_wcsicmp(parent.c_str(), current.c_str()) == 0) continue;
                std::wstring dest = current;
                if (!dest.empty() && dest.back() != L'\\') dest += L'\\';
                dest += TrayItemName(item.path);
                if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    ++n;
                    c.conflict_ids.push_back(item.id);
                }
            }
        }
        c.conflicts = n;
        c.conflict_sig = sig;
        c.conflict_dir = current;
        c.conflict_at = now;
    }
    deck.conflicts = c.conflicts;
    if (deck.stage == static_cast<int>(ui::TrayStage::Open) || deck.prev_stage == static_cast<int>(ui::TrayStage::Open))
        FillTrayOpen(s, deck, now);
}

ui::WindowViewModel BuildVm(AppState& s, bool probe_details) {
    SyncTaskbarWindowTitle(s);
    if (!s.pane) return {};
    TickContentPackInstall(s);
    TickAiPackInstall(s);
    // A finished pack download repaints the window once; drop the cached
    // failed thumbnails so the newly supported files are decoded again.
    if (s.settings.TakeMediaPackResult()) {
        s.renderer.EvictThumbnails();
        s.quickPreview.OnMediaPackInstalled();
    }
    if (s.settings.TakeImagePackResult()) {
        s.renderer.EvictThumbnails();
        s.quickPreview.OnImagePackInstalled();
    }
    const bool raw_installed = s.settings.TakeRawPackResult();
    const bool archive_installed = s.settings.TakeArchivePackResult();
    if (raw_installed || archive_installed) {
        s.renderer.EvictThumbnails();
        s.quickPreview.OnExtraPackInstalled();
    }
    if (s.quickPreview.visible()) {
        ui::MediaPackOffer offer, image_offer;
        s.settings.FillMediaPackOffer(offer);
        s.settings.FillImagePackOffer(image_offer);
        // Fixture for the Quick Look offers while no pack is published.
        wchar_t fake[8]{};
        if (s.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_PACK_OFFER", fake, ARRAYSIZE(fake))) {
            offer.installable = image_offer.installable = true;
            if (!offer.download_bytes) offer.download_bytes = uint64_t{28} << 20;
            if (!image_offer.download_bytes) image_offer.download_bytes = uint64_t{9} << 20;
            if (GetEnvironmentVariableW(L"PULSE_TEST_PACK_PROGRESS", fake, ARRAYSIZE(fake))) {
                offer.installing = image_offer.installing = true;
                offer.progress = image_offer.progress = static_cast<float>(_wtoi(fake)) / 100.0f;
            }
        }
        s.quickPreview.SetMediaPackOffer(offer);
        s.quickPreview.SetImagePackOffer(image_offer);
        ui::CloudDownloadOffer cloud;
        if (fs::IsCloudPlaceholder(s.quickPreview.item().attrs)) {
            cloud.path = s.quickPreview.item().path;
            cloud.percent = app::CloudDownloadPercent(cloud.path);
            cloud.provider = app::CloudRootLabelFor(cloud.path);
        }
        s.quickPreview.SetCloudDownloadOffer(cloud);
    }
    UpdateFolderCompare(s);
    s.changes.visible_paths.clear();
    ForEachPane(s, [&](app::Pane& pane) {
        if (auto* tab = pane.ActiveTab()) {
            tab->SetShowHiddenFiles(s.appPrefs.show_hidden_files);
            tab->SetShowProtectedOsFiles(s.appPrefs.show_protected_os_files);
        }
    });
    SyncColumnStrips(s);
    SyncDetailsTrees(s);
    UpdateWallHint(s);
    ui::WindowViewModel vm = app::BuildWindowViewModel(*s.pane, s.sidebar,
        s.pane->focused, s.maximized, s.darkMode, &s.places, s.sidebarCollapsedMask,
        SidebarHiddenMaskWithBar(s), s.starredExpanded, &s.sidebarOrder,
        s.sidebarQuickAccessHiddenMask);
    if (const app::Tab* tab = ActiveTab(s)) {
        FillWallHint(*tab, vm.pane);  // photo wall suggestion
        // A writable phone folder takes 新建文件夹 (CreateDeviceFolder), as its
        // background menu and Ctrl+Shift+N already do; the toolbar follows.
        if (IsDeviceTab(tab) && app::DeviceRegistry::Instance().FolderWritable(tab->current_path))
            vm.pane.can_create_folder = true;
    }
    app::FillWindowTabStrip(vm, s.window_tabs);
    // Rail: devices found on this network that no window has shown yet.
    if (const int badge = NetworkRailBadge(s); badge > 0) {
        for (auto& group : vm.sidebar)
            if (group.id == static_cast<int>(app::SidebarSectionId::Networks)) group.badge_count = badge;
    }
    // Open settings shows as the selected settings button, not as a tab.
    if (s.window_tabs.active < vm.tabs.size() && IsSettingsTab(ActiveTab(s)))
        vm.tabs[s.window_tabs.active].hidden = true;
    if (WindowIdentityShown(s) && app::ValidWindowColor(s.identity.color)) {  // M7c
        vm.window_identity = true;
        vm.window_identity_name = WindowDisplayName(s);
        vm.window_identity_rgb = app::kWindowColors[s.identity.color];
    }
    vm.show_pinned_tab_names = s.appPrefs.show_pinned_tab_names;
    vm.settings_list_smart_date = s.appPrefs.list_smart_date;
    vm.settings_list_zebra_rows = s.appPrefs.list_zebra_rows;
    vm.settings_list_tree_sticky = s.appPrefs.list_tree_sticky;
    vm.settings_menu_acrylic = s.appPrefs.menu_acrylic;
    vm.settings_list_size_bar = s.appPrefs.list_size_bar;
    vm.settings_list_tag_names = s.appPrefs.list_tag_name_color;
    vm.settings_list_selection_outline = s.appPrefs.list_selection_outline;
    vm.settings_list_thumbnail_badges = s.appPrefs.list_thumbnail_badges;
    vm.settings_mtime = app::MtimeHintFromPrefs(s.appPrefs);
    vm.settings_mtime.tree = s.appPrefs.list_mtime_tree;   // the switch shows the pref itself
    vm.settings_sync_onedrive = s.appPrefs.sync_onedrive;
    vm.settings_sync_svn = s.appPrefs.sync_svn;
    vm.settings_single_click = s.appPrefs.single_click_open;
    vm.settings_name_highlight = s.appPrefs.name_highlight;
    vm.settings_hover_select = s.appPrefs.hover_select;
    vm.settings_item_checkboxes = s.appPrefs.item_checkboxes;
    vm.settings_vertical_tabs = s.appPrefs.vertical_tabs;
    vm.settings_tray_hide_empty = s.appPrefs.tray_hide_when_empty;
    vm.settings_lan_discovery = s.appPrefs.lan_discovery;
    vm.settings_network_recent = s.appPrefs.network_recent_enabled;
    vm.settings_hidden_shares = s.appPrefs.show_hidden_shares;
    vm.settings_lan_cache_mode = s.appPrefs.lan_cache_mode;
    vm.settings_show_hints = s.appPrefs.show_hints;
    vm.settings_tips_seen = s.appPrefs.tips_seen != 0;
    vm.settings_folder_sort = s.appPrefs.folder_sort_mode;
    vm.settings_startup_open = s.appPrefs.startup_open;
    vm.settings_notify_icon = s.appPrefs.notify_icon_mode;
    vm.settings_new_tab_open = s.appPrefs.new_tab_open;
    vm.settings_external_open = s.appPrefs.external_open;
    vm.settings_close_last_tab = s.appPrefs.close_window_with_last_tab;
    vm.settings_close_tab_double_click = s.appPrefs.close_tab_on_double_click;
    vm.settings_confirm_delete = s.appPrefs.confirm_recycle_delete;
    vm.settings_home_folder = s.appPrefs.home_folder;
    vm.settings_text_render = s.appPrefs.text_render;
    vm.settings_ui_font_scale = s.appPrefs.ui_font_scale;
    vm.settings_ui_font_family = s.appPrefs.ui_font_family;
    std::copy_n(s.settings.custom_accent(), 3, vm.settings_accent_hsl);
    vm.sidebar_scroll = s.sidebarScroll;
    vm.sidebar_scrollbar_opacity = s.sidebarScrollbarFade.Opacity();
    vm.sidebar_scrollbar_expand = s.sidebarScrollbarFade.Expand();
    if (s.groupDragActive) {
        vm.sidebar_group_drag_id = s.groupDragId;
        if (s.groupGapVisible) vm.sidebar_group_gap_line_y = s.groupGapLineY;
    }
    if (s.pinDragActive) {
        vm.sidebar_pin_drag_index = s.pinDragRun;
        if (s.pinGapVisible) vm.sidebar_pin_gap_line_y = s.pinGapLineY;
        POINT cursor{};
        if (s.hwnd && GetCursorPos(&cursor) && ScreenToClient(s.hwnd, &cursor))
            vm.sidebar_pin_drag_dy = static_cast<float>(cursor.y - s.pinDragStartPt.y);
    }
    if (app::Tab* sel_tab = ActiveTab(s); sel_tab && sel_tab->content_results && sel_tab->SelectedCount() > 1) {
        wchar_t count[64]{};
        swprintf_s(count, l10n::Get(l10n::StringId::SelectedCountFormat).c_str(), sel_tab->SelectedCount());
        const std::optional<uint64_t> bytes = ContentSelectionSize(*sel_tab);
        vm.status.selection_text = std::wstring(count) + L"  \u00B7  " +
            (bytes ? pulse::format::ByteSize(*bytes, true) : l10n::Get(l10n::StringId::LoadingEllipsis));
    }
    ops::OpStatus st = s.ops.Status();
    if (st.active || !st.last_error.empty() || !st.summary.empty()) {
        vm.status.task_text = st.last_error.empty() ? st.summary
            : st.summary + L" — " + st.last_error;
        vm.status.task_progress = st.active ? st.percent : -1.0f;
        vm.status.task_active = st.active;
        vm.status.task_failed = !st.last_error.empty();
        vm.status.task_paused = st.active && (st.phase == ops::OpPhase::Paused || st.pause_requested);
        if (!st.active && st.phase == ops::OpPhase::Completed && st.last_error.empty()) {
            // The finished pill offers 撤销 / 打开 (pulse_ops_dialogs.html).
            vm.status.task_can_undo = s.ops.CanUndoTask(st.task_id);
            vm.status.task_can_open = (st.type == ops::OpType::Copy || st.type == ops::OpType::Move) &&
                                      !st.destination_dir.empty();
        }
        if (!st.active && st.phase == ops::OpPhase::Failed && st.failed_items > 0 && !st.failures.empty()) {
            // Partial failure: what did not go through, and where to look.
            vm.status.task_failed_items = st.failed_items;
            const std::wstring count = pulse::format::GroupedInt(st.failed_items);
            wchar_t pill[160]{};
            swprintf_s(pill, l10n::Get(l10n::StringId::OpPillFailed).c_str(), count.c_str());
            vm.status.task_text = pill;
        }
        if(s.contentSelectionAction) vm.status.selection_text=l10n::Get(l10n::StringId::OpPreparingList);
    }
    app::ApplyUpdateStatus(vm.status, UpdateProgressForView(s), st.active);
    {
        std::wstring idx = l10n::ServiceText(s.index.Status());
        app::Tab* active = ActiveTab(s);
        std::wstring virtual_kind;
        std::wstring virtual_rest;
        const bool search_visible = active &&
            app::ParsePulsePath(active->current_path, &virtual_kind, &virtual_rest) &&
            virtual_kind == L"search";
        if (!idx.empty() && !vm.status.query_active && (s.paletteSearching || search_visible)) {
            if (!vm.status.status_text.empty()) vm.status.status_text += L"  ·  ";
            vm.status.status_text += idx;
        }
    }
    if (s.showFps) {
        wchar_t perf[256];
        swprintf_s(perf, l10n::Get(l10n::StringId::PerformanceFormat).c_str(),
            s.lastFrameMs, s.lastFps, s.timing.enum_ms, s.timing.sort_ms,
            s.processCpuPercent, s.workingSetMb);
        wchar_t breakdown[96];
        swprintf_s(breakdown, L"  \u00B7  D %.1f ms  \u00B7  P %.1f ms",
            s.timing.draw_ms, s.timing.present_ms);
        vm.status.performance_text = std::wstring(perf) + breakdown;
        wchar_t lumaStatsEnabled[8]{};
        const bool showLumaStats = GetEnvironmentVariableW(
            L"PULSE_LUMATEXT_STATS", lumaStatsEnabled, ARRAYSIZE(lumaStatsEnabled)) > 0;
        wchar_t compactPerf[160];
        const auto* lumaStats = showLumaStats ? s.compositor.GetLumaTextStats() : nullptr;
        if (lumaStats && s.compositor.LumaTextEnabled()) {
            swprintf_s(compactPerf,
                L"DEV %.1f ms \u00B7 D %.1f \u00B7 P %.1f \u00B7 %.0f FPS \u00B7 LT %llu/%llu E%llu \u00B7 %.0f MB",
                s.lastFrameMs, s.timing.draw_ms, s.timing.present_ms, s.lastFps,
                static_cast<unsigned long long>(lumaStats->surface_cache_hits),
                static_cast<unsigned long long>(lumaStats->surface_cache_misses),
                static_cast<unsigned long long>(lumaStats->surface_cache_evictions),
                s.workingSetMb);
        } else {
            swprintf_s(compactPerf, L"DEV  %.1f ms  \u00B7  D %.1f  \u00B7  P %.1f  \u00B7  %.0f FPS  \u00B7  %.0f MB",
                s.lastFrameMs, s.timing.draw_ms, s.timing.present_ms,
                s.lastFps, s.workingSetMb);
        }
        vm.status.performance_compact_text = compactPerf;
    } else {
        const auto region = static_cast<ui::HitTestResult::Region>(s.hoverRegion);
        if (HoverHintIsCommand(region) && !s.addressSearching && !s.filterEditing) {
            // Tooltips may carry extra lines; the status bar only has room for the first.
            vm.status.hint_text = TooltipForHover(s);
            const size_t nl = vm.status.hint_text.find(L'\n');
            if (nl != std::wstring::npos) vm.status.hint_text.resize(nl);
        }
        if (vm.status.hint_text.empty()) {
            StatusHintAction action = StatusHintAction::None;
            vm.status.hint_text = ContextStatusHint(s, action);
            // The search-box keys are not keymap actions (Alt+Enter there is not 属性).
            if (!s.addressSearching) vm.status.hint_text = app::RemapShortcutText(vm.status.hint_text);
            if (action != StatusHintAction::None) {
                vm.status.hint_action = static_cast<int>(action);
                vm.status.hint_action_text = StatusHintActionText(action) + L" \u2192";
            }
        }
    }
    pulse::FillAiStatusVm(vm);   // the AI chip at the left end of the status bar
    // "已暂存" row pills (tray redesign).
    vm.pane.staged = StagedHashes(s);
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
    if (!s.leavingPaths.empty()) {
        if (app::Tab* tab = ActiveTab(s)) FillLeavingRows(s, tab->current_path, vm.pane);
    }
    if (app::Tab* tab = ActiveTab(s)) vm.pane.ai_list = AiListOverlayFor(tab->current_path);   // Pulse 助手
    if (!s.dragLanding.names.empty()) {
        if (app::Tab* tab = ActiveTab(s)) FillLandingRows(s, s.pane, tab->current_path, vm.pane);
    }
    vm.pane.drop_target_index = s.dropRow;
    vm.pane.drop_spring_progress = DropSpringProgress(s);
    vm.pane.rename_index = s.renameIndex;
    vm.pane.rename_text = RenameEditText(s);
    FillRenameHint(s, vm.pane);
    SyncSearchBarWidth(s);
    vm.address_editing = s.addressEditing;
    vm.address_searching = s.addressSearching;
    vm.address_suggestions = s.addressSuggest;
    vm.address_suggestion_header = s.addressSuggestHeader;
    vm.address_suggestion_selected = s.addressSuggestSel;
    FillFilterTagSuggestionsVm(s, vm);
    vm.address_search_current = s.addressSearchCurrent;
    vm.address_search_has_text = s.addressSearching && s.hwndAddressEdit &&
                                GetWindowTextLengthW(s.hwndAddressEdit) > 0;
    vm.address_search_animation = s.addressSearchAnimation;
    vm.address_scope_animation = s.addressScopeAnimation;
    FillAddressSearchView(s, vm);
    vm.splitter_pressed = s.splitterDragging;
    vm.details_resize_pressed = s.detailsPanelResizing;
    if (s.marqueeActive) {
        vm.pane.marquee_active = true;
        vm.pane.marquee_rect = MarqueeDisplayRect(s);
    }
    vm.breadcrumb_hover = s.breadcrumbHover;
    vm.breadcrumb_drop = s.dropBreadcrumb;
    vm.sidebar_drop_index = s.dropSidebar;
    vm.tab_drop_index = s.tabDropIndex;
    vm.tab_file_drop = s.springTab;
    if (s.springTab >= 0)
        vm.tab_file_drop_progress =
            static_cast<float>(GetTickCount64() - s.springTabStart) / static_cast<float>(app::kTabSpringMs);
    vm.content_drop = s.contentDrop;
    vm.content_drop_as_tab = s.contentDropAsTab;
    vm.content_drop_rects = s.contentDropRects;
    vm.drag_ghost = s.dragGhost;
    // Sidebar tags: mark the row being renamed.
    int tagsGroup = -1;
    for (int g = 0; g < static_cast<int>(vm.sidebar.size()); ++g) {
        if (vm.sidebar[g].id == static_cast<int>(app::SidebarSectionId::Tags)) { tagsGroup = g; break; }
    }
    if (tagsGroup >= 0) {
        auto& items = vm.sidebar[tagsGroup].items;
        if (!s.tagRenameId.empty()) {
            const std::wstring editing_path = app::MakeTagPath(s.tagRenameId);
            for (auto& item : items) item.editing = item.path == editing_path;
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
            POINT cursor{};
            if (s.window_tabs.items.size() >= 2 && !s.tabDragFromChip && s.hwnd &&
                GetCursorPos(&cursor) && ScreenToClient(s.hwnd, &cursor)) {
                const float dy = static_cast<float>(cursor.y - s.tabDragStartPt.y);
                const float limit = 24.0f * s.scale;  // kTabDetachDistanceDip
                vm.tab_detach_dy = std::clamp(dy, -limit, limit);
                vm.tab_detach_pull = std::clamp(std::fabs(dy) / limit, 0.0f, 1.0f);
            }
            vm.tab_drag_count = s.tabDragRunLen;
            vm.tab_drag_chip = s.tabDragFromChip;
            if (s.tabDragRunLen > 1) vm.tab_drag_index = s.tabDragRunPos;
            const int origActive = static_cast<int>(s.window_tabs.active);
            for (int pos = 0; pos < static_cast<int>(s.tabOrder.size()); ++pos) {
                const bool isActive = s.tabOrder[pos] == origActive;
                vm.tabs[static_cast<size_t>(pos)].active = isActive;
                if (isActive) vm.active_tab = pos;
            }
        }
        for (int pos = 0; pos < static_cast<int>(vm.tabs.size()); ++pos) {
            const int orig = dragging ? s.tabOrder[pos] : pos;
            if (orig < 0 || orig >= static_cast<int>(s.window_tabs.items.size())) continue;
            const app::LayoutTab* key = s.window_tabs.items[static_cast<size_t>(orig)].get();
            const auto off = s.tabOffsets.find(key);
            if (off != s.tabOffsets.end())
                vm.tabs[static_cast<size_t>(pos)].x_offset = off->second;
            vm.tabs[static_cast<size_t>(pos)].flash = TabFlashAmount(s, key);
        }
        for (size_t gi = 0; gi < vm.tab_groups.size(); ++gi) {
            const auto off = s.chipOffsets.find(vm.tab_groups[gi].id);
            if (off != s.chipOffsets.end()) vm.tab_groups[gi].x_offset = off->second;
        }
    }
    // Staging tray card stack: live window (top first) + exiting ghosts.
    {
        const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(TrayStackTop(s)),
                                             static_cast<size_t>(TrayDeckCap(s)));
        ui::TrayDeckView& deck = vm.tray_deck;
        deck.open = s.trayOpen;
        deck.spread = s.traySpread;
        deck.thumb_dip = static_cast<float>(s.appPrefs.tray_icon_size);
        deck.offset = TrayStackTop(s);
        deck.total_count = TrayItemTotalCount(s.tray);
        deck.batch_count = static_cast<int>(s.tray.batches().size());
        deck.release_move = !s.tray.batches().empty() && s.tray.batches().back().move_intent;
        deck.live_count = static_cast<int>(entries.size());
        for (const auto& b : s.tray.batches())
            for (const auto& item : b.items)
                if (!item.exists) ++deck.stale_count;
        {
            // Two-file compare: exactly two staged items, both existing files.
            const app::TrayItem* pair[2]{};
            int n = 0;
            bool files = true;
            for (const auto& b : s.tray.batches()) {
                for (const auto& item : b.items) {
                    if (n < 2) pair[n] = &item;
                    ++n;
                    files = files && item.exists && !item.is_dir;
                }
            }
            deck.can_compare = n == 2 && files;
            deck.comparing = deck.can_compare && s.trayCompare;
            if (deck.comparing) FillTrayCompare(s, pair, deck.compare);
            else if (s.trayMetadata && !s.trayView.compare) s.trayMetadata->Cancel();
        }
        // Live sum: items removed one by one leave the batch totals stale.
        for (const auto& b : s.tray.batches())
            for (const auto& item : b.items)
                if (item.exists && !item.is_dir) deck.total_size += item.size;
        for (const auto& dir : TrayDestList(s)) {
            ui::TrayDestView d;
            d.path = dir;
            d.label = BaseName(dir);
            if (d.label.empty()) d.label = dir;
            // Network folders are not probed per frame (a dead share would stall).
            d.missing = !fs::IsUncPath(dir) && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES;
            deck.dests.push_back(std::move(d));
        }
        const int hover_index = TrayDeckHoverIndex(s);
        deck.hovered = hover_index >= 0 && hover_index < deck.live_count ? hover_index : -1;
        auto fill = [&s](ui::TrayCardView& card, const AppState::TrayCardAnim& anim) {
            card.name = anim.name;
            card.folder = anim.folder;
            card.depth = anim.depth;
            card.hover = anim.hover;
            card.appear = anim.appear;
            card.fly = anim.fly;
            card.dx = anim.dx;
            card.dy = anim.dy;
            card.angle = anim.angle;
            card.shrink = anim.shrink;
            card.opacity = anim.opacity;
            card.on_top = anim.motion == AppState::TrayMotion::ThrowOut ||
                          (anim.ghost && anim.exit == AppState::TrayExit::Tumble) ||
                          (s.trayDrag.active && !anim.ghost);
        };
        for (int i = 0; i < deck.live_count; ++i) {
            const TrayDeckEntry& entry = entries[static_cast<size_t>(i)];
            const app::TrayItem& item = *entry.item;
            ui::TrayCardView card;
            card.path = item.path;
            card.batch = entry.batch;
            card.sub = entry.sub;
            card.size = item.size;
            card.is_dir = item.is_dir;
            card.attrs = item.attrs;
            card.missing = !item.exists;
            card.cut = entry.batch >= 0 &&
                entry.batch < static_cast<int>(s.tray.batches().size()) &&
                s.tray.batches()[static_cast<size_t>(entry.batch)].move_intent;
            const auto found = s.trayCards.find(item.path);
            if (found != s.trayCards.end() && !found->second.ghost) {
                fill(card, found->second);
                card.on_top = card.on_top && (i == 0 ||
                    found->second.motion == AppState::TrayMotion::ThrowOut);
            } else {
                card.name = TrayItemName(item.path);
                card.depth = static_cast<float>(i);
            }
            // A collect that the deck tick has not seen yet stays hidden for
            // this one frame, so the drop-in never flashes the settled card.
            if (i == 0 && TrayItemTotalCount(s.tray) > static_cast<int>(s.trayDeckLastTotal))
                card.appear = 0.0f;
            deck.cards.push_back(std::move(card));
        }
        for (const auto& [key, anim] : s.trayCards) {
            if (!anim.ghost) continue;
            ui::TrayCardView card;
            card.path = key;
            card.is_dir = anim.is_dir;
            card.attrs = anim.attrs;
            card.missing = anim.missing;
            card.cut = anim.cut;
            card.size = anim.size;
            card.ghost = true;
            fill(card, anim);
            deck.cards.push_back(std::move(card));
        }
        const ULONGLONG now = ui::motion::NowMs();
        for (const auto& p : s.trayPuffs) {
            if (now < p.start) continue;
            ui::TrayPuffView view;
            view.t = std::clamp(static_cast<float>(now - p.start) / 620.0f, 0.0f, 1.0f);
            view.angle = p.angle;
            view.dist = p.dist;
            view.size = p.size;
            deck.puffs.push_back(view);
        }
        FillTrayShelf(s, deck, now);
    }
    // Vertical tabs join the sidebar before its scroll range is measured.
    ApplyVerticalTabs(s, vm);
    FillFavoritesBar(s, vm);
    // The tray (including exiting cards) determines the sidebar viewport.
    // Clamp only after it is populated, using the same model as draw/hit-test.
    s.sidebarScroll = std::clamp(s.sidebarScroll, 0.0f, s.renderer.SidebarMaxScroll(
        vm, static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height())));
    vm.sidebar_scroll = s.sidebarScroll;
    // Right details panel: selection info + size walk + tag chips.
    vm.details_visible = s.showDetailsPanel;
    vm.layout_preset = static_cast<int>(LayoutOf(s));
    std::wstring sizeTarget; // folder that should be walking ("" = none)
    if (s.showDetailsPanel) {
        ui::DetailsPanelView& dv = vm.details;
        dv.scroll_y = s.detailsScroll;
        dv.preview_only = s.detailsPreviewOnly;
        dv.preview_enabled = s.detailsPreviewEnabled;
        dv.preview_expansion = s.detailsPreviewExpansion;
        dv.collapsed_mask = s.detailsCollapsedMask;
        app::Tab* tab = ActiveTab(s);
        const int selCount = tab ? tab->SelectedCount() : 0;
        if (tab && selCount >= 1) {
            dv.has_selection = true;
            dv.multi_count = selCount;
        }
        if (dv.has_selection && selCount == 1 && tab->snapshot &&
            tab->selected_index >= 0 &&
            tab->selected_index < static_cast<int>(tab->EntryCount())) {
            const fs::DirEntry& e = tab->EntryAt(static_cast<size_t>(tab->selected_index));
            const bool penetrated = !e.link_target.empty();
            dv.name = e.name;
            dv.is_dir = penetrated ? e.link_target_is_dir : e.is_dir;
            dv.attrs = e.attrs;
            dv.link_kind = fs::ClassifyLink(e.attrs, e.reparse_tag);
            dv.is_link = dv.link_kind != fs::LinkKind::None || (!e.is_dir && ui::IsShortcutName(e.name));
            if (penetrated && e.link_target_is_dir) dv.attrs |= FILE_ATTRIBUTE_DIRECTORY;
            dv.size_value = penetrated ? e.link_target_size : e.size;
            const FILETIME& shown_mtime = penetrated ? e.link_target_mtime : e.mtime;
            dv.modified_value = (static_cast<uint64_t>(shown_mtime.dwHighDateTime) << 32) |
                                shown_mtime.dwLowDateTime;
            dv.view_generation = tab->view_generation;
            dv.path = penetrated ? e.link_target
                                 : EntryFullPath(*tab, tab->selected_index);
            // 下载并预览 (C4b): only the entry itself; a shortcut's target is
            // not what the well would download.
            dv.cloud_online = !penetrated && !dv.is_dir && fs::IsCloudPlaceholder(e.attrs);
            if (dv.cloud_online) {
                dv.cloud_download_percent = app::CloudDownloadPercent(dv.path);
                dv.cloud_provider = app::CloudRootLabelFor(dv.path);
            }
            // Directory refreshes replace the snapshot while the selected
            // path stays the same. Keep the details panel's cached metadata
            // aligned with the refreshed entry instead of showing its old
            // modification time indefinitely. For shortcuts, compare the
            // resolved target path shown in the details panel.
            if (s.detailsSelPath == dv.path &&
                (shown_mtime.dwHighDateTime != 0 || shown_mtime.dwLowDateTime != 0)) {
                s.detailsModified = shown_mtime;
            }
        }
        if (dv.has_selection && selCount > 1 && tab->snapshot) {
            uint64_t knownSize = 0;
            int files = 0, folders = 0;
            std::optional<uint64_t> contentSize;
            if(tab->content_results) {
                files=selCount;contentSize=ContentSelectionSize(*tab);knownSize=contentSize.value_or(0);
            } else {
                tab->SelectionSizeSummary(&knownSize, &files, &folders);
            }
            wchar_t composition[96]{};
            if (files && folders)
                swprintf_s(composition,
                    l10n::Get(l10n::StringId::FilesFoldersFormat).c_str(), files, folders);
            else if (files)
                swprintf_s(composition,
                    l10n::Get(l10n::StringId::FilesOnlyFormat).c_str(), files);
            else
                swprintf_s(composition,
                    l10n::Get(l10n::StringId::FoldersOnlyFormat).c_str(), folders);
            dv.type_text = composition;
            dv.size_text = tab->content_results && !contentSize ? l10n::Get(l10n::StringId::LoadingEllipsis) : pulse::format::ByteSize(knownSize, true);
            dv.location_text = fs::IsVirtualPath(tab->current_path)
                ? l10n::Get(l10n::StringId::MultipleLocations)
                : TrayDisplayPath(tab->current_path);
        }
        if (dv.has_selection && dv.multi_count == 1 && !dv.path.empty()) {
            if (probe_details && s.detailsSelPath != dv.path) {
                s.detailsSelPath = dv.path;
                s.detailsScroll = 0.0f;
                dv.scroll_y = 0.0f;
                // Clear stale facts immediately; GetFileAttributesEx /
                // SHGetFileInfo / security APIs run off-thread (WM_DETAILS_META).
                s.detailsSelValid = false;
                s.detailsTypeName.clear();
                s.detailsMetaPath.clear();
                s.detailsOwner.clear();
                s.detailsPermissions.clear();
                s.detailsDrive.clear();
                s.detailsFileSystem.clear();
                s.detailsFreeSpace.clear();
                if (!fs::IsVirtualPath(dv.path))
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
            dv.type_text = dv.link_kind != fs::LinkKind::None
                ? ui::LinkTypeText(dv.link_kind) : s.detailsTypeName;
            if (!dv.is_dir) {
                dv.subtitle_text = dv.type_text;
                if (!dv.subtitle_text.empty()) dv.subtitle_text += L" · ";
                dv.subtitle_text += pulse::format::ByteSize(dv.size_value, true);
                dv.size_text = pulse::format::ByteSize(dv.size_value, true) + L" (" +
                               pulse::format::GroupedInt(dv.size_value) +
                               pulse::l10n::Get(pulse::l10n::StringId::Bytes).c_str();
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
            swprintf_s(contains, l10n::Get(l10n::StringId::ContainsFormat).c_str(),
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
    vm.drag_badge_move = s.dropBadgeMove && !s.dropBadge.empty();
    vm.hover_region = s.hoverRegion;
    vm.hover_control_index = s.hoverControlIndex;
    vm.hover_sub_index = s.hoverSubIndex;
    if (s.shot.active && s.isolatedTest) {
        // Hidden checks of the 右键菜单 preview: PULSE_SHOT_MENU_HOVER=<item>
        // (or f1 / f2 for the fixed rows) hovers a row; PULSE_SHOT_MENU_DRAG=
        // <item>,<px> holds it with the pointer at that client coordinate.
        wchar_t v[32]{};
        if (GetEnvironmentVariableW(L"PULSE_SHOT_MENU_HOVER", v, ARRAYSIZE(v)) > 0) {
            vm.hover_region = static_cast<int>(ui::HitTestResult::SettingsToggle);
            vm.hover_control_index = v[0] == L'f' ? ui::kSettingsMenuFixedHit + _wtoi(v + 1)
                                                  : ui::kSettingsMenuRowHit + _wtoi(v);
        }
        if (GetEnvironmentVariableW(L"PULSE_SHOT_MENU_DRAG", v, ARRAYSIZE(v)) > 0) {
            wchar_t* end = nullptr;
            vm.settings_menu_drag = static_cast<int>(wcstol(v, &end, 10));
            if (end && *end == L',') vm.settings_menu_drag_pos = static_cast<float>(wcstod(end + 1, nullptr));
        }
    }
    vm.hover_pane_index = s.hoverPaneIndex;
    vm.column_resize_pressed = s.columnResizing;
    // No hover tips while a marquee owns the pointer.
    vm.tooltip_text = s.marqueeActive ? std::wstring{} : s.tooltipText;
    if (s.teachTip >= 0) {
        using I = l10n::StringId;
        static constexpr I kTitles[] = {I::TeachMiddleTitle, I::TeachTrayTitle, I::TeachCompareTitle,
                                        I::TeachSearchTitle, I::TeachSidebarTitle, I::TeachPreviewTitle};
        static constexpr I kBodies[] = {I::TeachMiddleBody, I::TeachTrayBody, I::TeachCompareBody,
                                        I::TeachSearchBody, I::TeachSidebarBody, I::TeachPreviewBody};
        vm.teach.visible = true;
        vm.teach.title = l10n::Get(kTitles[s.teachTip]);
        vm.teach.body = l10n::Get(kBodies[s.teachTip]);
        vm.teach.primary = s.teachTip == 2 ? l10n::Get(I::TeachTry) + L" \u2192" : l10n::Get(I::TeachGotIt);
        vm.teach.never = l10n::Get(I::TeachNever);
    }
    vm.tooltip_x = static_cast<float>(s.hoverPoint.x);
    vm.tooltip_y = static_cast<float>(s.hoverPoint.y);
    pulse::FillAiAssistantVm(s, vm);   // Pulse 助手 and its chip (after FillAiStatusVm, may set a tip)
    FillPaneSlots(s, vm);
    FillChangePopover(s, vm);
    FillFolderSizes(s, vm);
    vm.window_effect = ui::WindowEffectFromId(s.appPrefs.window_effect);
    vm.background_image = s.appPrefs.background_image;
    vm.wallpaper_look = s.appPrefs.wallpaper_look;
    vm.wallpaper_blur = s.appPrefs.wallpaper_blur;
    vm.frame_transparency = s.appPrefs.frame_transparency;
    // Also set here (not only when painting) so hit testing sees the
    // same enabled 侧边栏与标题栏 slider as the frame that drew it.
    vm.backdrop_enabled = !(s.shot_high_contrast || ui::IsHighContrast()) &&
                          s.compositor.UsesTransparentComposition() && s.backdropActive;
    vm.safe_mode = s.safeMode;
    {
        // Toolbar "Group" button: views that draw group headers, in groupable
        // places only (same rule as the Sort menu's group section).
        int toolbar_group = -1;
        if (const app::Tab* gt = ActiveTab(s)) {
            std::wstring kind;
            app::ParsePulsePath(gt->current_path, &kind, nullptr);
            const bool can_group = !gt->content_results &&
                ((!gt->current_path.empty() && !fs::IsVirtualPath(gt->current_path)) || kind == L"recent" ||
                 kind == L"search" || kind == L"saved-search" || kind == L"tag" || kind == L"tags" ||
                 kind == L"recycle");
            const bool groups_shown = ui::ShowsGroups(gt->view_mode) &&
                                      !(gt->wall && ui::IsZoomIconMode(gt->view_mode));
            if (can_group && groups_shown && !vm.pane.compare_active) toolbar_group = gt->group_by;
        }
        s.renderer.SetToolbarGroup(toolbar_group);
        // A hidden filter or group control still shows while it is in effect.
        s.renderer.SetToolbarView(s.appPrefs.toolbar_show_sort,
                                  s.appPrefs.toolbar_show_filter || !vm.pane.filter_text.empty(),
                                  s.appPrefs.toolbar_show_group || toolbar_group > 0);
    }
    // Toolbar follows the pointer: a file drag over the window swaps the
    // selection chip and the pinned commands for the pinned apps.
    const bool drop_mode = s.toolbarDropMode;
    if (vm.pane.selected_count > 0 && !drop_mode) vm.selection_actions = SelectionChipActions(s);
    s.renderer.SetToolbarSelection(drop_mode ? 0 : vm.pane.selected_count,
                                   static_cast<int>(vm.selection_actions.size()));
    if (!drop_mode) vm.toolbar_pins = ToolbarPinActions(s);
    vm.toolbar_apps = ToolbarAppPins(s);
    vm.toolbar_drop_mode = drop_mode;
    if (drop_mode) {
        vm.toolbar_drop_app = s.dropToolbarApp;
        vm.toolbar_drop_force = s.dropToolbarForce;
        vm.toolbar_drop_pin = s.dropToolbarPin;
        if (!s.toolbarDropProgram.empty()) {
            vm.toolbar_drop_label = l10n::Get(l10n::StringId::ToolbarPinApp);
            const size_t at = vm.toolbar_drop_label.find(L"{app}");
            const size_t slash = s.toolbarDropProgram.find_last_of(L"\\/");
            if (at != std::wstring::npos)
                vm.toolbar_drop_label.replace(at, 5, fs::StripLnkSuffix(
                    slash == std::wstring::npos ? s.toolbarDropProgram : s.toolbarDropProgram.substr(slash + 1)));
        } else if (!vm.toolbar_apps.empty()) {
            vm.toolbar_drop_label = l10n::Get(l10n::StringId::ToolbarDropToOpen);
        }
    }
    s.renderer.SetToolbarPrefs(s.appPrefs.toolbar_new_primary, s.appPrefs.toolbar_last_new == 1,
                               static_cast<int>(vm.toolbar_pins.size() + vm.toolbar_apps.size()));
    s.renderer.SetToolbarApps(static_cast<int>(vm.toolbar_pins.size()), drop_mode, vm.toolbar_drop_label);
    uint64_t separators = app::ToolbarSeparatorMask(s.appPrefs.toolbar_tools);
    // The apps' own separators (bit: before the app after the marked one).
    const size_t app_from = vm.toolbar_pins.size();
    for (size_t k = 0; k + 1 < vm.toolbar_apps.size() && k < s.appPrefs.toolbar_apps.size(); ++k)
        if (app_from + k + 1 < 64 &&
            app::ToolbarAppBreakAfter(s.appPrefs.toolbar_app_breaks, s.appPrefs.toolbar_apps[k]))
            separators |= uint64_t{1} << (app_from + k + 1);
    s.renderer.SetToolbarSeparators(drop_mode ? 0 : separators);
    return vm;
}

// True while the sidebar is collapsed to its icon rail: rows carry no text, so
// every hover needs a name hint. EffectiveSidebarWidth returns pixels (the
// upstream DIP-scaling fix), which is what the rail test expects.
bool SidebarRailActive(const AppState& s) {
    // SidebarRect, not EffectiveSidebarWidth: the hover-peek overlay has text.
    const D2D1_RECT_F sb = s.renderer.SidebarRect(static_cast<float>(s.compositor.Width()),
                                                  static_cast<float>(s.compositor.Height()));
    return ui::SidebarRailLayout(sb.right - sb.left, s.scale);
}

void ApplyHoverTarget(AppState& s, const ui::HitTestResult& hit) {
    s.hoverRegion = static_cast<int>(hit.region);
    s.hoverControlIndex = hit.index;
    s.hoverSubIndex = hit.sub_index;
    s.hoverPath = hit.path;
    s.hoverLabel = hit.label;
    s.hoverSince = GetTickCount64();
    s.tooltipText.clear();
}

std::wstring TooltipForHover(AppState& s) {
    using R = ui::HitTestResult;
    using I = l10n::StringId;
    // Tooltips name keys; show the ones the user set (keymap.h).
    auto text = [](I id) { return app::RemapShortcutText(l10n::Get(id)); };
    switch (static_cast<R::Region>(s.hoverRegion)) {
    case R::RowFolderSize: {
        auto* pane = PaneAtSlot(s, s.hoverPaneIndex);
        auto* tab = pane ? pane->ActiveTab() : ActiveTab(s);
        if (!tab || s.hoverControlIndex < 0 || s.hoverControlIndex >= static_cast<int>(tab->EntryCount())) return L"";
        return DescribeFolderSize(s, EntryFullPath(*tab, s.hoverControlIndex));
    }
    case R::AddressSearch: return text(s.appPrefs.show_hints ? I::TipxSearch : I::Search);
    case R::AddressSearchScope: {
        const auto* tab = ActiveTab(s);
        const bool current = !s.addressSearching && IsAddressSearchResults(tab)
            ? tab->search_input_current : s.addressSearchCurrent;
        return text(current ? I::LocationCurrent : I::LocationIndexed);
    }
    case R::AddressSearchMode: {
        // Say both the current state and what a click will do.
        const auto* tab = ActiveTab(s);
        const bool content = !s.addressEditing && IsAddressSearchResults(tab)
            ? tab->search_input_content : s.addressSearchContent;
        return text(content ? I::TipSearchModeContent : I::TipSearchModeName);
    }
    case R::AddressSearchContent: return text(I::SearchModeContent);
    case R::AddressSearchOptions: return text(I::SearchOptions);
    case R::ContentIndexManage:
    case R::SettingsContentIndex: return text(I::ContentIndexManage);
    case R::NetworkIndexAdd: return text(I::NetworkLiveAdd);
    case R::PaneBannerButton:  // the buttons carry their words; only the close glyph needs one
        return s.hoverControlIndex == static_cast<int>(ui::PaneBannerAction::Dismiss) ? text(I::Close) : L"";
    case R::AddressSearchClear: return text(I::Clear);
    case R::AddressSearchClose: return text(I::Back);
    case R::TabClose: return text(I::TooltipCloseTab);
    case R::Tab: {
        if (!s.pane) return L"";
        int i = s.hoverControlIndex;
        if (s.tabDragging && i >= 0 && i < static_cast<int>(s.tabOrder.size()))
            i = s.tabOrder[static_cast<size_t>(i)];
        if (i < 0 || i >= static_cast<int>(s.window_tabs.items.size()) ||
            !s.window_tabs.items[static_cast<size_t>(i)])
            return L"";
        const auto& layout_tab = *s.window_tabs.items[static_cast<size_t>(i)];
        std::wstring label = app::LayoutTabTitle(layout_tab);
        const auto* folder = layout_tab.ActiveFolder();
        if (folder && !folder->current_path.empty())
            label += L" — " + pulse::path::StripExtendedPathPrefix(folder->current_path);
        return label;
    }
    case R::TabNew: return text(s.appPrefs.show_hints ? I::TipxNewTab : I::TooltipNewTab);
    case R::ThemeToggle: return text(I::TooltipToggleTheme);
    case R::StatusBarAi: return text(I::AiChipTip);   // Pulse 助手  Ctrl+J
    case R::SettingsButton: return text(I::Settings);
    case R::SettingsFind: return text(I::SettingsFind);
    case R::SettingsNav: {
        if (s.hoverControlIndex == 5) return l10n::Get(l10n::StringId::QuickPreview).c_str();
        if (s.hoverControlIndex == 2) return l10n::Get(l10n::StringId::ContextMenu).c_str();  // one entry for both pages
        const I names[]={I::SettingsGeneral,I::SettingsSearchIndex,I::SettingsContextMenu,I::SettingsAboutDiagnostics,I::SettingsDuplicates};
        return s.hoverControlIndex>=0 && s.hoverControlIndex<5 ? text(names[s.hoverControlIndex]) : L"";
    }
    case R::SettingsEffect: {
        if (s.hoverControlIndex >= 0 && s.hoverControlIndex < ui::kWindowEffectCount) {
            static constexpr I effects[] = {
                I::EffectNone, I::EffectAcrylic, I::EffectMica, I::EffectMicaAlt,
            };
            wchar_t label[128]{};
            swprintf_s(label, text(I::WindowEffectFormat).c_str(),
                       text(effects[s.hoverControlIndex]).c_str());
            return label;
        }
        return text(I::SettingsWindowEffect);
    }
    case R::SettingsWallpaper:
        return text(s.hoverControlIndex == 1
            ? I::TooltipClearBackground : I::TooltipChooseBackground);
    case R::SettingsDensity: return text(I::SettingsRowHeight);
    case R::SettingsFolderSort: return text(I::SettingsFolderSort);
    case R::SettingsStartupOpen: return text(I::SettingsStartupOpen);
    case R::SettingsNotifyIcon: return text(I::SettingsNotifyIcon);
    case R::SettingsNewTabOpen: return text(I::SettingsNewTabOpen);
    case R::SettingsExternalOpen: return text(I::SettingsExternalOpen);
    case R::SettingsBlankClick: return text(I::SettingsBlankClickBack);
    case R::SettingsHomeFolder:
        return text(s.hoverControlIndex == 1 ? I::ThisPc : I::SettingsHomeFolderPick);
    case R::SettingsThumbCache: return text(I::ThumbCacheTooltip);
    case R::SettingsTextRender: return text(I::SettingsTextRender);
    case R::SettingsUiFontSize: return text(I::SettingsUiFontSize);
    case R::SettingsTrayIcon: return text(I::SettingsTrayIcon);
    case R::SettingsDoubleCtrlGap: return text(I::GlobalSearchDoubleCtrlGap);
    case R::SettingsWallpaperLook: return text(I::SettingsWallpaperLook);
    case R::SettingsWallpaperBlur: return text(I::SettingsWallpaperBlur);
    case R::SettingsFrameLook: return text(I::SettingsFrameLook);
    case R::SettingsAccent:
        return text(s.hoverControlIndex == 0 ? I::TooltipFollowAccent : I::SettingsThemeColor);
    case R::SettingsToggle:
        if (s.hoverControlIndex == 3) return text(I::SettingsOpenFolders);
        if (s.hoverControlIndex == 4) return text(I::SettingsShowPerformance);
        return L"";
    case R::SettingsIndexExcludeAction:
        if (s.hoverControlIndex == 1) return text(I::SettingsSystemFoldersTip);
        return s.hoverControlIndex == 0 ? text(I::TooltipAddExclusion) : L"";
    case R::SettingsIndexExcludeRemove: return text(I::TooltipRemoveExclusion);
    case R::SettingsDiagnosticsAction: {
        static constexpr I actions[] = {
            I::OpenDiagnostics, I::ClearDiagnostics, I::ExportDiagnostics,
        };
        return s.hoverControlIndex >= 0 && s.hoverControlIndex < 3
            ? text(actions[s.hoverControlIndex]) : L"";
    }
    case R::SettingsUpdateAction:
        return text(s.hoverControlIndex == 1 ? I::DownloadUpdate : I::CheckForUpdates);
    case R::Minimize: return text(I::Minimize);
    case R::Maximize: return text(s.maximized ? I::Restore : I::Maximize);
    case R::Close: return text(I::Close);
    case R::NavBack: return text(I::Back);
    case R::NavForward: return text(I::Forward);
    case R::NavUp: return text(I::Up);
    case R::NavRefresh: return text(I::Refresh);
    case R::NewButton:
        if (const app::Tab* tab = ActiveTab(s); tab && IsConnectServerPage(tab->current_path))
            return text(I::ConnectServerHint);
        return ToolbarNewCreatesFolder(s) ? text(I::NewFolder) + L" (Ctrl+Shift+N)" : text(I::NewTextDocument);
    case R::NewButtonArrow:
        if (const app::Tab* tab = ActiveTab(s); tab && IsConnectServerPage(tab->current_path))
            return text(I::ConnectServerMenu);
        return text(I::New);
    case R::Cut: return text(I::CutShortcut);
    case R::Copy: return text(I::CopyShortcut);
    case R::Paste: return text(I::PasteShortcut);
    case R::Rename: return text(I::RenameShortcut);
    case R::Delete: return text(I::DeleteShortcut);
    case R::SplitButton: return text(s.appPrefs.show_hints ? I::TipxSplit : I::SplitLayout);
    case R::DetailsToggle: return text(s.showDetailsPanel ? I::CollapseDetails : I::ExpandDetails);
    case R::PaneMediumIcons: return text(I::MediumIcons);
    case R::PaneWall: return text(I::PhotoWall);
    case R::PaneWallHint:  // Switch says its word; the close glyph needs one
        return s.hoverControlIndex == 2 ? text(I::PhotoWallSuggestNever) : L"";
    case R::PaneSizeSlider:
    case R::PaneSizeButton: {  // the icon size it is at, e.g. "Large icons · 136"
        const app::Tab* tab = ActiveTab(s);
        if (!tab || !ui::IsZoomIconMode(tab->view_mode)) return L"";
        const I mode = tab->view_mode == ui::ViewMode::ExtraLargeIcons ? I::ViewExtraLarge
            : tab->view_mode == ui::ViewMode::LargeIcons ? I::ViewLarge : I::MediumIcons;
        const float dip = ui::IconDip(tab->view_mode, tab->icon_dip, s.scale);
        if (tab->wall)  // the wall is sized by its row height (prototype: size x 0.78)
            return text(I::PhotoWall) + L" \u00B7 " + text(I::WallRowHeight) + L" " +
                   std::to_wstring(static_cast<int>(dip * 0.78f + 0.5f));
        return text(mode) + L" \u00B7 " + std::to_wstring(static_cast<int>(dip + 0.5f));
    }
    case R::PaneDetails: return text(I::ViewDetails);
    case R::ToolbarSort: return text(I::SortBy);
    case R::ToolbarGroup: return text(I::GroupBy);
    case R::ToolbarGroupClear: return text(I::GroupClear);
    case R::ToolbarMore:   // the customize button (T5); silent while its picker is open
        return ui::ToolbarPickerOpen() ? std::wstring() : text(I::CustomizeToolbar);
    case R::ToolbarPin: {
        const auto pins = ToolbarPinActions(s);
        if (s.hoverControlIndex >= static_cast<int>(pins.size()))
            return ToolbarAppTooltip(s, static_cast<size_t>(s.hoverControlIndex - static_cast<int>(pins.size())));
        return s.hoverControlIndex >= 0 ? pins[static_cast<size_t>(s.hoverControlIndex)].label : std::wstring{};
    }
    case R::FavoritesBarItem: {
        ui::HitTestResult hit;
        hit.region = R::FavoritesBarItem;
        hit.index = s.hoverControlIndex;
        return FavoritesBarTooltip(s, hit);
    }
    case R::FavoritesBarMore: return text(I::FavoritesBarMore);
    case R::SidebarBarSwitch:
        return text(I::FavoritesBarMoveToBar) + L" (" + app::ShownShortcut(L"Ctrl+Shift+B") + L")";
    case R::FavoritesBarDock:
        return text(I::FavoritesBarMoveToSidebar) + L" (" + app::ShownShortcut(L"Ctrl+Shift+B") + L")";
    case R::FavoritesBarChevron: return text(I::FavoritesBarSubfolders);
    case R::AddressFavorites: return text(I::StarredItems);
    case R::ToolbarSelAction: {
        if (s.hoverControlIndex == ui::kSelChipMore) return text(I::MoreActions);
        if (s.hoverControlIndex == ui::kSelChipClear) return text(I::ClearSelectionEsc);
        if (s.hoverControlIndex == ui::kSelChipPin) return text(I::SelChipPinTo);
        if (s.hoverControlIndex == ui::kSelChipLabel) {
            // "2 个文件夹、1 个文件 · 24.6 MB"
            const app::Tab* tab = ActiveTab(s);
            if (!tab || tab->content_results || !tab->snapshot) return L"";
            uint64_t bytes = 0;
            int files = 0, folders = 0;
            tab->SelectionSizeSummary(&bytes, &files, &folders);
            std::wstring tip = text(I::SelChipSummary);
            if (const size_t at = tip.find(L"%s"); at != std::wstring::npos) tip.replace(at, 2, std::to_wstring(folders));
            if (const size_t at = tip.find(L"%s"); at != std::wstring::npos) tip.replace(at, 2, std::to_wstring(files));
            if (files > 0) tip += L" \u00B7 " + pulse::format::ByteSize(bytes, true);
            return tip;
        }
        if (s.hoverControlIndex < 0) return L"";
        const auto actions = SelectionChipActions(s);
        return s.hoverControlIndex < static_cast<int>(actions.size())
            ? actions[static_cast<size_t>(s.hoverControlIndex)].label : std::wstring{};
    }
    case R::PaneColumnLayout: return text(I::ColumnLayout);
    case R::PaneViewButton: return text(I::More);
    case R::FilterBox: return text(s.appPrefs.show_hints ? I::TipxFilter : I::FilterCurrent);
    case R::FilterClear: return text(I::Clear);
    case R::Splitter:
        if (s.hoverControlIndex >= ui::kSplitterSwapIndex)
            return text(LayoutOf(s) == app::LayoutPreset::TwoHorizontal ? I::SplitSwapTopBottom
                                                                         : I::SplitSwapLeftRight);
        // Motion 4b: while dragging, the ratio badge speaks instead.
        if (s.splitterDragging) return {};
        return text(I::ResizeSplit);
    case R::DetailsOpen: return text(I::Open);
    case R::DetailsStar: return text(I::Favorite);
    case R::DetailsMore: return text(I::MoreActions);
    case R::DetailsRename: return text(I::Rename);
    case R::DetailsTagAdd: return text(I::AddTag);
    case R::DetailsResize: return text(I::ResizeDetails);
    case R::DetailsPreviewToggle: return text(s.detailsPreviewOnly ? I::PreviewExpandDetails : I::PreviewCollapseDetails);
    // Names the action, matching the star and rename hints beside it.
    case R::DetailsPreviewEnable:
        return text(s.detailsPreviewEnabled ? I::PreviewHide : I::PreviewShow);
    case R::DetailsPreview: return L"";
    case R::StatusBarTask:
        if (s.hoverSubIndex == 0) return s.ops.UndoLabel();
        if (s.hoverSubIndex == 1) return s.ops.Status().destination_dir;
        return text(I::OpDetails);
    case R::StatusBarCancelSearch: return text(I::ContentCancelSearch);
    case R::DetailsNewTab: return text(I::OpenNewTab);
    case R::DetailsCopyPath: return text(I::CopyPath);
    case R::DetailsSection: return text(I::ExpandCollapse);
    case R::DetailsAttrToggle:
        switch (s.hoverControlIndex) {
        case 0: return text(I::ReadOnly);
        case 1: return text(I::Hidden);
        case 2: return text(I::SystemAttributes);
        default: return L"";
        }
    case R::DetailsSecurityChange: return text(I::SystemAttributes);
    case R::RowStar: return text(s.appPrefs.show_hints ? I::TipxStar : I::Star);
    case R::RowNewTab: return text(s.appPrefs.show_hints ? I::TipxRowNewTab : I::OpenNewTab);
    case R::RowMore: return text(I::MoreActions);
    case R::RowTagDot: {
        // The dot's tag, and what a click does.
        if (s.hoverSubIndex < 0 || s.hoverSubIndex >= static_cast<int>(s.places.tags.size())) return L"";
        wchar_t tip[256]{};
        swprintf_s(tip, l10n::Get(I::TagDotTip).c_str(),
                   s.places.tags[static_cast<size_t>(s.hoverSubIndex)].name.c_str());
        return tip;
    }
    case R::SidebarItemAction:
        return text(IsVerticalTabPath(s.hoverPath) ? I::TooltipCloseTab : I::Unpin);
    case R::WindowIdentity: return text(I::WindowIdentityTip);
    case R::SidebarToggle:
        // The extra line only describes collapsing; an expanded-from-rail state keeps the plain name.
        return text(s.appPrefs.show_hints && !s.appPrefs.sidebar_collapsed ? I::TipxSidebar : I::ToggleSidebar);
    // On the icon rail there is no text to read, so every row (and every folded
    // section's icon) names itself on hover.
    case R::SidebarHeader: return SidebarRailActive(s) ? s.hoverLabel : L"";
    case R::SidebarItem: {
        if (SidebarRailActive(s) && !s.hoverLabel.empty()) return s.hoverLabel;
        if (const app::StarredItem* starred = s.places.FindStarred(s.hoverPath);
            starred && !starred->badge.empty()) {
            return starred->badge;
        }
        if (const app::QuickAccessBadge* badge = s.places.FindQuickAccessBadge(s.hoverPath);
            badge && !badge->badge.empty()) {
            return badge->badge;
        }
        return L"";
    }
    case R::Row: {
        app::Pane* pane = PaneAtSlot(s, s.hoverPaneIndex);
        app::Tab* tab = pane ? pane->ActiveTab() : ActiveTab(s);
        if (tab && tab->snapshot && s.hoverControlIndex >= 0 &&
            s.hoverControlIndex < static_cast<int>(tab->EntryCount()) &&
            !IsDetailsTreePlaceholder(*tab, s.hoverControlIndex)) {
            const auto& entry = tab->EntryAt(s.hoverControlIndex);
            std::wstring tooltip = entry.name;
            if (!entry.change_type_text.empty()) {
                tooltip += L" · " + entry.change_type_text + L" · " + entry.full_path;
                if (!entry.change_old_path.empty() && entry.change_old_path != entry.full_path)
                    tooltip += L" (" + entry.change_old_path + L" → " + entry.full_path + L")";
                return tooltip;
            }
            std::wstring full = entry.full_path;
            if (full.empty() && !fs::IsVirtualPath(tab->current_path)) {
                full = tab->current_path;
                if (!full.empty() && !full.ends_with(L"\\")) full += L"\\";
                full += entry.name;
            }
            // Narrow panes drop columns; keep their facts reachable on hover.
            if (tab->view_mode == ui::ViewMode::Details) {
                using K = ui::MainRenderer::ColumnKind;
                const uint32_t shown = s.renderer.PaintedColumnMask(std::max(0, s.hoverPaneIndex));
                auto hidden = [&](K kind) { return shown && !(shown & (1u << static_cast<uint32_t>(kind))); };
                std::wstring view_kind;
                app::ParsePulsePath(tab->current_path, &view_kind, nullptr);
                const bool search = view_kind == L"search";
                if (search && hidden(K::Path) && !full.empty()) {
                    const size_t slash = full.find_last_of(L'\\');
                    if (slash != std::wstring::npos && slash > 0)
                        tooltip += L" · " + full.substr(0, slash);
                }
                if (hidden(K::Date) && (entry.mtime.dwLowDateTime || entry.mtime.dwHighDateTime))
                    tooltip += L" · " + pulse::l10n::Get(pulse::l10n::StringId::ColumnModified) + L" " +
                               pulse::format::LocalFileTime(entry.mtime);
            }
            if (const auto* indices = s.places.TagIndicesForPath(full); indices && !indices->empty()) {
                tooltip += pulse::l10n::Get(pulse::l10n::StringId::TooltipTags).c_str();
                bool first = true;
                for (int index : *indices) {
                    if (index < 0 || index >= static_cast<int>(s.places.tags.size())) continue;
                    if (!first) tooltip += L"、";
                    tooltip += s.places.tags[static_cast<size_t>(index)].name;
                    first = false;
                }
            }
            if (const app::StarredItem* starred = s.places.FindStarred(full);
                starred && !starred->badge.empty()) {
                tooltip += pulse::l10n::Get(pulse::l10n::StringId::TooltipBadge).c_str() + starred->badge;
            }
            if (tab->compare_marks &&
                static_cast<size_t>(s.hoverControlIndex) < tab->compare_marks->size()) {
                using CM = ui::CompareMark;
                using I = pulse::l10n::StringId;
                switch (static_cast<CM>((*tab->compare_marks)[static_cast<size_t>(s.hoverControlIndex)])) {
                case CM::OnlyHere: tooltip += L" \xB7 " + pulse::l10n::Get(I::CompareOnlyHere); break;
                case CM::Newer: tooltip += L" \xB7 " + pulse::l10n::Get(I::CompareNewer); break;
                case CM::Older: tooltip += L" \xB7 " + pulse::l10n::Get(I::CompareOlder); break;
                case CM::Differs: tooltip += L" \xB7 " + pulse::l10n::Get(I::CompareDiffers); break;
                default: break;
                }
            }
            // Only the name and it is fully visible: a tooltip would just repeat
            // it (B站 #15). Shortened names and extra facts still show.
            if (tooltip.size() == entry.name.size() &&
                !s.renderer.HoveredNameTruncated(std::max(0, s.hoverPaneIndex), s.hoverControlIndex))
                return L"";
            return tooltip;
        }
        return L"";
    }
    case R::TrayCompare:
        return text(s.hoverControlIndex == 0   ? I::HintTrayCompare
                    : s.hoverControlIndex == 2 ? I::TrayCompareTwo
                                               : I::HintTrayCmpCheck);
    case R::TrayStale:
        return text(s.hoverControlIndex == 0 ? I::HintTrayStaleFind : I::HintTrayStaleRemove);
    case R::TrayShelf: return s.hoverLabel;  // the shelf hit test carries each control's tip
    case R::StagedBadge: return text(I::TrayStagedTip);
    case R::TrayDest: {
        const bool gone = GetFileAttributesW(s.hoverPath.c_str()) == INVALID_FILE_ATTRIBUTES;
        return s.hoverPath + L"\n" + text(gone ? I::TrayDestMissing : I::HintTrayDest);
    }
    case R::TrayCard: {
        const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(TrayStackTop(s)),
                                             static_cast<size_t>(TrayDeckCap(s)));
        if (s.hoverControlIndex >= 0 &&
            s.hoverControlIndex < static_cast<int>(entries.size())) {
            std::wstring tip =
                TrayDisplayPath(entries[static_cast<size_t>(s.hoverControlIndex)].item->path);
            if (entries.size() > 1 || TrayItemTotalCount(s.tray) > 1)
                tip += L"\n" + pulse::l10n::Get(pulse::l10n::StringId::TrayFlingHint);
            return tip;
        }
        return L"";
    }
    case R::TrayIntent: return pulse::l10n::Get(pulse::l10n::StringId::TipTrayIntent);
    case R::TrayRelease:
        return pulse::l10n::Get(!s.tray.batches().empty() && s.tray.batches().back().move_intent
            ? pulse::l10n::StringId::TipTrayReleaseMove
            : pulse::l10n::StringId::TipTrayReleaseCopy);
    case R::TrayPrev: return pulse::l10n::Get(pulse::l10n::StringId::TrayPrev);
    case R::TrayNext: return pulse::l10n::Get(pulse::l10n::StringId::TrayNext);
    default: return L"";
    }
}

// Full path of a directory entry (normalized, empty when out of range).
std::wstring EntryFullPath(const app::Tab& tab, int index) {
    if (!tab.snapshot || index < 0 || index >= static_cast<int>(tab.EntryCount())) return L"";
    const fs::DirEntry& e = tab.EntryAt(static_cast<size_t>(index));
    if (e.change_record_only) return L"";
    if (!e.full_path.empty()) return e.full_path;
    if (fs::IsVirtualPath(tab.current_path)) return L"";
    std::wstring full = tab.current_path;
    if (!full.ends_with(L"\\")) full += L"\\";
    full += e.name;
    return full;
}

std::vector<std::wstring> SelectedFullPaths(const app::Tab& tab) {
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

bool IsDeviceTab(const app::Tab* tab) {
    return tab && fs::IsDevicePath(tab->current_path);
}

std::vector<std::wstring> SelectedShellIds(const app::Tab& tab) {
    std::vector<std::wstring> out;
    if (!tab.snapshot) return out;
    const auto indices = tab.SelectedIndices();
    out.reserve(indices.size());
    for (int index : indices) {
        if (index < 0 || index >= static_cast<int>(tab.EntryCount())) continue;
        const std::wstring& id = tab.EntryAt(static_cast<size_t>(index)).shell_id;
        if (id.empty()) return {};
        out.push_back(id);
    }
    return out;
}

bool RunDeviceVerb(AppState& s, std::vector<std::wstring> shell_ids, const wchar_t* verb, bool background) {
    if (shell_ids.empty()) return false;
    return s.ops.RunShellVerb(std::move(shell_ids), verb ? verb : L"", background, s.hwnd);
}

bool SubmitDeviceTransfer(AppState& s, std::vector<std::wstring> sources, const std::wstring& dest,
                          bool move, uint64_t* task) {
    if (task) *task = 0;
    if (sources.empty() || dest.empty()) return false;
    ops::OpRequest req;
    req.type = move ? ops::OpType::Move : ops::OpType::Copy;
    if (fs::IsDevicePath(dest)) {
        auto& devices = app::DeviceRegistry::Instance();
        if (!devices.FolderWritable(dest)) return false;
        req.dest_dir = devices.FolderShellId(dest);
    } else if (!fs::IsVirtualPath(dest)) {
        req.dest_dir = ClipboardPath(dest);
    }
    if (req.dest_dir.empty()) return false;
    req.sources.reserve(sources.size());
    for (auto& source : sources)
        req.sources.push_back(ops::IsShellItemId(source) ? std::move(source) : ClipboardPath(source));
    if (!ops::IsShellTransferRequest(req)) return false;
    // Both ends re-list: the target, and the phone folder a move empties.
    req.refresh_directories.push_back(dest);
    if (const app::Tab* tab = ActiveTab(s); IsDeviceTab(tab) && _wcsicmp(tab->current_path.c_str(), dest.c_str()) != 0)
        req.refresh_directories.push_back(tab->current_path);
    const uint64_t id = s.ops.Submit(std::move(req));
    if (task) *task = id;
    return id != 0;
}

std::wstring DeviceFriendlyPath(const std::wstring& path) {
    const auto where = fs::ParseDevicePath(path);
    if (!where) return path;
    std::wstring name;
    if (!app::DeviceRegistry::Instance().Describe(where->device, &name, nullptr) || name.empty())
        name = where->device;
    std::wstring out = std::wstring(l10n::Get(l10n::StringId::ThisPc).c_str()) + L"\\" + name;
    if (!where->storage.empty()) out += L"\\" + where->storage;
    for (const auto& part : where->parts) out += L"\\" + part;
    return out;
}

std::wstring TagDiscoveryKey(std::wstring path) {
    path = fs::NormalizePath(std::move(path));
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path;
}

void ApplyTagAdsDiscoveries(AppState& s, const std::vector<TagAdsDiscovery>& discoveries) {
    const uint64_t before = s.places.TagRevision();
    for (const auto& discovery : discoveries) {
        const std::wstring key = TagDiscoveryKey(discovery.path);
        s.tagAdsDiscoveryQueued.erase(key);
        s.tagAdsDiscoveryChecked.insert(key);
        s.places.MergeAdsRecords(discovery.path, discovery.records, discovery.legacy_names);
    }
    if (s.places.TagRevision() != before && s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void QueueVisibleTagDiscovery(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->loading || !tab->snapshot || tab->EntryCount() == 0) return;

    ui::PaneViewModel pane;
    app::FillPaneViewModel(pane, *s.pane, &s.places);
    const auto bounds = FocusedPaneRect(s);
    const auto list = s.renderer.PaneListRect(pane, bounds);
    if (list.right <= list.left || list.bottom <= list.top) return;
    const auto [first, last] = s.renderer.VisibleRangeInPane(pane, bounds);
    if (first < 0 || last < first) return;
    std::vector<std::wstring> paths;
    const int end = std::min(last, static_cast<int>(pane.EntryCount()) - 1);
    for (int view_row = first; view_row <= end; ++view_row) {
        const auto item = s.renderer.ItemRectInPane(pane, bounds, view_row);
        if (item.right <= list.left || item.left >= list.right || item.bottom <= list.top ||
            item.top >= list.bottom || item.right <= item.left || item.bottom <= item.top) continue;
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

    const HWND notify = ServiceWindow(s);
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
std::wstring SelectedFullPath(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab) return L"";
    return EntryFullPath(*tab, tab->selected_index);
}

void SyncSavedSearchSidebar(AppState& s) {
    s.sidebar.saved_searches.clear();
    const auto& searches = s.savedSearches.items();
    s.sidebar.saved_searches.reserve(searches.size());
    for (size_t i = 0; i < searches.size(); ++i) {
        app::SidebarEntry entry;
        entry.label = searches[i].name;
        entry.detail = searches[i].root;
        entry.glyph = searches[i].mode == app::SavedSearchMode::Duplicates
            ? L"\xE8EF" : L"\xE721";
        entry.fallback = searches[i].mode == app::SavedSearchMode::Duplicates
            ? L"Dup" : L"Find";
        entry.color = ui::HexColor(searches[i].mode == app::SavedSearchMode::Content
            ? 0x0EA5E9 : searches[i].mode == app::SavedSearchMode::Duplicates
                ? 0xF59E0B : 0x22C55E);
        if (searches[i].mode == app::SavedSearchMode::Tags) {
            // A smart view: a saved tag combination, marked with the tag glyph.
            entry.detail.clear();
            entry.glyph = L"\xE8EC";
            entry.fallback = L"Tag";
            entry.color = ui::HexColor(0xA855F7);
        }
        entry.path = L"pulse:saved-search:" + std::to_wstring(i);
        s.sidebar.saved_searches.push_back(std::move(entry));
    }
}

// ---------------------------------------------------------------------------
// Phone folder watch (UpdateDeviceFolderWatch). UI thread only, but for the
// worker that parses the folder's PIDL and posts it back.
// ---------------------------------------------------------------------------
namespace {
struct DeviceFolderWatch {
    std::wstring path;   // the pulse:device: folder watched (or being parsed)
    ULONG id = 0;        // SHChangeNotifyRegister registration, 0 = none yet
    uint64_t ticket = 0; // matches the worker's answer to this path
};
std::unordered_map<HWND, DeviceFolderWatch> g_device_folder_watches;
uint64_t g_device_folder_ticket = 0;
// A copy of many files raises one notification per file: one re-list after
// the burst instead.
constexpr UINT_PTR kTimerDeviceWatch = 0x4457;  // 'DW'
constexpr UINT kDeviceWatchSettleMs = 400;
}  // namespace

void UpdateDeviceFolderWatch(AppState& s) {
    if (!s.hwnd) return;
    const app::Tab* tab = ActiveTab(s);
    const std::wstring wanted = IsDeviceTab(tab) ? tab->current_path : std::wstring();
    const auto found = g_device_folder_watches.find(s.hwnd);
    if (found != g_device_folder_watches.end() && _wcsicmp(found->second.path.c_str(), wanted.c_str()) == 0) return;
    ReleaseDeviceFolderWatch(s.hwnd);
    if (wanted.empty()) return;
    const std::wstring folder_id = app::DeviceRegistry::Instance().FolderShellId(wanted);
    if (folder_id.empty()) return;
    DeviceFolderWatch& watch = g_device_folder_watches[s.hwnd];
    watch.path = wanted;
    watch.ticket = ++g_device_folder_ticket;
    const HWND hwnd = s.hwnd;
    const uint64_t ticket = watch.ticket;
    std::thread([hwnd, ticket, folder_id] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(shell_id::ParseId(folder_id, &pidl)) && pidl &&
            !PostMessageW(hwnd, WM_DEVICE_WATCH_READY, static_cast<WPARAM>(ticket), reinterpret_cast<LPARAM>(pidl)))
            CoTaskMemFree(pidl);
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void OnDeviceWatchReady(AppState& s, WPARAM ticket, LPARAM pidl_param) {
    auto pidl = reinterpret_cast<PIDLIST_ABSOLUTE>(pidl_param);
    const auto found = g_device_folder_watches.find(s.hwnd);
    if (found != g_device_folder_watches.end() && found->second.ticket == static_cast<uint64_t>(ticket) &&
        !found->second.id && pidl) {
        SHChangeNotifyEntry entry{pidl, FALSE};
        found->second.id = SHChangeNotifyRegister(s.hwnd, SHCNRF_ShellLevel | SHCNRF_NewDelivery,
            SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER |
                SHCNE_UPDATEDIR | SHCNE_UPDATEITEM,
            WM_DEVICE_FOLDER_CHANGED, 1, &entry);
    }
    if (pidl) CoTaskMemFree(pidl);  // the registration keeps its own copy
}

void OnDeviceFolderChanged(AppState& s, WPARAM wparam, LPARAM lparam) {
    PIDLIST_ABSOLUTE* pidls = nullptr;
    LONG event = 0;
    if (HANDLE lock = SHChangeNotification_Lock(reinterpret_cast<HANDLE>(wparam), static_cast<DWORD>(lparam),
                                                &pidls, &event))
        SHChangeNotification_Unlock(lock);
    if (g_device_folder_watches.contains(s.hwnd)) SetTimer(s.hwnd, kTimerDeviceWatch, kDeviceWatchSettleMs, nullptr);
}

void FireDeviceFolderWatch(AppState& s) {
    KillTimer(s.hwnd, kTimerDeviceWatch);
    const auto found = g_device_folder_watches.find(s.hwnd);
    if (found == g_device_folder_watches.end() || found->second.path.empty()) return;
    s.store.MarkDirty(found->second.path);
    const app::Tab* tab = ActiveTab(s);
    if (tab && _wcsicmp(tab->current_path.c_str(), found->second.path.c_str()) == 0)
        RefreshActiveTab(s, RefreshReason::ShellNotification);
}

void ReleaseDeviceFolderWatch(HWND hwnd) {
    const auto found = g_device_folder_watches.find(hwnd);
    if (found == g_device_folder_watches.end()) return;
    if (found->second.id) SHChangeNotifyDeregister(found->second.id);
    g_device_folder_watches.erase(found);
    KillTimer(hwnd, kTimerDeviceWatch);
}

} // namespace pulse
