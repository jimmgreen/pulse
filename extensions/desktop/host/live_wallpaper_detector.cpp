// live_wallpaper_detector.cpp - Win32 candidate enumeration & out-of-context WinEvent hook.
#include "host/live_wallpaper_watcher.h"

#include <dwmapi.h>
#include <mutex>
#include <vector>

namespace pulse::desktop::host {
namespace {

uintptr_t ToId(HWND hwnd) noexcept {
    return reinterpret_cast<uintptr_t>(hwnd);
}

bool IsWindowCloaked(HWND hwnd) noexcept {
    BOOL cloaked = FALSE;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return false;
    }
    return cloaked != FALSE;
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
    std::vector<LiveWallpaperCandidate>* out = nullptr;
};

BOOL CALLBACK EnumChildProc(HWND child, LPARAM lparam) noexcept {
    auto* ctx = reinterpret_cast<EnumDescendantCtx*>(lparam);
    if (!ctx || !ctx->out) return FALSE;
    if (ctx->out->size() >= 128) return FALSE;
    ctx->out->push_back(BuildCandidate(child, ctx->ancestor_id, false));
    return TRUE;
}

std::mutex g_watcher_mtx;
std::vector<LiveWallpaperWatcher*> g_active_watchers;

} // namespace

LiveWallpaperDecision ProbeLiveWallpaperFromSystem(
    const mount::DesktopHierarchyInfo& hierarchy,
    int32_t primary_work_w,
    int32_t primary_work_h) noexcept {
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
    candidates.reserve(32);

    if (hierarchy.workerw_wallpaper && IsWindow(hierarchy.workerw_wallpaper)) {
        EnumDescendantCtx ctx{ToId(hierarchy.workerw_wallpaper), &candidates};
        EnumChildWindows(hierarchy.workerw_wallpaper, &EnumChildProc, reinterpret_cast<LPARAM>(&ctx));
    }
    if (hierarchy.progman && IsWindow(hierarchy.progman) &&
        hierarchy.progman != hierarchy.workerw_wallpaper) {
        EnumDescendantCtx ctx{ToId(hierarchy.progman), &candidates};
        EnumChildWindows(hierarchy.progman, &EnumChildProc, reinterpret_cast<LPARAM>(&ctx));
    }

    // Also inspect top-level windows Z-ordered beneath Explorer's desktop icon host
    // (covers engines that position a borderless top-level window behind SHELLDLL_DefView's parent).
    const HWND icon_host = hierarchy.defview_parent ? hierarchy.defview_parent : hierarchy.progman;
    if (icon_host && IsWindow(icon_host)) {
        size_t scanned = 0;
        for (HWND w = GetWindow(icon_host, GW_HWNDNEXT); w && scanned < 48; w = GetWindow(w, GW_HWNDNEXT), ++scanned) {
            if (w == hierarchy.progman || w == hierarchy.workerw_wallpaper) continue;
            candidates.push_back(BuildCandidate(w, 0, true));
        }
    }

    return EvaluateLiveWallpaper(h, candidates);
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
                                   int32_t primary_work_h) noexcept {
    notify_pending_.store(false, std::memory_order_release);
    const LiveWallpaperDecision next = ProbeLiveWallpaperFromSystem(hierarchy, primary_work_w, primary_work_h);
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
