// dock_stack_popup.h - Dock stacks (叠放), as on the Mac: clicking a folder in the Dock shows its
// items in a Fan (扇形: the icons rise from the Dock icon along an arc, each name on a pill beside
// it), a Grid (网格: a panel of icons) or a List (列表). Automatic picks the Fan when the items fit
// above a bottom Dock, the Grid beside a side Dock. The 应用程序 stack lists the Start menu's apps
// (shell:AppsFolder, desktop and Store apps alike) in the Grid.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::desktop::overlays {

enum class DockStackView : uint8_t { Automatic = 0, Fan = 1, Grid = 2, List = 3 };
enum class DockStackSort : uint8_t { DateAdded = 0, Name = 1, DateModified = 2, Kind = 3 };

struct DockStackEntry {
    std::wstring path;   // file system path, or "shell:AppsFolder\<id>" for an app
    std::wstring name;   // shown name (.lnk / .url endings dropped)
    bool is_dir = false;
    bool is_app = false;
    uint64_t added = 0;    // FILETIME ticks (creation time in the folder)
    uint64_t modified = 0; // FILETIME ticks
};

// Pure helpers (unit tested).
void SortDockStackEntries(std::vector<DockStackEntry>& entries, DockStackSort sort);
std::wstring DockStackDisplayName(const std::wstring& file_name, bool is_dir);
bool IsDockStackAppNoise(const std::wstring& name); // uninstallers, read-me and help links
// The visible items of `folder` (hidden / system files skipped), sorted.
std::vector<DockStackEntry> ListDockStackFolder(const std::wstring& folder, DockStackSort sort);
// Centre of fan item `i` (0 = nearest the Dock) relative to the top centre of the Dock icon, in DIP
// (y grows downwards, so it is negative). The arc bends right as it rises, like the Mac's.
struct DockFanPoint { float x = 0.0f, y = 0.0f; };
DockFanPoint DockFanItemCenter(int i) noexcept;
constexpr float kDockFanStep = 58.0f;  // DIP between fan items along the arc
constexpr float kDockFanIcon = 48.0f;  // fan icon size, DIP
// How many file items a fan can show in `room_dip` of height above the icon (one row is kept for
// the 在 Pulse 中打开 item); the Mac shows at most about a dozen before it switches to the Grid.
int DockFanCapacity(float room_dip) noexcept;
// Automatic: the Fan for a bottom Dock (the newest items that fit, the rest one click away through
// 在 Pulse 中打开); the Grid beside a side Dock and for 应用程序.
DockStackView ResolveDockStackView(DockStackView requested, bool is_apps, uint8_t dock_edge,
                                   size_t item_count, int fan_capacity) noexcept;

struct DockStackRequest {
    std::wstring key;      // the Dock item's key ("special:downloads", "special:apps", a folder path)
    std::wstring title;    // the Dock item's name
    std::wstring folder;   // folder to show; empty with is_apps
    bool is_apps = false;
    DockStackView view = DockStackView::Automatic;
    DockStackSort sort = DockStackSort::DateAdded;
    RECT icon_screen{};    // the Dock icon, screen px
    RECT work_screen{};    // the monitor work area, screen px
    float scale = 1.0f;    // px per DIP
    uint8_t dock_edge = 0; // 0 bottom, 1 left, 2 right
    bool dark = true;
    bool headless = false; // lay out and enumerate only; never shown (tests / review snapshots)
};

class DockStackPopup {
public:
    // Opens an entry: a file or app is launched, a folder (or 在 Pulse 中打开) opens in Pulse.
    using OpenFn = std::function<void(const DockStackEntry& entry)>;

    DockStackPopup();
    ~DockStackPopup();
    DockStackPopup(const DockStackPopup&) = delete;
    DockStackPopup& operator=(const DockStackPopup&) = delete;

    void SetOpenHandler(OpenFn fn);
    bool Show(const DockStackRequest& request);
    void Close(bool animate = true);
    bool IsOpen() const noexcept;
    bool IsOpenFor(const std::wstring& key) const noexcept;
    // True just after the stack for `key` closed, so the click on its Dock icon that closed it (by
    // taking focus away) does not open it again.
    bool RecentlyClosedFor(const std::wstring& key, ULONGLONG window_ms = 350) const noexcept;
    HWND Hwnd() const noexcept;

    // Headless render of the open stack's current frame (tests / review snapshots).
    bool RenderSnapshot(std::vector<uint32_t>& out_bgra, int& out_w, int& out_h, float progress = 1.0f);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::desktop::overlays
