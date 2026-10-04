// app_runtime.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "vertical_tabs.h"
#include "app_column_view.h"
#include "folder_sizes_ui.h"
#include "update_status.h"
#include "about_info.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/link_type_text.h"
#include "../ui/fluent_menu.h"
#include "../ui/preview_format_catalog.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/display_path.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "text_diff.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "global_search_controller.h"
#include "jump_list.h"
#include "duplicate_scan.h"
#include "search_query.h"
#include "resource.h"
#include "pulse_version.h"
#include "tray_reveal.h"
#include "default_file_manager.h"
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
    }
    return paths;
}

void SyncVisibleWatches(AppState& s) {
    s.watches.Sync(VisibleFolderPaths(s), [&s](const std::wstring& path, bool overflow,
                                              std::vector<fs::DirNotifyEvent> events) {
        std::lock_guard<std::mutex> lock(s.notify_mu);
        s.notify_queue.push_back({path, overflow, std::move(events)});
    });
}

void BindCurrentLayout(AppState& s) {
    app::LayoutTab* tab = s.window_tabs.Active();
    if (!tab || tab->panes.empty()) {
        s.pane = nullptr;
        s.targetPane = nullptr;
        SyncVisibleWatches(s);
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
}
std::wstring ResolveOpenFolderPath(std::wstring path) {
    while (!path.empty() && (path.front() == L'"' || path.back() == L'"')) {
        if (path.front() == L'"') path.erase(path.begin());
        if (!path.empty() && path.back() == L'"') path.pop_back();
    }
    if (path.empty()) return {};
    const DWORD attrs = GetFileAttributesW(path.c_str());
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
    const DWORD attrs = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
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
        s.tray_controller.RestoreWindow();
        return;
    }
    // Handed over tabs and shell forwards can name a virtual view ("pulse:recent",
    // a tag, a saved search): those open as they are.
    const std::wstring path = fs::IsVirtualPath(raw) ? raw : ResolveOpenFolderPath(raw);
    // Back from the tray with "open the default location" at startup: an
    // explicit folder starts over on its own, as launching with it would.
    if (!path.empty() && !IsWindowVisible(s.hwnd) && TakeFreshStart(s)) StartFreshAt(s, path);
    s.tray_controller.RestoreWindow();
    if (!path.empty() && !ActivateExistingFolderTab(s, path)) NewTab(s, path);
    else InvalidateRect(s.hwnd, nullptr, FALSE);
    // A file (Pulse as a file's default app, a launcher's "open containing
    // folder") shows up selected instead of silently opening its folder (#40).
    if (!path.empty()) SelectLaunchedFile(s, raw);
}

void AdoptSingletonOwnership(AppState& s) {
    if (!s.secondaryInstance) return;
    if (!s.adoptPending) {
        s.adoptPending = true;
        s.adoptDeadline = GetTickCount64() + 3000;
    }
    const auto result = s.single_instance.Acquire();
    if (result != app::SingleInstanceCoordinator::AcquireResult::Primary) {
        // The window that is closing still holds the mutex, or a freshly started
        // window claimed it. Keep trying for a moment, then leave it alone: it
        // belongs to whoever owns it, not to us.
        if (GetTickCount64() >= s.adoptDeadline) s.adoptPending = false;
        return;
    }
    s.adoptPending = false;
    // From here on this window behaves like any other first window: it saves the
    // session, keeps the tray icon and answers the global hotkey.
    s.secondaryInstance = false;
    ApplyGlobalSearchSettings(s);
    s.tray_controller.SetVisible(s.appPrefs.keep_running_on_close ||
                                 s.appPrefs.global_search_enabled);
    app::RefreshJumpList(s.places.quick_access_paths, s.appPrefs.multi_instance_mode);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void PostWorkerResult(AppState& s, app::WorkResult res) {
    {
        std::lock_guard<std::mutex> lock(s.resultMutex);
        s.results.push(std::move(res));
    }
    PostMessageW(s.hwnd, WM_WORKER_RESULT, 0, 0);
}
D2D1_RECT_F FocusedPaneRect(const AppState& s) {
    D2D1_RECT_F content = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    if (!Root(s) || !s.pane) return content;
    std::vector<std::pair<app::Pane*, D2D1_RECT_F>> laid;
    app::LayoutSplitTree(*Root(s), content, 4.0f * s.scale, laid);
    for (const auto& item : laid) {
        if (item.first == s.pane) return item.second;
    }
    return laid.empty() ? content : laid.front().second;
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
    if (tab && !tab->banner_message.empty()) extra = 36.0f * s.scale;
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
void FillPaneSlots(AppState& s, ui::WindowViewModel& vm) {
    D2D1_RECT_F content = s.renderer.ContentRect(
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    std::vector<std::pair<app::Pane*, D2D1_RECT_F>> laid;
    std::vector<app::SplitterLayout> splitters;
    const float gap = 8.0f * s.scale;
    if (Root(s)) app::LayoutSplitTree(*Root(s), content, gap, laid, &splitters);
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
            vm.settings_global_search_enabled = s.appPrefs.global_search_enabled;
            vm.settings_global_search_capturing = s.settings.global_search_hotkey_capturing();
            vm.settings_global_search_hotkey = s.settings.GlobalSearchHotkeyText();
            vm.settings_global_search_error = s.settings.global_search_error();
            vm.settings_content_status = ContentIndexStatusText(s);
            vm.settings_expanded = s.settingsExpanded;
            if (vm.settings_page == 0) vm.settings_preview_codecs = ui::DetectPreviewCodecs(false, s.hwnd);
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
            vm.settings_explorer_windows = s.appPrefs.take_over_explorer_windows;
            vm.settings_default_manager =
                static_cast<int>(app::DefaultFileManagerState(s.appPrefs));
            vm.settings_default_manager_desc = app::DefaultFileManagerSummary(s.appPrefs);
            vm.settings_integration_enabled = s.appPrefs.integration_enabled;
            vm.settings_integration_folders = s.appPrefs.integration_folders;
            vm.settings_integration_win_e = s.appPrefs.integration_win_e;
            vm.settings_integration_this_pc = s.appPrefs.integration_this_pc;
            vm.settings_integration_experimental = s.appPrefs.take_over_explorer_windows;
            vm.settings_integration_state = s.settings.IntegrationState();
            vm.settings_integration_summary = s.settings.IntegrationSummary();
            vm.settings_integration_can_retry = s.settings.IntegrationCanRetry() ||
                vm.settings_integration_state == 2;
            vm.settings_integration_can_restore = s.settings.IntegrationCanRestore();
            vm.settings_shell_tags = s.appPrefs.shell_tag_menu;
            vm.settings_blank_click_action = s.appPrefs.blank_click_action;
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
                row.on = s.ctxMenuPrefs.ItemEnabled(seen.key, seen.category, seen.from_com);
                vm.settings_items.push_back(std::move(row));
            }
            // Pulse's own commands: card 5, after the seen catalog (see
            // SettingsController::ToggleUi).
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
                FillChangePane(s, *tab, slot.pane, slot.rect);
                for (const auto& cutPath : s.cutPaths) {
                    if (fs::ParentPath(cutPath) != tab->current_path) continue;
                    const size_t slash = cutPath.find_last_of(L"\\/");
                    slot.pane.cut_names.insert(slash == std::wstring::npos
                        ? cutPath : cutPath.substr(slash + 1));
                }
            }
            if (static_cast<int>(i) == s.hoverPaneIndex) slot.pane.hover_index = s.hoverRow;
            if (s.stripResizing && static_cast<int>(i) == s.stripResizePane)
                slot.pane.column_strip.resize_column = s.stripResizeColumn;
            if (s.stripHScrolling && static_cast<int>(i) == s.stripResizePane)
                slot.pane.column_strip.hscroll_pressed = true;
            if (static_cast<int>(i) == s.dropPaneIndex) {
                slot.pane.drop_target_index = s.dropRow;
                slot.pane.header_drop = s.dropHeader;
            }
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
// stack can be flipped through endlessly.
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
    for (size_t i = 0; i < std::min(cap, n); ++i) out.push_back(all[(start + i) % n]);
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
    const HWND hwnd = s.hwnd;
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
    const ULONGLONG now = GetTickCount64();
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
    if (GetTickCount64() - drag.last_t > 80) vx = 0.0f; // stopped before letting go
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
        StartTrayMotion(a, AppState::TrayMotion::Spring, GetTickCount64());
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
    const ULONGLONG now = GetTickCount64();
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

// Per-frame stack animation on the 16 ms UI timer. Returns true while
// anything is still moving (caller invalidates).
bool TickTrayDeck(AppState& s) {
    using M = AppState::TrayMotion;
    using X = AppState::TrayExit;
    bool dirty = false;
    const ULONGLONG now = GetTickCount64();
    const float elapsed = s.trayLastTick ? static_cast<float>(now - s.trayLastTick) : 16.0f;
    s.trayLastTick = now;
    auto ease = [&dirty, elapsed](float& cur, float target, float k) {
        const float timed_k = 1.0f - std::pow(1.0f - k, std::min(elapsed, 100.0f) / 16.0f);
        const float next = cur + (target - cur) * timed_k;
        if (std::abs(next - cur) > 0.0015f) { cur = next; dirty = true; }
        else if (cur != target) { cur = target; dirty = true; }
    };

    ease(s.trayOpen, s.dropTray ? 1.0f : 0.0f, 0.20f);

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
            if (i == 0 && grew) { a.appear = 0.0f; a.depth = 0.0f; }
            else if (i > 0) a.depth = 3.0f;
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
        if (!tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
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
        if (!tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
            action = StatusHintAction::SearchSubfolders;
        return l10n::Get(I::HintFiltering);
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
    return l10n::Get(I::StatusHintIdle);
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
static void FillTrayCompare(AppState& s, const std::wstring (&paths)[2],
                            ui::TrayCompareView& v) {
    uint64_t t[2]{}, sz[2]{};
    std::wstring dir[2];
    for (int i = 0; i < 2; ++i) {
        const std::wstring shown = ClipboardPath(paths[i]);
        const size_t slash = shown.find_last_of(L"\\/");
        v.name[i] = slash == std::wstring::npos ? shown : shown.substr(slash + 1);
        dir[i] = slash == std::wstring::npos ? std::wstring() : shown.substr(0, slash);
        const size_t up = dir[i].find_last_of(L"\\/");
        v.where[i] = up == std::wstring::npos ? dir[i] : dir[i].substr(up + 1);
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (GetFileAttributesExW(paths[i].c_str(), GetFileExInfoStandard, &d)) {
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

ui::WindowViewModel BuildVm(AppState& s, bool probe_details) {
    if (!s.pane) return {};
    UpdateFolderCompare(s);
    s.changes.visible_paths.clear();
    ForEachPane(s, [&](app::Pane& pane) {
        if (auto* tab = pane.ActiveTab()) {
            tab->SetShowHiddenFiles(s.appPrefs.show_hidden_files);
            tab->SetShowProtectedOsFiles(s.appPrefs.show_protected_os_files);
        }
    });
    SyncColumnStrips(s);
    ui::WindowViewModel vm = app::BuildWindowViewModel(*s.pane, s.sidebar,
        s.pane->focused, s.maximized, s.darkMode, &s.places, s.sidebarCollapsedMask,
        s.sidebarHiddenMask, s.starredExpanded, &s.sidebarOrder,
        s.sidebarQuickAccessHiddenMask);
    app::FillWindowTabStrip(vm, s.window_tabs);
    if (s.groupCardGroupId != 0) {
        // The tab controller can close tabs without going through the AppState
        // wrappers, so re-check the remembered tabs before reading them.
        PruneGroupActivations(s);
        const app::LayoutTab* last_active = nullptr;
        if (auto it = s.lastActiveInGroup.find(s.groupCardGroupId);
            it != s.lastActiveInGroup.end()) {
            last_active = it->second;
        }
        vm.tab_group_card.group_id = s.groupCardGroupId;
        vm.tab_group_card.chip_index = s.groupCardChipIndex;
        vm.tab_group_card.hover_row = s.groupCardHoverRow;
        vm.tab_group_card.rows =
            app::TabGroupCardRows(s.window_tabs, s.groupCardGroupId, last_active);
        vm.tab_group_card.visible = !vm.tab_group_card.rows.empty();
    }
    vm.show_pinned_tab_names = s.appPrefs.show_pinned_tab_names;
    vm.settings_multi_instance_mode = s.appPrefs.multi_instance_mode;
    vm.settings_list_smart_date = s.appPrefs.list_smart_date;
    vm.settings_list_zebra_rows = s.appPrefs.list_zebra_rows;
    vm.settings_list_size_bar = s.appPrefs.list_size_bar;
    vm.settings_list_tag_names = s.appPrefs.list_tag_name_color;
    vm.settings_list_selection_outline = s.appPrefs.list_selection_outline;
    vm.settings_vertical_tabs = s.appPrefs.vertical_tabs;
    vm.settings_show_hints = s.appPrefs.show_hints;
    vm.settings_tips_seen = s.appPrefs.tips_seen != 0;
    vm.settings_folder_sort = s.appPrefs.folder_sort_mode;
    vm.settings_startup_open = s.appPrefs.startup_open;
    vm.settings_notify_icon = s.appPrefs.notify_icon_mode;
    vm.settings_new_tab_open = s.appPrefs.new_tab_open;
    vm.settings_close_last_tab = s.appPrefs.close_window_with_last_tab;
    vm.settings_confirm_delete = s.appPrefs.confirm_recycle_delete;
    vm.settings_home_folder = s.appPrefs.home_folder;
    vm.settings_text_render = s.appPrefs.text_render;
    vm.settings_ui_font_scale = s.appPrefs.ui_font_scale;
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
            if (action != StatusHintAction::None) {
                vm.status.hint_action = static_cast<int>(action);
                vm.status.hint_action_text = StatusHintActionText(action) + L" \u2192";
            }
        }
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
    SyncSearchBarWidth(s);
    vm.address_editing = s.addressEditing;
    vm.address_searching = s.addressSearching;
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
            vm.tag_gap_line_y = s.tagGapVisible ? s.tagGapLineY : 0.0f;
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
        uint64_t total_size = 0;
        for (const auto& b : s.tray.batches()) total_size += b.total_size;
        deck.total_size = total_size;
        deck.live_count = static_cast<int>(entries.size());
        for (const auto& b : s.tray.batches())
            for (const auto& item : b.items)
                if (!item.exists) ++deck.stale_count;
        {
            // Two-file compare: exactly two staged items, both existing files.
            std::wstring pair[2];
            int n = 0;
            bool files = true;
            for (const auto& b : s.tray.batches()) {
                for (const auto& item : b.items) {
                    if (n < 2) pair[n] = item.path;
                    ++n;
                    files = files && item.exists && !item.is_dir;
                }
            }
            deck.can_compare = n == 2 && files;
            deck.comparing = deck.can_compare && s.trayCompare;
            if (deck.comparing) FillTrayCompare(s, pair, deck.compare);
        }
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
        const ULONGLONG now = GetTickCount64();
        for (const auto& p : s.trayPuffs) {
            if (now < p.start) continue;
            ui::TrayPuffView view;
            view.t = std::clamp(static_cast<float>(now - p.start) / 620.0f, 0.0f, 1.0f);
            view.angle = p.angle;
            view.dist = p.dist;
            view.size = p.size;
            deck.puffs.push_back(view);
        }
    }
    // Vertical tabs join the sidebar before its scroll range is measured.
    ApplyVerticalTabs(s, vm);
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
                               pulse::l10n::Pick(L" \u5B57\u8282)", L" bytes)");
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
    vm.hover_pane_index = s.hoverPaneIndex;
    vm.column_resize_pressed = s.columnResizing;
    vm.tooltip_text = s.tooltipText;
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
    FillPaneSlots(s, vm);
    FillChangePopover(s, vm);
    FillFolderSizes(s, vm);
    vm.window_effect = ui::WindowEffectFromId(s.appPrefs.window_effect);
    vm.background_image = s.appPrefs.background_image;
    vm.wallpaper_look = s.appPrefs.wallpaper_look;
    vm.wallpaper_blur = s.appPrefs.wallpaper_blur;
    vm.safe_mode = s.safeMode;
    {
        // Toolbar "Group" button: list-like views of groupable places only
        // (same rule as the Sort menu's group section).
        int toolbar_group = -1;
        if (const app::Tab* gt = ActiveTab(s)) {
            std::wstring kind;
            app::ParsePulsePath(gt->current_path, &kind, nullptr);
            const bool can_group = !gt->content_results &&
                ((!gt->current_path.empty() && !fs::IsVirtualPath(gt->current_path)) || kind == L"recent" ||
                 kind == L"search" || kind == L"saved-search" || kind == L"tag" || kind == L"recycle");
            const bool list_like = gt->view_mode == ui::ViewMode::Details ||
                                   gt->view_mode == ui::ViewMode::Content;
            if (can_group && list_like && !vm.pane.compare_active) toolbar_group = gt->group_by;
        }
        s.renderer.SetToolbarGroup(toolbar_group);
    }
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
    const int previous_index = s.hoverControlIndex;
    const bool was_chip = s.hoverRegion == static_cast<int>(ui::HitTestResult::TabGroup);
    s.hoverRegion = static_cast<int>(hit.region);
    s.hoverControlIndex = hit.index;
    s.hoverSubIndex = hit.sub_index;
    // The group card dwells on one chip: moving to a sibling chip restarts the
    // clock (that chip's card opens), leaving the strip cancels it. Arriving
    // from anywhere but the same chip always restarts it, even when the new
    // index happens to equal the old non-chip index.
    if (hit.region == ui::HitTestResult::TabGroup) {
        if (!was_chip || previous_index != hit.index) s.groupCardSince = GetTickCount64();
    } else if (hit.region != ui::HitTestResult::TabGroupCard &&
               hit.region != ui::HitTestResult::TabGroupCardRow) {
        s.groupCardSince = 0;
    }
    // A tab that is being dragged is on its way out of the strip; a hint pinned
    // to where it used to sit would stay behind in this window.
    if (s.tabDragging || s.tabDragPending) {
        s.hoverPath.clear();
        s.hoverLabel.clear();
        s.tooltipText.clear();
        s.hoverSince = GetTickCount64();
        return;
    }
    s.hoverPath = hit.path;
    s.hoverLabel = hit.label;
    s.hoverSince = GetTickCount64();
    s.tooltipText.clear();
}

void HideTabGroupCard(AppState& s) {
    s.groupCardGroupId = 0;
    s.groupCardChipIndex = -1;
    s.groupCardHoverRow = -1;
    s.groupCardSince = 0;
}

std::wstring TooltipForHover(AppState& s) {
    using R = ui::HitTestResult;
    using I = l10n::StringId;
    auto text = [](I id) -> const std::wstring& { return l10n::Get(id); };
    switch (static_cast<R::Region>(s.hoverRegion)) {
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
    case R::SettingsButton: return text(I::Settings);
    case R::SettingsFind: return text(I::SettingsFind);
    case R::SettingsNav: {
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
    case R::SettingsBlankClick: return text(I::SettingsBlankClickBack);
    case R::SettingsHomeFolder:
        return text(s.hoverControlIndex == 1 ? I::ThisPc : I::SettingsHomeFolderPick);
    case R::SettingsTextRender: return text(I::SettingsTextRender);
    case R::SettingsUiFontSize: return text(I::SettingsUiFontSize);
    case R::SettingsTrayIcon: return text(I::SettingsTrayIcon);
    case R::SettingsWallpaperLook: return text(I::SettingsWallpaperLook);
    case R::SettingsWallpaperBlur: return text(I::SettingsWallpaperBlur);
    case R::SettingsAccent:
        return text(s.hoverControlIndex == 0 ? I::TooltipFollowAccent : I::SettingsThemeColor);
    case R::SettingsToggle:
        if (s.hoverControlIndex == 3) return text(I::SettingsOpenFolders);
        if (s.hoverControlIndex == 4) return text(I::SettingsShowPerformance);
        return L"";
    case R::SettingsIndexExcludeAction: return text(I::TooltipAddExclusion);
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
    case R::NewButton: return text(I::New);
    case R::Cut: return text(I::CutShortcut);
    case R::Copy: return text(I::CopyShortcut);
    case R::Paste: return text(I::PasteShortcut);
    case R::Rename: return text(I::RenameShortcut);
    case R::Delete: return text(I::DeleteShortcut);
    case R::SplitButton: return text(s.appPrefs.show_hints ? I::TipxSplit : I::SplitLayout);
    case R::DetailsToggle: return text(s.showDetailsPanel ? I::CollapseDetails : I::ExpandDetails);
    case R::PaneMediumIcons: return text(I::MediumIcons);
    case R::PaneDetails: return text(I::ViewDetails);
    case R::ToolbarSort: return text(I::SortBy);
    case R::ToolbarGroup: return text(I::GroupBy);
    case R::ToolbarGroupClear: return text(I::GroupClear);
    case R::ToolbarMore: return text(I::More);
    case R::PaneColumnLayout: return text(I::ColumnLayout);
    case R::PaneViewButton: return text(I::More);
    case R::FilterBox: return text(s.appPrefs.show_hints ? I::TipxFilter : I::FilterCurrent);
    case R::FilterClear: return text(I::Clear);
    case R::Splitter: return text(I::ResizeSplit);
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
    case R::StatusBarTask: return text(I::OpDetails);
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
    case R::SidebarItemAction:
        return text(IsVerticalTabPath(s.hoverPath) ? I::TooltipCloseTab : I::Unpin);
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
            s.hoverControlIndex < static_cast<int>(tab->EntryCount())) {
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

std::wstring TagDiscoveryKey(std::wstring path) {
    path = fs::NormalizePath(std::move(path));
    for (auto& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path;
}

void QueueVisibleTagDiscovery(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->loading || !tab->snapshot || tab->EntryCount() == 0) return;

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
        entry.path = L"pulse:saved-search:" + std::to_wstring(i);
        s.sidebar.saved_searches.push_back(std::move(entry));
    }
}

} // namespace pulse
