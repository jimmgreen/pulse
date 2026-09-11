// app_runtime.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "duplicate_scan.h"
#include "resource.h"
#include "pulse_version.h"
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
#include <unordered_set>

using namespace pulse;

namespace {

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
void PrefetchDetailsMeta(HWND hwnd, const std::wstring& path) {
    std::thread([hwnd, path] {
        auto* result = new DetailsMetaResult{};
        result->path = path;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            result->attrs_valid = true;
            result->created = fad.ftCreationTime;
            result->modified = fad.ftLastWriteTime;
            result->accessed = fad.ftLastAccessTime;
        }
        SHFILEINFOW sfi{};
        const std::wstring shell_path = pulse::path::StripExtendedPathPrefix(path);
        if (SHGetFileInfoW(shell_path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME) &&
            sfi.szTypeName[0]) {
            result->type_name = sfi.szTypeName;
        }
        result->meta = app::FetchDetailsMeta(path);
        if (!PostMessageW(hwnd, WM_DETAILS_META, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
    }).detach();
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

void OpenFolderInNewTab(AppState& s, const std::wstring& raw) {
    s.tray_controller.RestoreWindow();
    const std::wstring path = ResolveOpenFolderPath(raw);
    if (!path.empty() && !ActivateExistingFolderTab(s, path)) NewTab(s, path);
    else InvalidateRect(s.hwnd, nullptr, FALSE);
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
    const ui::ViewMode mode = tab ? tab->view_mode : ui::ViewMode::Details;
    pane.top += s.renderer.PaneHeaderHeight() + extra +
                (ui::ShowsColumnHeader(mode) ? s.renderer.ColumnHeaderHeight() : 0.0f);
    return pane;
}
bool ScrollbarGeometry(const AppState& s, const ui::PaneViewModel& pane,
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
            vm.settings_keep_running = s.appPrefs.keep_running_on_close;
            vm.settings_show_hidden_files = s.appPrefs.show_hidden_files;
            vm.settings_open_folders = s.appPrefs.open_folders_in_pulse;
            vm.settings_blank_click_go_back = s.appPrefs.blank_click_go_back;
            vm.settings_row_height = s.appPrefs.row_height;
            vm.settings_tray_icon = s.appPrefs.tray_icon_size;
            vm.settings_language = s.appPrefs.language == L"zh-CN" ? 1
                : s.appPrefs.language == L"en-US" ? 2 : 0;
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
            vm.settings_update_checking = s.update_checker.checking();
            vm.settings_update_downloading = s.update_installer.downloading();
            vm.settings_update_installing = s.update_installer.installing();
            DWORD update_install_error = s.update_install_error;
            if (s.shot.active) {
                vm.settings_update_downloading |= s.shot.update_state == L"downloading";
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
                vm.settings_update_status = l10n::Get(l10n::StringId::UpdateDesc);
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
            vm.settings_index_service = s.index.ServiceMode();
            vm.settings_index_installed = s.settings.service_installed();
            vm.settings_index_status = s.index.Status();
            vm.settings_index_migrating = s.settings.migration_pending();
            if (s.shot.active) {
                wchar_t simulated[2]{};
                if (GetEnvironmentVariableW(L"PULSE_TEST_INDEX_MIGRATING", simulated, 2) == 1 &&
                    simulated[0] == L'1') vm.settings_index_migrating = true;
            }
            if (vm.settings_index_migrating)
                vm.settings_index_status = l10n::Get(l10n::StringId::IndexMigrating);
            vm.settings_index_path = s.index.IndexPath();
            vm.settings_index_error = s.settings.error();
            vm.settings_index_volumes.clear();
            vm.settings_index_excluded_paths = s.index.ExcludedPaths();
            vm.settings_network_roots.clear();
            auto index_volumes = s.index.Volumes();
            if (s.shot.active && vm.settings_page == 1 && index_volumes.empty()) {
                index::IndexConfig defaults;
                index_volumes = index::EnumerateLocalVolumes(defaults);
                vm.settings_index_service = true;
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
                row.state = volume.state;
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
                row.state = root.state;
                row.detail = root.error;
                row.online = root.online;
                row.building = root.building;
                vm.settings_network_roots.push_back(std::move(row));
            }
            if (s.shot.active && vm.settings_page == 1 && vm.settings_network_roots.empty()) {
                ui::NetworkRootRowView row;
                row.path = L"\\\\fileserver\\projects\\设计资料";
                row.state = L"已同步 · 128,420 项";
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
            vm.settings_items.reserve(s.ctxMenuPrefs.seen.size());
            for (const auto& seen : s.ctxMenuPrefs.seen) {
                ui::SettingsRowView row;
                row.key = seen.key;
                row.text = seen.text;
                row.group = static_cast<int>(ipc::GroupOf(seen.category));
                row.on = s.ctxMenuPrefs.ItemEnabled(seen.key, seen.category, seen.from_com);
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
                for (const auto& cutPath : s.cutPaths) {
                    if (fs::ParentPath(cutPath) != tab->current_path) continue;
                    const size_t slash = cutPath.find_last_of(L"\\/");
                    slot.pane.cut_names.insert(slash == std::wstring::npos
                        ? cutPath : cutPath.substr(slash + 1));
                }
            }
            if (static_cast<int>(i) == s.hoverPaneIndex) slot.pane.hover_index = s.hoverRow;
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


std::vector<TrayDeckEntry> TrayDeckEntries(const app::StagingTray& tray, size_t offset,
                                                  size_t cap) {
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

int TrayItemTotalCount(const app::StagingTray& tray) {
    int n = 0;
    for (const auto& b : tray.batches()) n += static_cast<int>(b.items.size());
    return n;
}

// Extended-length prefixes leak into tooltips otherwise: \\?\C:\x -> C:\x.
std::wstring TrayDisplayPath(const std::wstring& path) {
    if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\" + path.substr(7);
    if (path.compare(0, 4, L"\\\\?\\") == 0) return path.substr(4);
    return path;
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
    if (s.hoverRegion == static_cast<int>(ui::HitTestResult::TrayItemRemove)) {
        const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset),
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
        if (!result.empty()) result += pulse::l10n::effective_language() == pulse::l10n::Language::ZhCN ? L"、" : L", ";
        result += value.name;
    }
    return result.empty() ? pulse::l10n::Get(pulse::l10n::StringId::AttrNormal).c_str() : result;
}

// Exponential smoothing toward layout targets; runs on the 16 ms UI timer.
// Returns true while anything is still moving (caller invalidates).
bool TickTrayDeck(AppState& s) {
    bool dirty = false;
    const ULONGLONG now = GetTickCount64();
    const float elapsed = s.trayLastTick ? static_cast<float>(now - s.trayLastTick) : 16.0f;
    s.trayLastTick = now;
    auto ease = [&dirty, elapsed](float& cur, float target, float k) {
        const float timed_k = 1.0f - std::pow(1.0f - k, elapsed / 16.0f);
        const float next = cur + (target - cur) * timed_k;
        if (std::abs(next - cur) > 0.0015f) { cur = next; dirty = true; }
        else if (cur != target) { cur = target; dirty = true; }
    };

    ease(s.trayOpen, s.dropTray ? 1.0f : 0.0f, 0.20f);

    // Window into the newest-first list; a fresh collect always jumps back
    // to the newest items.
    const int total = TrayItemTotalCount(s.tray);
    const int cap = TrayDeckCap(s);
    const int max_offset = std::max(0, total - cap);
    const int clamped_offset = std::clamp(s.trayDeckOffset, 0, max_offset);
    if (clamped_offset != s.trayDeckOffset) { s.trayDeckOffset = clamped_offset; dirty = true; }
    const bool grew = total > static_cast<int>(s.trayDeckLastTotal);
    if (grew && s.trayDeckOffset != 0) { s.trayDeckOffset = 0; dirty = true; }
    s.trayDeckLastTotal = static_cast<size_t>(total);

    const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset),
                                         static_cast<size_t>(cap));
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
            // Collected items slide out from the center; items revealed by
            // wheel-scrolling fade in directly at their slot.
            a.slot = grew ? 0.0f : target_slot;
        }
        if (a.name.empty()) a.name = TrayItemName(item.path);
        a.attrs = item.attrs;
        a.is_dir = item.is_dir;
        a.missing = !item.exists;
        a.ghost = false;
        a.layout_count = n;
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
        if (!a.ghost) {
            a.ghost = true;
            a.exit_started = now;
            a.exit_opacity = a.opacity;
        }
        // A bounded, time-based ease-out avoids a long tail at low frame rates.
        const float progress = std::clamp(static_cast<float>(now - a.exit_started) / 140.0f, 0.0f, 1.0f);
        a.opacity = a.exit_opacity * (1.0f - progress) * (1.0f - progress);
        dirty = true;
        ++ghosts;
        if (progress >= 1.0f || ghosts > 6) { it = s.trayCards.erase(it); dirty = true; }
        else ++it;
    }
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
        return false;
    default:
        return true;
    }
}

static std::wstring ContextStatusHint(const app::Tab* tab) {
    if (!tab) return {};
    if (IsSettingsTab(tab)) return l10n::Get(l10n::StringId::Settings);
    if (IsRecycleTab(tab)) return l10n::Get(l10n::StringId::StatusHintRecycle);
    std::wstring kind;
    app::ParsePulsePath(tab->current_path, &kind, nullptr);
    if (kind == L"search") return l10n::Get(l10n::StringId::StatusHintSearch);
    if (tab->net_readonly) return l10n::Get(l10n::StringId::StatusHintReadonly);
    if (tab->SelectedCount() > 0) return l10n::Get(l10n::StringId::StatusHintSelected);
    return l10n::Get(l10n::StringId::StatusHintIdle);
}

// View-model wrapper: builds the base VM and layers ops-layer status on top.
// probe_details=false: hit-test / input paths must not kick off selection
// probes (or mutate detailsSelPath); paint owns those side effects.
ui::WindowViewModel BuildVm(AppState& s, bool probe_details) {
    if (!s.pane) return {};
    ForEachPane(s, [&](app::Pane& pane) {
        if (auto* tab = pane.ActiveTab()) tab->SetShowHiddenFiles(s.appPrefs.show_hidden_files);
    });
    ui::WindowViewModel vm = app::BuildWindowViewModel(*s.pane, s.sidebar,
        s.pane->focused, s.maximized, s.darkMode, &s.places, s.sidebarCollapsedMask,
        s.starredExpanded);
    app::FillWindowTabStrip(vm, s.window_tabs);
    vm.show_pinned_tab_names = s.appPrefs.show_pinned_tab_names;
    vm.sidebar_scroll = s.sidebarScroll;
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
        if (HoverHintIsCommand(region))
            vm.status.hint_text = TooltipForHover(s);
        if (vm.status.hint_text.empty())
            vm.status.hint_text = ContextStatusHint(ActiveTab(s));
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
        }
        for (size_t gi = 0; gi < vm.tab_groups.size(); ++gi) {
            const auto off = s.chipOffsets.find(vm.tab_groups[gi].id);
            if (off != s.chipOffsets.end()) vm.tab_groups[gi].x_offset = off->second;
        }
    }
    // Staging tray card deck: live cards (newest batch first) + exiting ghosts.
    {
        const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset),
                                             static_cast<size_t>(TrayDeckCap(s)));
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
            card.exit_layout_count = anim.layout_count;
            card.slot = anim.slot;
            card.hover = anim.hover;
            card.appear = anim.appear;
            card.opacity = anim.opacity;
            deck.cards.push_back(std::move(card));
        }
    }
    // The tray (including exiting cards) determines the sidebar viewport.
    // Clamp only after it is populated, using the same model as draw/hit-test.
    s.sidebarScroll = std::clamp(s.sidebarScroll, 0.0f, s.renderer.SidebarMaxScroll(
        vm, static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height())));
    vm.sidebar_scroll = s.sidebarScroll;
    // Right details panel: selection info + size walk + tag chips.
    vm.details_visible = s.showDetailsPanel;
    std::wstring sizeTarget; // folder that should be walking ("" = none)
    if (s.showDetailsPanel) {
        ui::DetailsPanelView& dv = vm.details;
        dv.scroll_y = s.detailsScroll;
        dv.preview_only = s.detailsPreviewOnly;
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
            tab->selected_index < static_cast<int>(tab->snapshot->size())) {
            const fs::DirEntry& e = (*tab->snapshot)[static_cast<size_t>(tab->selected_index)];
            const bool penetrated = !e.link_target.empty();
            dv.name = e.name;
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
            for (int index : tab->SelectedIndices()) {
                if (index < 0 || index >= static_cast<int>(tab->snapshot->size())) continue;
                const fs::DirEntry& entry = (*tab->snapshot)[static_cast<size_t>(index)];
                if (entry.is_dir) ++folders;
                else { ++files; knownSize += entry.size; }
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
            dv.size_text = pulse::format::ByteSize(knownSize, true);
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
    vm.hover_region = s.hoverRegion;
    vm.hover_control_index = s.hoverControlIndex;
    vm.hover_sub_index = s.hoverSubIndex;
    vm.hover_pane_index = s.hoverPaneIndex;
    vm.column_resize_pressed = s.columnResizing;
    vm.tooltip_text = s.tooltipText;
    vm.tooltip_x = static_cast<float>(s.hoverPoint.x);
    vm.tooltip_y = static_cast<float>(s.hoverPoint.y);
    FillPaneSlots(s, vm);
    vm.window_effect = ui::WindowEffectFromId(s.appPrefs.window_effect);
    vm.background_image = s.appPrefs.background_image;
    vm.safe_mode = s.safeMode;
    return vm;
}

std::wstring TooltipForHover(AppState& s) {
    using R = ui::HitTestResult;
    using I = l10n::StringId;
    auto text = [](I id) -> const std::wstring& { return l10n::Get(id); };
    switch (static_cast<R::Region>(s.hoverRegion)) {
    case R::AddressSearch: return text(I::Search);
    case R::AddressSearchScope: {
        const auto* tab = ActiveTab(s);
        const bool current = !s.addressSearching && IsAddressSearchResults(tab)
            ? tab->search_input_current : s.addressSearchCurrent;
        return text(current ? I::LocationCurrent : I::LocationIndexed);
    }
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
    case R::TabNew: return text(I::TooltipNewTab);
    case R::ThemeToggle: return text(I::TooltipToggleTheme);
    case R::SettingsButton: return text(I::Settings);
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
    case R::SettingsTrayIcon: return text(I::SettingsTrayIcon);
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
    case R::SplitButton: return text(I::SplitLayout);
    case R::DetailsToggle: return text(s.showDetailsPanel ? I::CollapseDetails : I::ExpandDetails);
    case R::PaneMediumIcons: return text(I::MediumIcons);
    case R::PaneViewButton: return text(I::View);
    case R::FilterBox: return text(I::FilterCurrent);
    case R::Splitter: return text(I::ResizeSplit);
    case R::DetailsOpen: return text(I::Open);
    case R::DetailsStar: return text(I::Favorite);
    case R::DetailsMore: return text(I::MoreActions);
    case R::DetailsRename: return text(I::Rename);
    case R::DetailsTagAdd: return text(I::AddTag);
    case R::DetailsResize: return text(I::ResizeDetails);
    case R::DetailsPreviewToggle: return text(s.detailsPreviewOnly ? I::PreviewExpandDetails : I::PreviewCollapseDetails);
    case R::DetailsPreview: return L"";
    case R::StatusBarTask: return text(I::OpDetails);
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
    case R::RowStar: return text(I::Star);
    case R::RowNewTab: return text(I::OpenNewTab);
    case R::RowMore: return text(I::MoreActions);
    case R::SidebarItemAction: return text(I::Unpin);
    case R::SidebarItem: {
        if (const app::StarredItem* starred = s.places.FindStarred(s.hoverPath);
            starred && !starred->badge.empty()) {
            return starred->badge;
        }
        return L"";
    }
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
            return tooltip;
        }
        return L"";
    }
    case R::TrayCard: {
        const auto entries = TrayDeckEntries(s.tray, static_cast<size_t>(s.trayDeckOffset),
                                             static_cast<size_t>(TrayDeckCap(s)));
        if (s.hoverControlIndex >= 0 &&
            s.hoverControlIndex < static_cast<int>(entries.size()))
            return TrayDisplayPath(entries[static_cast<size_t>(s.hoverControlIndex)].item->path);
        return L"";
    }
    default: return L"";
    }
}

// Full path of a directory entry (normalized, empty when out of range).
std::wstring EntryFullPath(const app::Tab& tab, int index) {
    if (!tab.snapshot || index < 0 || index >= static_cast<int>(tab.snapshot->size())) return L"";
    const fs::DirEntry& e = (*tab.snapshot)[static_cast<size_t>(index)];
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
