// desktop_mount.h - Zero-injection desktop hierarchy prober, read-only Shell COM icon snapshot,
// desktop directory watcher, and the top-level layer window kept above Explorer's desktop.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "desktop_takeover_record.h"
#include "host/model/desktop_model.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse::desktop::mount {

enum class DesktopHierarchyMode : uint8_t {
    NotFound = 0,
    ClassicWorkerW = 1,
    Win11_24H2_ProgmanChild = 2,
    FallbackProgman = 3,
};

const char* HierarchyModeName(DesktopHierarchyMode mode) noexcept;

struct DesktopHierarchyInfo {
    DesktopHierarchyMode mode = DesktopHierarchyMode::NotFound;
    HWND progman = nullptr;
    HWND defview_parent = nullptr;
    HWND shelldll_defview = nullptr;
    HWND sys_listview32 = nullptr;
    HWND workerw_wallpaper = nullptr;
    DWORD explorer_pid = 0;
    bool listview_visible = false;
    model::RectI virtual_bounds{};
};

struct NativeIconEntry {
    std::wstring parsing_name;
    std::wstring relative_name;
    std::wstring display_name;
    model::PointI position{0, 0};
};

struct NativeIconSnapshot {
    int64_t captured_unix_ms = 0;
    uint32_t view_mode = 0;
    int32_t icon_size = 48;
    std::vector<model::MonitorWorkArea> monitors;
    std::vector<NativeIconEntry> items;
};

// Strictly read-only: detects Progman / WorkerW / SHELLDLL_DefView / SysListView32 without mutating state.
DesktopHierarchyInfo ProbeDesktopHierarchy() noexcept;
std::vector<model::MonitorWorkArea> QueryMonitorWorkAreas();
std::wstring DefaultSnapshotFilePath();
std::wstring DefaultLayoutStateFilePath();

// Captures and restores Explorer desktop icon coordinates via CLSID_ShellWindows -> IFolderView2.
bool CaptureNativeIconSnapshot(NativeIconSnapshot& out_snapshot, HRESULT* out_hr = nullptr);
bool RestoreNativeIconSnapshot(const NativeIconSnapshot& snapshot, size_t* out_restored_count = nullptr,
                               HRESULT* out_hr = nullptr);

// Atomic JSON persistence (.tmp + FlushFileBuffers + MoveFileExW MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH).
bool SaveIconSnapshotFile(const std::wstring& file_path, const NativeIconSnapshot& snapshot);
bool LoadIconSnapshotFile(const std::wstring& file_path, NativeIconSnapshot& out_snapshot);

// Enumerates FOLDERID_Desktop + FOLDERID_PublicDesktop and resolves .lnk shortcut targets on background/STA thread.
std::vector<model::DesktopItem> EnumerateDesktopItems(const NativeIconSnapshot* optional_snapshot = nullptr);

// Enumerates direct children of a single directory mapped to a group card (bounded by max_items).
std::vector<model::DesktopItem> EnumerateDirectoryItems(
    const std::wstring& dir_path,
    const std::string& owner_group_id,
    size_t max_items = 500);

struct MiniFolderItem {
    std::wstring path;
    std::wstring name;
    std::wstring extension;
    bool is_folder = false;
    uint64_t size_bytes = 0;
    int64_t modified_unix_ms = 0;
};

struct RecycleBinStats {
    uint64_t item_count = 0;
    uint64_t total_bytes = 0;
};

struct DriveSpaceStats {
    std::wstring root_path = L"C:\\";
    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
};

std::wstring ResolveKnownFolder(const GUID& folder_id);
std::vector<MiniFolderItem> QueryFolderTopItems(const std::wstring& dir_path, size_t max_items = 6);
RecycleBinStats QueryRecycleBinStats() noexcept;
DriveSpaceStats QueryDriveSpaceStats(const std::wstring& root_path) noexcept;

// Watches FOLDERID_Desktop and FOLDERID_PublicDesktop asynchronously via ReadDirectoryChangesW.
class DesktopFolderWatcher {
public:
    using ChangeCallback = std::function<void()>;

    DesktopFolderWatcher() = default;
    ~DesktopFolderWatcher();
    DesktopFolderWatcher(const DesktopFolderWatcher&) = delete;
    DesktopFolderWatcher& operator=(const DesktopFolderWatcher&) = delete;

    bool Start(ChangeCallback on_changed, std::vector<std::wstring> extra_dirs = {});
    void Stop() noexcept;

private:
    void WatchThreadMain(std::vector<std::wstring> dirs);

    HANDLE stop_event_ = nullptr;
    std::atomic<bool> running_{false};
    ChangeCallback on_changed_;
    std::thread thread_;
};

struct MountHostCallbacks {
    std::function<LRESULT(HWND, UINT, WPARAM, LPARAM, bool& handled)> on_message;
    std::function<void(const model::RectI& bounds, uint32_t dpi)> on_geometry_changed;
    std::function<void()> on_wallpaper_changed;
    std::function<void()> on_explorer_restarted;
};

class DesktopMountHost {
public:
    DesktopMountHost() = default;
    ~DesktopMountHost();
    DesktopMountHost(const DesktopMountHost&) = delete;
    DesktopMountHost& operator=(const DesktopMountHost&) = delete;

    bool Mount(bool takeover_native_icons,
               const std::wstring& snapshot_file_path,
               MountHostCallbacks callbacks,
               const wchar_t* takeover_reg_key = desktop_takeover::kDefaultRecordKey);

    void Unmount(bool restore_icon_positions) noexcept;

    HWND Hwnd() const noexcept { return hwnd_; }
    bool IsMounted() const noexcept { return mounted_; }
    // Pixels: the layer window over the primary work area.
    model::RectI SurfaceBounds() const noexcept { return bounds_; }
    // The host's coordinate space is DIPs; this is the layer in screen DIPs.
    model::RectI SurfaceBoundsDip() const noexcept;
    uint32_t CurrentDpi() const noexcept { return dpi_; }
    float DipScale() const noexcept { return dpi_ / 96.0f; }
    void ClientToScreenDip(POINT* pt) const noexcept;
    void ScreenDipToClient(POINT* pt) const noexcept;
    POINT ClientDipToScreenPx(POINT pt) const noexcept;
    POINT ScreenPxToClientDip(POINT pt) const noexcept;
    bool HidNativeIcons() const noexcept { return hid_icons_; }
    // Turns Explorer's icons off once the layer shows something in their place.
    void HideNativeIconsNow();
    const DesktopHierarchyInfo& Hierarchy() const noexcept { return hierarchy_; }
    const NativeIconSnapshot& InitialSnapshot() const noexcept { return initial_snapshot_; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) noexcept;
    void ComputeBounds();
    HWND LayerHost() const noexcept;
    bool IsAboveHost() const noexcept;
    void PlaceAboveHost();
    void HideNativeIcons();
    void UpdateSurfaceBounds();

    HWND hwnd_ = nullptr;
    bool mounted_ = false;
    bool takeover_icons_ = false;
    bool hid_icons_ = false;
    bool icons_taken_ = false;
    bool placing_ = false;
    UINT msg_taskbar_created_ = 0;
    uint32_t dpi_ = 96;
    model::RectI bounds_{};
    DesktopHierarchyInfo hierarchy_{};
    NativeIconSnapshot initial_snapshot_{};
    std::wstring snapshot_path_;
    std::wstring takeover_reg_key_ = desktop_takeover::kDefaultRecordKey;
    MountHostCallbacks callbacks_;
};

} // namespace pulse::desktop::mount
