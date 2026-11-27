// live_wallpaper_detector.cpp - Win32 candidate enumeration & out-of-context WinEvent hook.
#include "host/live_wallpaper_watcher.h"

#include <cwchar>
#include <dwmapi.h>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace pulse::desktop::host {
namespace {

uintptr_t ToId(HWND hwnd) noexcept {
    return reinterpret_cast<uintptr_t>(hwnd);
}

bool IsWindowCloaked(HWND hwnd) noexcept {
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return false;
    }
    return cloaked != 0;
}

bool IsDesktopClassName(const wchar_t* cls) noexcept {
    return cls && (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0);
}

bool IsShellChromeClassName(const wchar_t* cls) noexcept {
    return cls && (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0);
}

bool IsOverlayPassthroughWindow(HWND hwnd) noexcept {
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return ((ex & WS_EX_TRANSPARENT) != 0) ||
           (((ex & WS_EX_LAYERED) != 0) && ((ex & WS_EX_NOACTIVATE) != 0));
}

bool IsEligibleDesktopHost(HWND hwnd, int64_t min_area) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    // WGC CreateForWindow only succeeds on top-level windows (never WS_CHILD).
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsWindowCloaked(hwnd)) return false;
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return false;
    const int64_t w = static_cast<int64_t>(rc.right) - static_cast<int64_t>(rc.left);
    const int64_t h = static_cast<int64_t>(rc.bottom) - static_cast<int64_t>(rc.top);
    return w > 0 && h > 0 && (w * h) >= min_area;
}

LiveWallpaperCandidate BuildCandidate(HWND hwnd, uintptr_t desktop_ancestor, bool below_icons) noexcept {
    LiveWallpaperCandidate c{};
    if (!hwnd || !IsWindow(hwnd)) return c;
    c.hwnd = ToId(hwnd);
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    c.root_hwnd = ToId(root ? root : hwnd);
    c.desktop_ancestor = desktop_ancestor;
    c.below_desktop_icons = below_icons;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    c.pid = static_cast<uint32_t>(pid);

    c.visible = IsWindowVisible(hwnd) != FALSE;
    c.minimized = IsIconic(hwnd) != FALSE;
    c.cloaked = c.visible ? IsWindowCloaked(hwnd) : false;

    RECT rc{};
    if (GetWindowRect(hwnd, &rc)) {
        c.width = rc.right - rc.left;
        c.height = rc.bottom - rc.top;
    }
    return c;
}

struct EnumDescendantCtx {
    uintptr_t ancestor_id = 0;
    bool below_icons = false;
    std::vector<LiveWallpaperCandidate>* out = nullptr;
    std::unordered_set<uintptr_t>* seen = nullptr;
};

BOOL CALLBACK EnumChildProc(HWND child, LPARAM lparam) noexcept {
    auto* ctx = reinterpret_cast<EnumDescendantCtx*>(lparam);
    if (!ctx || !ctx->out || !ctx->seen) return FALSE;
    if (ctx->out->size() >= 192) return FALSE;
    const uintptr_t id = ToId(child);
    if (!ctx->seen->insert(id).second) return TRUE;
    ctx->out->push_back(BuildCandidate(child, ctx->ancestor_id, ctx->below_icons));
    return TRUE;
}

struct EnumWorkerWsCtx {
    HWND defview_parent = nullptr;
    std::vector<HWND>* workerws = nullptr;
};

BOOL CALLBACK EnumAllWorkerWsProc(HWND top, LPARAM lparam) noexcept {
    auto* ctx = reinterpret_cast<EnumWorkerWsCtx*>(lparam);
    if (!ctx || !ctx->workerws) return FALSE;
    if (top == ctx->defview_parent) return TRUE;
    wchar_t cls[64]{};
    if (GetClassNameW(top, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"WorkerW") == 0) {
        ctx->workerws->push_back(top);
    }
    return TRUE;
}

std::mutex g_watcher_mtx;
std::vector<LiveWallpaperWatcher*> g_active_watchers;

} // namespace

LiveWallpaperDecision ProbeLiveWallpaperFromSystem(
    const mount::DesktopHierarchyInfo& hierarchy,
    int32_t primary_work_w,
    int32_t primary_work_h,
    HWND self_surface_hwnd,
    bool allow_desktop_host_capture) noexcept {
    LiveWallpaperHierarchy h{};
    h.mode = static_cast<uint8_t>(hierarchy.mode);
    h.progman = ToId(hierarchy.progman);
    h.defview_parent = ToId(hierarchy.defview_parent);
    h.shelldll_defview = ToId(hierarchy.shelldll_defview);
    h.workerw_wallpaper = ToId(hierarchy.workerw_wallpaper);
    h.explorer_pid = static_cast<uint32_t>(hierarchy.explorer_pid);
    h.self_pid = static_cast<uint32_t>(GetCurrentProcessId());
    h.primary_work_w = primary_work_w > 0 ? primary_work_w : std::max(1, GetSystemMetrics(SM_CXSCREEN));
    h.primary_work_h = primary_work_h > 0 ? primary_work_h : std::max(1, GetSystemMetrics(SM_CYSCREEN));

    std::vector<LiveWallpaperCandidate> candidates;
    candidates.reserve(64);
    std::unordered_set<uintptr_t> seen;
    seen.reserve(96);

    if (hierarchy.workerw_wallpaper && IsWindow(hierarchy.workerw_wallpaper)) {
        EnumDescendantCtx ctx{ToId(hierarchy.workerw_wallpaper), false, &candidates, &seen};
        EnumChildWindows(hierarchy.workerw_wallpaper, &EnumChildProc, reinterpret_cast<LPARAM>(&ctx));
    }
    if (hierarchy.progman && IsWindow(hierarchy.progman) &&
        hierarchy.progman != hierarchy.workerw_wallpaper) {
        EnumDescendantCtx ctx{ToId(hierarchy.progman), false, &candidates, &seen};
        EnumChildWindows(hierarchy.progman, &EnumChildProc, reinterpret_cast<LPARAM>(&ctx));
    }

    // Scan ALL other top-level WorkerW windows too: multi-WorkerW engines (Wallpaper Engine, Lively,
    // 元气壁纸, 飞火, etc.) frequently parent their video renderer inside a sibling WorkerW.
    std::vector<HWND> extra_workerws;
    {
        EnumWorkerWsCtx wctx{hierarchy.defview_parent, &extra_workerws};
        EnumWindows(&EnumAllWorkerWsProc, reinterpret_cast<LPARAM>(&wctx));
    }
    for (HWND ww : extra_workerws) {
        if (!ww || ww == hierarchy.workerw_wallpaper || ww == hierarchy.progman || !IsWindow(ww)) continue;
        EnumDescendantCtx ctx{0, true, &candidates, &seen};
        EnumChildWindows(ww, &EnumChildProc, reinterpret_cast<LPARAM>(&ctx));
    }

    // Inspect top-level windows Z-ordered strictly beneath our desktop surface and beneath Explorer's
    // desktop icon host (covers engines and video windows positioned behind SHELLDLL_DefView's parent).
    auto scan_below = [&](HWND start_hwnd) {
        if (!start_hwnd || !IsWindow(start_hwnd)) return;
        size_t scanned = 0;
        for (HWND w = GetWindow(start_hwnd, GW_HWNDNEXT); w && scanned < 64; w = GetWindow(w, GW_HWNDNEXT), ++scanned) {
            if (w == self_surface_hwnd || w == hierarchy.progman || w == hierarchy.workerw_wallpaper ||
                w == hierarchy.defview_parent) {
                continue;
            }
            wchar_t cls[64]{};
            if (GetClassNameW(w, cls, ARRAYSIZE(cls)) > 0 &&
                (IsDesktopClassName(cls) || IsShellChromeClassName(cls))) {
                continue;
            }
            if (IsOverlayPassthroughWindow(w)) continue;
            const uintptr_t id = ToId(w);
            if (!seen.insert(id).second) continue;
            candidates.push_back(BuildCandidate(w, 0, true));
        }
    };
    const HWND icon_host = hierarchy.defview_parent ? hierarchy.defview_parent : hierarchy.progman;
    if (self_surface_hwnd && self_surface_hwnd != icon_host) {
        scan_below(self_surface_hwnd);
    }
    scan_below(icon_host);

    LiveWallpaperDecision decision = EvaluateLiveWallpaper(h, candidates);
    if (decision.active || !allow_desktop_host_capture) {
        return decision;
    }

    // Reference-aligned live desktop capture: when no separate third-party video child was matched,
    // capture Explorer's top-level desktop host (WorkerW / Progman) directly via borderless WGC so
    // live wallpaper transitions, Slideshow, Spotlight, and DirectComposition wallpapers update in real time.
    const int64_t min_area =
        (static_cast<int64_t>(h.primary_work_w) * static_cast<int64_t>(h.primary_work_h) * 40) / 100;
    HWND desk = nullptr;
    if (IsEligibleDesktopHost(hierarchy.workerw_wallpaper, min_area)) {
        desk = hierarchy.workerw_wallpaper;
    }
    if (!desk && icon_host && IsWindow(icon_host)) {
        for (HWND w = GetWindow(icon_host, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
            if (w == hierarchy.defview_parent) continue;
            wchar_t cls[64]{};
            if (GetClassNameW(w, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"WorkerW") == 0 &&
                IsEligibleDesktopHost(w, min_area)) {
                desk = w;
                break;
            }
        }
    }
    if (!desk && IsEligibleDesktopHost(hierarchy.progman, min_area)) {
        desk = hierarchy.progman;
    }
    if (!desk && IsEligibleDesktopHost(hierarchy.defview_parent, min_area)) {
        desk = hierarchy.defview_parent;
    }

    if (desk) {
        DWORD desk_pid = h.explorer_pid;
        if (desk_pid == 0) GetWindowThreadProcessId(desk, &desk_pid);
        decision.active = true;
        decision.wallpaper_hwnd = ToId(desk);
        decision.capture_target_hwnd = ToId(desk);
        decision.wallpaper_pid = static_cast<uint32_t>(desk_pid);
        decision.is_desktop_host = true;
    }
    return decision;
}

bool LiveWallpaperWatcher::Start(HWND notify_hwnd, UINT notify_msg) noexcept {
    Stop();
    if (!notify_hwnd || !IsWindow(notify_hwnd) || notify_msg == 0) return false;
    notify_hwnd_ = notify_hwnd;
    notify_msg_ = notify_msg;
    notify_pending_.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_watcher_mtx);
        g_active_watchers.push_back(this);
    }

    // Subscribe ONLY to window create/destroy/show/hide and reparent/cloak events (never mouse/location!).
    constexpr DWORD kFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    hook_lifecycle_ = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr, &LiveWallpaperWatcher::WinEventProc, 0, 0, kFlags);
    hook_hierarchy_ = SetWinEventHook(
        EVENT_OBJECT_PARENTCHANGE, EVENT_OBJECT_UNCLOAKED, nullptr, &LiveWallpaperWatcher::WinEventProc, 0, 0, kFlags);
    return hook_lifecycle_ != nullptr || hook_hierarchy_ != nullptr;
}

void LiveWallpaperWatcher::Stop() noexcept {
    if (hook_lifecycle_) {
        UnhookWinEvent(hook_lifecycle_);
        hook_lifecycle_ = nullptr;
    }
    if (hook_hierarchy_) {
        UnhookWinEvent(hook_hierarchy_);
        hook_hierarchy_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_watcher_mtx);
        g_active_watchers.erase(
            std::remove(g_active_watchers.begin(), g_active_watchers.end(), this),
            g_active_watchers.end());
    }
    notify_hwnd_ = nullptr;
    notify_msg_ = 0;
    notify_pending_.store(false, std::memory_order_relaxed);
}

bool LiveWallpaperWatcher::Refresh(const mount::DesktopHierarchyInfo& hierarchy,
                                   int32_t primary_work_w,
                                   int32_t primary_work_h,
                                   HWND self_surface_hwnd,
                                   bool allow_desktop_host_capture) noexcept {
    notify_pending_.store(false, std::memory_order_release);
    const LiveWallpaperDecision next = ProbeLiveWallpaperFromSystem(
        hierarchy, primary_work_w, primary_work_h, self_surface_hwnd, allow_desktop_host_capture);
    if (next == decision_) return false;
    decision_ = next;
    return true;
}

void CALLBACK LiveWallpaperWatcher::WinEventProc(HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd,
                                                 LONG id_object, LONG id_child,
                                                 DWORD /*event_thread*/, DWORD /*event_time*/) noexcept {
    if (id_object != OBJID_WINDOW || id_child != CHILDID_SELF || !hwnd) return;
    if (event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_DESTROY &&
        event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE &&
        event != EVENT_OBJECT_PARENTCHANGE &&
        event != EVENT_OBJECT_CLOAKED && event != EVENT_OBJECT_UNCLOAKED) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_watcher_mtx);
    for (LiveWallpaperWatcher* w : g_active_watchers) {
        if (!w || !w->notify_hwnd_) continue;
        bool expected = false;
        if (w->notify_pending_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            PostMessageW(w->notify_hwnd_, w->notify_msg_, 0, 0);
        }
    }
}

} // namespace pulse::desktop::host
