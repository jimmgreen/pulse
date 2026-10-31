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
struct Spawned {
    HWND source, window;
    app::LayoutTab *tab;
    size_t index;
    app::LayoutTab *copy = nullptr;
    std::wstring copy_path;
    uint64_t selection = 0, view = 0;
    size_t back = 0, forward = 0;
};
std::vector<Spawned> spawned;
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
    spawned.clear();
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
static bool ArrangeWindowsImpl(AppState &s, app::WindowArrangeLayout layout, bool all_monitors,
                               const std::vector<HWND> &order, bool force_include) {
    Stop();
    if (!Eligible(s) || s.ops.Status().active)
        return false;
    const HMONITOR monitor = MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi))
        return false;
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
    const UINT dpi = static_cast<UINT>(96.0f * s.scale);
    const int gap = s.appPrefs.arrange_gap ? MulDiv(6, static_cast<int>(dpi), 96) : 0;
    MINMAXINFO minimum{};
    SendMessageW(s.hwnd, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&minimum));
    const int mw = minimum.ptMinTrackSize.x, mh = minimum.ptMinTrackSize.y;
    std::vector<Spawned> new_windows;
    struct Rollback {
        AppState &source;
        std::vector<Spawned> &windows;
        ~Rollback() {
            for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
                auto *dst = source.core.windows.Lookup(it->window);
                if (!dst || dst->window_tabs.items.size() != 1)
                    continue;
                if (it->tab && dst->window_tabs.items[0].get() == it->tab &&
                    MoveLayoutTab(*dst, 0, source, it->index, TransferSource::Closing))
                    DestroyWindow(it->window);
                else if (it->copy && dst->window_tabs.items[0].get() == it->copy)
                    DestroyWindow(it->window);
            }
        }
    } rollback{s, new_windows};
    // Check room before creating or transferring a tab.
    if (windows.size() < 2) {
        const size_t desired =
            layout == app::WindowArrangeLayout::Columns ? 3
            : (layout == app::WindowArrangeLayout::SideBySide || layout == app::WindowArrangeLayout::Stacked)
                ? 2
                : s.window_tabs.items.size();
        if (desired < 2 || (desired > s.window_tabs.items.size() && desired != 2))
            return false;
        if (app::CalculateWindowArrangement(layout, mi.rcWork, desired, gap, mw, mh,
                                            MulDiv(32, static_cast<int>(dpi), 96))
                .empty())
            return false;
        for (size_t i = 1; i < desired; ++i) {
            WindowInit init;
            init.monitor = monitor;
            app::LayoutTab *moved = nullptr;
            if (s.window_tabs.items.size() > 1) {
                init.adopt_from = &s;
                init.adopt_index = s.window_tabs.active + 1 < s.window_tabs.items.size() ? s.window_tabs.active + 1
                                                                                         : s.window_tabs.active - 1;
                moved = s.window_tabs.items[init.adopt_index].get();
            } else {
                if (layout != app::WindowArrangeLayout::SideBySide && layout != app::WindowArrangeLayout::Stacked)
                    break;
                auto *tab = s.window_tabs.Active();
                auto *folder = tab ? tab->ActiveFolder() : nullptr;
                if (!folder)
                    break;
                init.path = folder->current_path;
            }
            const HWND created = OpenPulseWindow(s, init);
            if (!created)
                return false;
            Spawned record{s.hwnd, created, moved, init.adopt_index, nullptr, {}};
            if (!moved) {
                auto *dst = s.core.windows.Lookup(created);
                record.copy = dst ? dst->window_tabs.Active() : nullptr;
                record.copy_path = init.path;
                if (auto *folder = record.copy ? record.copy->ActiveFolder() : nullptr) {
                    record.selection = folder->selection_revision;
                    record.view = folder->view_generation;
                    record.back = folder->back_stack.size();
                    record.forward = folder->forward_stack.size();
                }
            }
            new_windows.push_back(std::move(record));
            add(created);
        }
    }
    if (windows.size() < 2)
        return false;
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
                continue;
            auto *first = s.core.windows.Lookup(group.front());
            const int scale = static_cast<int>(96.0f * first->scale);
            MINMAXINFO group_min{};
            SendMessageW(group.front(), WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&group_min));
            const auto positions = app::CalculateWindowArrangement(
                layout, info.rcWork, group.size(), s.appPrefs.arrange_gap ? MulDiv(6, scale, 96) : 0,
                group_min.ptMinTrackSize.x, group_min.ptMinTrackSize.y, MulDiv(32, scale, 96));
            for (size_t i = 0; i < positions.size(); ++i) {
                grouped.push_back(group[i]);
                rects.push_back(positions[i]);
            }
        }
        windows = std::move(grouped);
    } else
        rects = app::CalculateWindowArrangement(layout, mi.rcWork, windows.size(), gap, mw, mh,
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
    spawned = std::move(new_windows);
    new_windows.clear();
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
    if (GetTickCount64() <= expires)
        for (auto it = spawned.rbegin(); it != spawned.rend(); ++it) {
            auto live = [&](HWND hwnd) {
                return std::any_of(saved.begin(), saved.end(),
                                   [&](const Saved &v) { return v.hwnd == hwnd && Live(v); });
            };
            auto *src = s.core.windows.Lookup(it->source);
            auto *dst = s.core.windows.Lookup(it->window);
            if (src && dst && live(it->source) && live(it->window) && Eligible(*src) && Eligible(*dst) &&
                dst->window_tabs.items.size() == 1) {
                if (it->tab && dst->window_tabs.items.front().get() == it->tab &&
                    MoveLayoutTab(*dst, 0, *src, it->index, TransferSource::Closing))
                    DestroyWindow(it->window);
                else if (it->copy && dst->window_tabs.Active() == it->copy) {
                    auto *folder = it->copy->ActiveFolder();
                    if (folder && folder->current_path == it->copy_path &&
                        folder->selection_revision == it->selection && folder->view_generation == it->view &&
                        folder->back_stack.size() == it->back && folder->forward_stack.size() == it->forward)
                        DestroyWindow(it->window);
                }
            }
        }
    Forget();
}
} // namespace pulse
