// live_wallpaper_detector.h - Event-driven, zero-injection detector for third-party
// dynamic wallpaper engines (Wallpaper Engine, Lively, etc.) across Classic WorkerW
// and Windows 11 24H2 layered Progman hierarchies.
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace pulse::desktop::host {

struct LiveWallpaperHierarchy {
    uint8_t mode = 0; // Matches mount::DesktopHierarchyMode (1=ClassicWorkerW, 2=24H2, 3=Fallback)
    uintptr_t progman = 0;
    uintptr_t defview_parent = 0;
    uintptr_t shelldll_defview = 0;
    uintptr_t workerw_wallpaper = 0;
    uint32_t explorer_pid = 0;
    uint32_t self_pid = 0;
    int32_t primary_work_w = 1920;
    int32_t primary_work_h = 1080;
};

struct LiveWallpaperCandidate {
    uintptr_t hwnd = 0;
    uintptr_t root_hwnd = 0;         // GetAncestor(hwnd, GA_ROOT) — top-level capture target
    uintptr_t desktop_ancestor = 0;  // `workerw_wallpaper` or `progman` if child/descendant, else 0
    uint32_t pid = 0;
    int32_t width = 0;
    int32_t height = 0;
    bool visible = false;
    bool cloaked = false;
    bool minimized = false;
    bool below_desktop_icons = false; // true for top-level windows Z-ordered behind SHELLDLL_DefView's host
};

struct LiveWallpaperDecision {
    bool active = false;
    uintptr_t wallpaper_hwnd = 0;
    uintptr_t capture_target_hwnd = 0;
    uint32_t wallpaper_pid = 0;

    constexpr bool operator==(const LiveWallpaperDecision&) const noexcept = default;
};

// Pure deterministic evaluator (no Win32 calls; directly unit-testable).
inline LiveWallpaperDecision EvaluateLiveWallpaper(
    const LiveWallpaperHierarchy& h,
    std::span<const LiveWallpaperCandidate> candidates) noexcept {
    LiveWallpaperDecision out{};
    if (h.primary_work_w <= 0 || h.primary_work_h <= 0) {
        return out;
    }

    const int64_t min_area =
        (static_cast<int64_t>(h.primary_work_w) * static_cast<int64_t>(h.primary_work_h) * 40) / 100;

    int64_t best_score = -1;
    for (const auto& c : candidates) {
        if (c.hwnd == 0 || !c.visible || c.cloaked || c.minimized) continue;
        if (c.pid == 0 || c.pid == h.explorer_pid || c.pid == h.self_pid) continue;
        if (c.hwnd == h.progman || c.hwnd == h.defview_parent ||
            c.hwnd == h.shelldll_defview || c.hwnd == h.workerw_wallpaper) {
            continue;
        }
        if (c.width <= 0 || c.height <= 0) continue;

        const int64_t area = static_cast<int64_t>(c.width) * static_cast<int64_t>(c.height);
        if (area < min_area) continue;

        const bool parented_to_wallpaper =
            (h.workerw_wallpaper != 0 && c.desktop_ancestor == h.workerw_wallpaper) ||
            (h.progman != 0 && c.desktop_ancestor == h.progman);
        if (!parented_to_wallpaper && !c.below_desktop_icons) {
            continue;
        }

        // Prefer windows explicitly parented inside Explorer's wallpaper hierarchy over loose Z-order peers.
        const int64_t score = area + (parented_to_wallpaper ? (int64_t{1} << 40) : 0);
        if (score > best_score) {
            best_score = score;
            out.active = true;
            out.wallpaper_hwnd = c.hwnd;
            out.capture_target_hwnd = (c.root_hwnd != 0) ? c.root_hwnd : c.hwnd;
            out.wallpaper_pid = c.pid;
        }
    }
    return out;
}

} // namespace pulse::desktop::host
