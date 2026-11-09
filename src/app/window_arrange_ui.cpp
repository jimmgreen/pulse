#include "window_arrange_ui.h"
#include "../common/localization.h"
#include "app_internal.h"
#include "app_messages.h"
#include "tab_transfer.h"
#include "window_factory.h"
#include <algorithm>
#include <cstdint>
namespace pulse {
namespace {
constexpr wchar_t kToken[] = L"Pulse.Arrange.Generation";
struct Saved {
    HWND hwnd;
    HANDLE token;
    WINDOWPLACEMENT placement;
    RECT start, target;
    bool visible;
};
std::vector<Saved> saved;
UINT_PTR timer = 0, expiry_timer = 0;
ULONGLONG started = 0, expires = 0;
uintptr_t generation = 0, batch_generation = 0;
bool animations = true;
bool Live(const Saved &v) {
    return IsWindow(v.hwnd) && GetPropW(v.hwnd, kToken) == v.token;
}
void Stop() {
    if (timer)
        KillTimer(nullptr, timer);
    timer = 0;
}
void Forget() {
    Stop();
    if (expiry_timer)
        KillTimer(nullptr, expiry_timer);
    expiry_timer = 0;
    for (const auto &v : saved)
        if (Live(v))
            RemovePropW(v.hwnd, kToken);
    saved.clear();
}
void CALLBACK Expire(HWND, UINT, UINT_PTR id, DWORD) {
    if (id == expiry_timer)
        Forget();
}
void Place(const Saved &v, RECT r) {
    if (Live(v) && IsWindowEnabled(v.hwnd))
        SetWindowPos(v.hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
void CALLBACK Tick(HWND, UINT, UINT_PTR id, DWORD) {
    if (id != timer)
        return;
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(GetCurrentThreadId(), &info) && (info.flags & (GUI_INMOVESIZE | GUI_INMENUMODE))) {
        Stop();
        return;
    }
    const double t = (std::min)(1.0, static_cast<double>(GetTickCount64() - started) / 180.0);
    const double p = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    for (const auto &v : saved) {
        auto lerp = [&](LONG a, LONG b) { return static_cast<LONG>(a + (b - a) * p); };
        Place(v, {lerp(v.start.left, v.target.left), lerp(v.start.top, v.target.top),
                  lerp(v.start.right, v.target.right), lerp(v.start.bottom, v.target.bottom)});
    }
    if (t >= 1.0)
        Stop();
}
bool Eligible(AppState &s) {
    return IsWindow(s.hwnd) && IsWindowEnabled(s.hwnd) && (!s.operationWindow || !s.operationWindow->IsVisible()) &&
           !s.hwndRenameEdit;
}
} // namespace
void CleanupWindowArrange() {
    Forget();
}
void SetWindowArrangeAnimations(bool enabled) {
    animations = enabled;
}
std::vector<HWND> WindowArrangementCandidates(AppState &s, bool all_monitors,
                                              const std::vector<HWND> &order, bool force_include) {
    if (!Eligible(s) || s.ops.Status().active) return {};
    const HMONITOR monitor = MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST);
    std::vector<HWND> windows;
    auto add = [&](HWND hwnd) {
        auto *other = s.core.windows.Lookup(hwnd);
        if (!other || !Eligible(*other) || std::find(windows.begin(), windows.end(), hwnd) != windows.end())
            return;
        if (!all_monitors && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != monitor)
            return;
        if (!(force_include || s.appPrefs.arrange_include_minimized) && (!IsWindowVisible(hwnd) || IsIconic(hwnd)))
            return;
        windows.push_back(hwnd);
    };
    for (HWND hwnd : order)
        add(hwnd);
    add(s.hwnd);
    s.core.windows.ForEach([&](HWND hwnd, AppState &) { add(hwnd); });
    return windows;
}
static bool ArrangeWindowsImpl(AppState &s, app::WindowArrangeLayout layout, bool all_monitors,
                               const std::vector<HWND> &order, bool force_include) {
    Stop();
    if (!Eligible(s) || s.ops.Status().active)
        return false;
    const HMONITOR monitor = MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi))
        return false;
    auto windows = WindowArrangementCandidates(s, all_monitors, order, force_include);
    const UINT dpi = static_cast<UINT>(96.0f * s.scale);
    const int gap = s.appPrefs.arrange_gap ? MulDiv(6, static_cast<int>(dpi), 96) : 0;
    MINMAXINFO minimum{};
    SendMessageW(s.hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
    for (HWND hwnd : windows) {
        MINMAXINFO value{};
        SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&value));
        minimum.ptMinTrackSize.x = (std::max)(minimum.ptMinTrackSize.x, value.ptMinTrackSize.x);
        minimum.ptMinTrackSize.y = (std::max)(minimum.ptMinTrackSize.y, value.ptMinTrackSize.y);
    }
    const int mw = minimum.ptMinTrackSize.x, mh = minimum.ptMinTrackSize.y;
    if (windows.size() < 2)
        return false;
    const auto calculate = [&](RECT work, const std::vector<HWND>& group, int spacing, int min_w, int min_h, int step) {
        std::vector<SIZE> sizes;
        if (layout == app::WindowArrangeLayout::Cascade) {
            for (HWND hwnd : group) {
                RECT bounds{};
                if (IsZoomed(hwnd) || IsIconic(hwnd)) {
                    WINDOWPLACEMENT placement{sizeof(placement)};
                    if (!GetWindowPlacement(hwnd, &placement)) return std::vector<RECT>{};
                    bounds = placement.rcNormalPosition;
                } else if (!GetWindowRect(hwnd, &bounds)) return std::vector<RECT>{};
                sizes.push_back({bounds.right - bounds.left, bounds.bottom - bounds.top});
            }
        }
        return app::CalculateWindowArrangement(layout, work, group.size(), spacing, min_w, min_h, step, sizes);
    };
    std::vector<RECT> rects;
    if (all_monitors) {
        std::vector<HWND> grouped;
        std::vector<HMONITOR> monitors;
        for (HWND hwnd : windows) {
            const auto m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (std::find(monitors.begin(), monitors.end(), m) == monitors.end())
                monitors.push_back(m);
        }
        for (HMONITOR m : monitors) {
            std::vector<HWND> group;
            for (HWND hwnd : windows)
                if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == m)
                    group.push_back(hwnd);
            MONITORINFO info{sizeof(info)};
            if (!GetMonitorInfoW(m, &info))
                return false;
            auto *first = s.core.windows.Lookup(group.front());
            const int scale = static_cast<int>(96.0f * first->scale);
            MINMAXINFO group_min{};
            for (HWND hwnd : group) {
                MINMAXINFO value{};
                SendMessageW(hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&value));
                group_min.ptMinTrackSize.x = (std::max)(group_min.ptMinTrackSize.x, value.ptMinTrackSize.x);
                group_min.ptMinTrackSize.y = (std::max)(group_min.ptMinTrackSize.y, value.ptMinTrackSize.y);
            }
            const auto positions = calculate(
                info.rcWork, group, s.appPrefs.arrange_gap ? MulDiv(6, scale, 96) : 0,
                group_min.ptMinTrackSize.x, group_min.ptMinTrackSize.y, MulDiv(32, scale, 96));
            if (positions.size() != group.size())
                return false;
            for (size_t i = 0; i < positions.size(); ++i) {
                grouped.push_back(group[i]);
                rects.push_back(positions[i]);
            }
        }
        windows = std::move(grouped);
    } else
        rects = calculate(mi.rcWork, windows, gap, mw, mh,
                                                MulDiv(32, static_cast<int>(dpi), 96));
    if (rects.size() < 2)
        return false;
    Forget();
    for (size_t i = 0; i < rects.size(); ++i) {
        Saved v{};
        v.hwnd = windows[i];
        v.placement.length = sizeof(WINDOWPLACEMENT);
        v.visible = IsWindowVisible(v.hwnd) != FALSE;
        if (!GetWindowPlacement(v.hwnd, &v.placement)) {
            Forget();
            return false;
        }
        v.token = reinterpret_cast<HANDLE>(++generation);
        if (!SetPropW(v.hwnd, kToken, v.token)) {
            Forget();
            return false;
        }
        v.target = rects[i];
        saved.push_back(v);
    }
    batch_generation = ++generation;
    for (auto &v : saved) {
        ShowWindow(v.hwnd, SW_SHOWNOACTIVATE);
        if (IsZoomed(v.hwnd) || IsIconic(v.hwnd))
            ShowWindow(v.hwnd, SW_RESTORE);
        if (!GetWindowRect(v.hwnd, &v.start))
            v.start = v.target;
    }
    started = GetTickCount64();
    expires = started + 8000;
    expiry_timer = SetTimer(nullptr, 0, 8000, Expire);
    ANIMATIONINFO ai{sizeof(ai)};
    const bool animate = animations && SystemParametersInfoW(SPI_GETANIMATION, sizeof(ai), &ai, 0) && ai.iMinAnimate;
    if (animate)
        timer = SetTimer(nullptr, 0, 15, Tick);
    if (!timer)
        for (const auto &v : saved)
            Place(v, v.target);
    s.notification_toast.ShowSnack(
        s.hwnd, ui::SnackIcon::Merge, l10n::Get(l10n::StringId::ArrangeApplied), {},
        {{l10n::Get(l10n::StringId::SnackUndo), WM_WINDOW_ARRANGE_UNDO, static_cast<WPARAM>(batch_generation)}}, {},
        8000);
    return true;
}
bool ArrangeWindows(AppState &s, app::WindowArrangeLayout layout, bool all_monitors, const std::vector<HWND> &order,
                    bool force_include) {
    const bool ok = ArrangeWindowsImpl(s, layout, all_monitors, order, force_include);
    if (!ok)
        s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Merge, l10n::Get(l10n::StringId::ArrangeUnavailable), {},
                                       {});
    return ok;
}
WPARAM WindowArrangeGeneration() {
    return static_cast<WPARAM>(batch_generation);
}
void UndoWindowArrange(AppState &s, WPARAM requested_generation) {
    if (requested_generation && requested_generation != batch_generation)
        return;
    Stop();
    if (GetTickCount64() <= expires)
        for (auto &v : saved) {
            auto *window = s.core.windows.Lookup(v.hwnd);
            if (window && Live(v) && Eligible(*window)) {
                SetWindowPlacement(v.hwnd, &v.placement);
                if (!v.visible)
                    ShowWindow(v.hwnd, SW_HIDE);
            }
        }
    Forget();
}
} // namespace pulse
