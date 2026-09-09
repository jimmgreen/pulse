#include "duplicate_scan.h"
#include "app_internal.h"
#include "../fs/fs_enum.h"
#include "../common/localization.h"

#include <thread>

namespace pulse {

void PersistDuplicateScanPrefs(AppState& s) {
    s.appPrefs.duplicate_scan_scope = static_cast<int>(s.duplicateScan.scope);
    s.appPrefs.duplicate_scan_folder = s.duplicateScan.folder_path;
    s.appPrefs.duplicate_scan_drive = s.duplicateScan.drive_root;
    s.appPrefs.Save();
}

void RequestDuplicateVolumeCache(AppState& s, bool force) {
    if (!s.hwnd || s.dup_volume_cache_pending) return;
    if (!force && s.dup_volume_cache_tick != 0 &&
        GetTickCount64() - s.dup_volume_cache_tick <= 2000)
        return;
    s.dup_volume_cache_pending = true;
    HWND hwnd = s.hwnd;
    std::thread([hwnd] {
        index::IndexConfig config;
        auto* payload = new std::vector<index::VolumeInfo>(index::EnumerateLocalVolumes(config));
        if (!PostMessageW(hwnd, WM_DUP_VOLUMES, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    }).detach();
}

void ApplyDuplicateVolumeCache(AppState& s, std::vector<index::VolumeInfo> volumes) {
    s.dup_volume_cache = std::move(volumes);
    s.dup_volume_cache_tick = GetTickCount64();
    s.dup_volume_cache_pending = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void StartDuplicateScan(AppState& s) {
    auto& session = s.duplicateScan;
    if (session.scope == app::DuplicateScanScope::AllFixed && s.dup_volume_cache.empty()) {
        RequestDuplicateVolumeCache(s, true);
        return;
    }
    const auto& volumes = s.dup_volume_cache;
    if (session.scope == app::DuplicateScanScope::Folder && session.folder_path.empty()) {
        std::wstring path;
        if (!PickFolder(s.hwnd, path, l10n::Get(l10n::StringId::DupFolderPlaceholder).c_str()))
            return;
        session.folder_path = std::move(path);
    }
    if (session.scope == app::DuplicateScanScope::Drive && session.drive_root.empty()) {
        for (const auto& volume : volumes) {
            if (volume.mount_point.empty()) continue;
            session.drive_root = app::DuplicateScanSession::NormalizeDriveRoot(volume.mount_point);
            break;
        }
    }
    auto roots = app::DuplicateScanSession::ResolveRoots(
        session.scope, session.folder_path, session.drive_root, volumes);
    if (roots.empty()) return;
    PersistDuplicateScanPrefs(s);
    session.ResetResults();
    session.generation += 1;
    session.scanning = true;
    index::ContentSearchRequest request;
    request.generation = session.generation;
    request.mode = index::ContentSearchMode::Duplicates;
    request.roots = roots;
    request.root = roots.front();
    request.recursive = true;
    request.skip_system_locations = session.scope != app::DuplicateScanScope::Folder;
    request.minimum_file_bytes = session.minimum_file_bytes;
    s.duplicateSearch.SearchAsync(std::move(request));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void CancelDuplicateScan(AppState& s) {
    s.duplicateSearch.Cancel();
    s.duplicateScan.scanning = false;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void RecycleDuplicateGroup(AppState& s, size_t group) {
    auto paths = s.duplicateScan.FilesToDelete(group);
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RecycleDelete;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

void RecycleAllDuplicateExtras(AppState& s) {
    auto paths = s.duplicateScan.AllFilesToDelete();
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RecycleDelete;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

void OpenDuplicateLocation(AppState& s, size_t group, size_t file) {
    if (group >= s.duplicateScan.groups.size()) return;
    const auto& files = s.duplicateScan.groups[group].files;
    if (file >= files.size()) return;
    const std::wstring parent = fs::ParentPath(files[file].path);
    if (!parent.empty()) NavigateTo(s, parent);
}

} // namespace pulse
