// live_wallpaper_watcher.h - Win32/WinEvent out-of-context observer around EvaluateLiveWallpaper.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "host/desktop_mount.h"
#include "host/live_wallpaper_detector.h"

#include <atomic>

namespace pulse::desktop::host {

LiveWallpaperDecision ProbeLiveWallpaperFromSystem(
    const mount::DesktopHierarchyInfo& hierarchy,
    int32_t primary_work_w,
    int32_t primary_work_h) noexcept;

// Out-of-context WinEvent listener (zero polling timers, zero injection) that posts
// `notify_msg` to `notify_hwnd` only when window show/hide/destroy/parent/cloak changes occur.
class LiveWallpaperWatcher {
public:
    LiveWallpaperWatcher() = default;
    ~LiveWallpaperWatcher() { Stop(); }
    LiveWallpaperWatcher(const LiveWallpaperWatcher&) = delete;
    LiveWallpaperWatcher& operator=(const LiveWallpaperWatcher&) = delete;

    bool Start(HWND notify_hwnd, UINT notify_msg) noexcept;
    void Stop() noexcept;

    // Clears the coalesced notification latch and re-evaluates the current desktop hierarchy.
    // Returns true if `decision_` changed since the previous probe.
    bool Refresh(const mount::DesktopHierarchyInfo& hierarchy,
                 int32_t primary_work_w,
                 int32_t primary_work_h) noexcept;

    const LiveWallpaperDecision& Current() const noexcept { return decision_; }

private:
    static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                      LONG id_object, LONG id_child,
                                      DWORD event_thread, DWORD event_time) noexcept;

    HWND notify_hwnd_ = nullptr;
    UINT notify_msg_ = 0;
    HWINEVENTHOOK hook_lifecycle_ = nullptr;
    HWINEVENTHOOK hook_hierarchy_ = nullptr;
    std::atomic<bool> notify_pending_{false};
    LiveWallpaperDecision decision_{};
};

} // namespace pulse::desktop::host
