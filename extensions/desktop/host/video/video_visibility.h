#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

#include <cstdint>
#include <vector>

#include "host/video/wallpaper_policy.h"

namespace pulse::desktop::video {

inline bool CoversDesktopWorkArea(const RECT& frame, const RECT& work) noexcept {
    RECT intersection{};
    if (!IntersectRect(&intersection, &frame, &work)) return false;
    const int64_t work_area = static_cast<int64_t>(work.right - work.left) * (work.bottom - work.top);
    const int64_t covered_area = static_cast<int64_t>(intersection.right - intersection.left) *
                                (intersection.bottom - intersection.top);
    return work_area > 0 && covered_area * 100 >= work_area * 98;
}

inline bool IsForegroundDesktopShell(HWND candidate, HWND desktop_hwnd, HWND overlay_hwnd,
                                     DWORD host_process_id) noexcept {
    if (!candidate || !IsWindow(candidate)) return true;
    if (candidate == desktop_hwnd || candidate == overlay_hwnd) return true;
    DWORD process_id = 0;
    GetWindowThreadProcessId(candidate, &process_id);
    if (host_process_id != 0 && process_id == host_process_id) return true;
    wchar_t cls[64]{};
    GetClassNameW(candidate, cls, ARRAYSIZE(cls));
    return _wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0 ||
           _wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

// Accept the candidate explicitly so an isolated desktop can exercise the same
// Win32 path without taking foreground focus away from the user's application.
inline bool IsSingleWindowCoveringDesktop(HWND candidate, HWND desktop_hwnd, HWND overlay_hwnd,
                                          DWORD host_process_id) noexcept {
    if (!candidate || !IsWindow(candidate) || !IsWindowVisible(candidate) || IsIconic(candidate)) return false;
    if (candidate == desktop_hwnd || candidate == overlay_hwnd) return false;
    DWORD process_id = 0;
    GetWindowThreadProcessId(candidate, &process_id);
    if (host_process_id != 0 && process_id == host_process_id) return false;

    wchar_t cls[64]{};
    GetClassNameW(candidate, cls, ARRAYSIZE(cls));
    if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0 ||
        _wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0) return false;

    const LONG_PTR ex = GetWindowLongPtrW(candidate, GWL_EXSTYLE);
    // Transparent click-through helper overlays (HUDs, screen-recording borders, capture frames)
    // do not occlude the desktop underneath.
    if ((ex & WS_EX_TRANSPARENT) &&
        ((ex & WS_EX_LAYERED) || (ex & WS_EX_TOOLWINDOW) ||
         (ex & WS_EX_TOPMOST) || (ex & WS_EX_NOREDIRECTIONBITMAP))) {
        return false;
    }
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(candidate, nullptr, &alpha, &flags)) {
            // Color-keyed hollow frames (classic recording borders) or translucent masks do not occlude.
            if ((flags & LWA_COLORKEY) != 0) return false;
            if ((flags & LWA_ALPHA) != 0 && alpha < 220) return false;
        } else if ((ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) {
            // UpdateLayeredWindow per-pixel-alpha topmost/tool overlay (e.g. QQ/WeChat/Snipaste
            // recording frame or floating annotation canvas).
            return false;
        }
    }

    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(candidate, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }
    RECT frame{};
    if (FAILED(DwmGetWindowAttribute(candidate, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
        !GetWindowRect(candidate, &frame)) return false;
    const HMONITOR monitor = MonitorFromWindow(desktop_hwnd ? desktop_hwnd : candidate,
                                               MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info) || !CoversDesktopWorkArea(frame, info.rcWork)) return false;

    // Respect custom window regions (SetWindowRgn): hollow 4-strip screen-recording borders have a
    // full-screen bounding rect but cover < 2% of the work area once the center cutout is excluded.
    HRGN hrgn = CreateRectRgn(0, 0, 0, 0);
    if (hrgn) {
        const int rgn_type = GetWindowRgn(candidate, hrgn);
        if (rgn_type == NULLREGION) {
            DeleteObject(hrgn);
            return false;
        }
        if (rgn_type == SIMPLEREGION || rgn_type == COMPLEXREGION) {
            const DWORD bytes = GetRegionData(hrgn, 0, nullptr);
            if (bytes >= sizeof(RGNDATAHEADER) && bytes <= 64u * 1024u) {
                std::vector<uint8_t> buf(bytes);
                auto* rd = reinterpret_cast<RGNDATA*>(buf.data());
                if (GetRegionData(hrgn, bytes, rd) == bytes) {
                    RECT wr{};
                    GetWindowRect(candidate, &wr);
                    int64_t rgn_covered = 0;
                    const auto* rects = reinterpret_cast<const RECT*>(rd->Buffer);
                    for (DWORD i = 0; i < rd->rdh.nCount; ++i) {
                        RECT sr = rects[i];
                        OffsetRect(&sr, wr.left, wr.top);
                        RECT inter{};
                        if (IntersectRect(&inter, &sr, &info.rcWork)) {
                            rgn_covered += static_cast<int64_t>(inter.right - inter.left) *
                                           (inter.bottom - inter.top);
                        }
                    }
                    const int64_t work_area = static_cast<int64_t>(info.rcWork.right - info.rcWork.left) *
                                              (info.rcWork.bottom - info.rcWork.top);
                    if (work_area > 0 && rgn_covered * 100 < work_area * 98) {
                        DeleteObject(hrgn);
                        return false;
                    }
                }
            }
        }
        DeleteObject(hrgn);
    }
    return true;
}

inline bool IsWindowCoveringDesktop(HWND candidate, HWND desktop_hwnd, HWND overlay_hwnd,
                                    DWORD host_process_id) noexcept {
    if (IsSingleWindowCoveringDesktop(candidate, desktop_hwnd, overlay_hwnd, host_process_id)) return true;
    if (!candidate || !IsWindowVisible(candidate) || IsIconic(candidate) ||
        candidate == desktop_hwnd || candidate == overlay_hwnd) return false;
    // CAD/tool windows can own the foreground while their maximized main
    // window remains underneath. Follow only its same-process owner chain.
    const HWND root_owner = GetAncestor(candidate, GA_ROOTOWNER);
    if (!root_owner || root_owner == candidate) return false;
    DWORD candidate_pid = 0;
    DWORD owner_pid = 0;
    GetWindowThreadProcessId(candidate, &candidate_pid);
    GetWindowThreadProcessId(root_owner, &owner_pid);
    return candidate_pid == owner_pid && owner_pid != host_process_id &&
           IsSingleWindowCoveringDesktop(root_owner, desktop_hwnd, overlay_hwnd, host_process_id);
}

inline PauseSignals MakeVisibilityPauseSignals(bool manual, bool session_locked, bool display_off,
                                                bool auto_sleep_enabled, VideoPauseReason system_reason,
                                                bool desktop_occluded) noexcept {
    PauseSignals signals{};
    signals.manual = manual;
    signals.session_locked = session_locked;
    signals.display_off = display_off;
    signals.fullscreen = auto_sleep_enabled && system_reason == VideoPauseReason::Fullscreen;
    signals.battery_saver = auto_sleep_enabled && system_reason == VideoPauseReason::BatterySaver;
    signals.desktop_occluded = auto_sleep_enabled && desktop_occluded;
    return signals;
}

inline bool ShouldPauseVideoTranscoder(const PauseSignals& signals) noexcept {
    return signals.fullscreen || signals.battery_saver || signals.session_locked ||
           signals.display_off || signals.desktop_occluded;
}

inline bool ShouldStartVideoPaused(bool manual, PauseLifecycleState state) noexcept {
    return manual || state != PauseLifecycleState::Playing;
}

} // namespace pulse::desktop::video
