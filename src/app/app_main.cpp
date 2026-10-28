#include "app_diagnostics.h"
#include "partial_redraw.h"
#include "git_ui.h"
#include "mtime_hint_prefs.h"
#include "app_wall_hint.h"
#include "arrival_select.h"
#include "zip_compress_ui.h"
#include "folder_alias.h"
#include "../common/runtime_log.h"
#include "../index/filename_pinyin.h"
#include "../common/windows_compat.h"
#include "../ui/FluentTokens.h"
#include "quick_access.h"
#include "tag_manager.h"
#include "app_hover_preview.h"
#include "app_details_tree.h"
#include "update_stage.h"
#include "tag_ads_sync.h"
#include "app_prompts.h"
#include "vertical_tabs.h"
#include "favorites_bar.h"
#include "tag_view.h"
#include "tag_rules.h"
#include "filter_animation.h"
#include "sidebar_resize.h"
#include "ui_timer_pacer.h"
#include <dwmapi.h>
// app_main.cpp — Pulse UI process entry point, window, input, shot mode.
#include "../ui/ui_compositor.h"
#include "../ui/known_extensions.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/ui_renderer.h"
#include "../ui/preview_grab_cursor.h"
#include "../ui/thumbnail_disk_cache.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/quick_preview_window.h"
#include "app_sidebar_refresh.h"
#include "cloud_sync_probe.h"
#include "device_registry.h"
#include "operation_intent_ui.h"
#include "../ui/typography.h"
#include "../ui/preview_format_catalog.h"
#include "../common/crash_reporter.h"
#include "../common/diagnostics_exporter.h"
#include "../common/localization.h"
#include "pulse_version.h"
#include "../fs/fs_enum.h"
#include "../fs/fs_recycle.h"
#include "../fs/fs_snapshot.h"
#include "../fs/fs_watch.h"
#include "../fs/fs_net_cache.h"
#include "app_model.h"
#include "app_worker.h"
#include "snapshot_patch.h"
#include "session.h"
#include "session_save.h"
#include "window_placement.h"
#include "bitlocker_unlock.h"
#include "drive_verbs.h"
#include "network_connect.h"
#include "context_menu.h"
#include "context_menu_controller.h"
#include "shell_verbs.h"
#include "places.h"
#include "batch_rename.h"
#include "details_meta.h"
#include "context_menu_prefs.h"
#include "app_prefs.h"
#include "startup_launch.h"
#include "entry_sort.h"
#include "drop_staging.h"
#include "folder_sizes_ui.h"
#include "saved_search.h"
#include "search_query.h"
#include "settings_controller.h"
#include "single_instance_coordinator.h"
#include "settings_transfer_ui.h"
#include "window_factory.h"
#include "open_routing.h"
#include "window_identity_ui.h"
#include "tab_transfer.h"
#include "tab_detach.h"
#include "window_merge.h"
#include "reopen_closed.h"
#include "tray_controller.h"
#include "ai_launcher.h"
#include "ai_service.h"
#include "ai_settings_ui.h"
#include "ai_assistant.h"
#include "ai_assistant_chat.h"
#include "ai_assistant_voice.h"
#include "ai_status.h"
#include "ai_tools.h"
#include "ai_tools_ui.h"
#include "global_search_controller.h"
#include "keymap_input.h"
#include "shortcut_page.h"
#include "tab_controller.h"
#include "startup_location.h"
#include "update_checker.h"
#include "app_updates.h"
#include "update_download_cleanup.h"
#include "update_shutdown.h"
#include "link_resolve.h"
#include "../ui/color_picker.h"
#include "../ui/bloom_accent_picker.h"
#include "../ui/folder_picker_dialog.h"
#ifdef PULSE_WITH_SELFTEST
#include "selftest_1b2.h"
#include "../bench/startup_rollback_test.h"
#endif
#include "resource.h"
#include "../index/index_client.h"
#include "../index/network_index.h"
#include "../index/network_agent_client.h"
#include "../index/content_search_client.h"
#include "../ops/ops_manager.h"
#include "../ops/elevated_transfer_client.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include <windows.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <prsht.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include "../ui/ui_motion.h"
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <process.h>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "psapi.lib")
#include "app_internal.h"
#include "app_network_page.h"
#include "cloud_download.h"

namespace {
// Default-program changes (thumbnail "opens with" badges): one shell-level
// registration for SHCNE_ASSOCCHANGED on the main window.
ULONG g_assocNotify = 0;

void RegisterAssocChangeNotify(HWND hwnd) {
    if (g_assocNotify) return;
    PIDLIST_ABSOLUTE desktop = nullptr;
    if (FAILED(SHGetFolderLocation(nullptr, CSIDL_DESKTOP, nullptr, 0, &desktop))) return;
    SHChangeNotifyEntry entry{desktop, TRUE};
    g_assocNotify = SHChangeNotifyRegister(hwnd, SHCNRF_ShellLevel, SHCNE_ASSOCCHANGED,
                                           pulse::WM_ASSOC_CHANGED, 1, &entry);
    CoTaskMemFree(desktop);
}

void UnregisterAssocChangeNotify() {
    if (g_assocNotify) SHChangeNotifyDeregister(g_assocNotify);
    g_assocNotify = 0;
}

// --test-ai-broker (test builds): a --test-instance that still serves AI
// clients, for live MCP tests. Only while LOCALAPPDATA points somewhere else
// than the user's own folder: the pipe name and ai.json follow it, so a test
// never answers on the user's pipe or reads their AI settings.
bool TestAiBroker() {
#ifdef PULSE_WITH_TEST_HOOKS
    bool asked = false;
    for (int i = 1; i < __argc; ++i) asked = asked || wcscmp(__wargv[i], L"--test-ai-broker") == 0;
    if (!asked) return false;
    wchar_t env[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH);
    PWSTR real = nullptr;
    bool elsewhere = false;
    if (n > 0 && n < MAX_PATH && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &real)))
        elsewhere = _wcsicmp(env, real) != 0;
    CoTaskMemFree(real);
    return elsewhere;
#else
    return false;
#endif
}
} // namespace
#include "group_wheel_ui.h"
#include "window_overview_ui.h"
#include "window_switcher_ui.h"
#include <imm.h>
#include "duplicate_scan.h"
#include "shell_tag_menu.h"
#include "shell_tag_com.h"
#include "shell_tag_audit.h"
#include "hang_watch.h"
#include "tray_reveal.h"
#include "default_file_manager.h"
#include "shell_window_sync.h"
#include "host_window.h"
#include "host_routing.h"
#include "startup_trace.h"
#include <commctrl.h>
#include <dbt.h> // WM_DEVICECHANGE / DEV_BROADCAST_HDR
#include "../ui/device_crumbs.h"

using namespace pulse;
namespace pulse {
void DispatchMenuCommand(AppState& s, int cmd); // app_commands.cpp (--menushot PULSE_TEST_MENU_RUN)
constexpr UINT_PTR kTimerDeviceHelpTest = 0xD2B1;  // PULSE_TEST_DEVICE_HELP_MS (test instances)
}

uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

void UpdateProcessMetrics(AppState& s) {
    const ULONGLONG now = GetTickCount64();
    if (s.processSampleTick != 0 && now - s.processSampleTick < 500) return;

    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const uint64_t processTime = FileTimeValue(kernel) + FileTimeValue(user);
        if (s.processSampleTick != 0 && processTime >= s.lastProcessTime100ns) {
            const double wall100ns = static_cast<double>(now - s.processSampleTick) * 10000.0;
            const DWORD processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            s.processCpuPercent = 100.0 * static_cast<double>(processTime - s.lastProcessTime100ns)
                / (wall100ns * processors);
        }
        s.lastProcessTime100ns = processTime;
    }

    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        s.workingSetMb = static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
    }
    s.processSampleTick = now;
}

// Index status only reaches the screen through search views, the palette and
// the settings page (see BuildVm); other notifications need no repaint.
// --view wall (screenshots): large icons shown as a photo wall.
static bool g_shot_wall = false;
// GUI verification for the 收藏栏: shown, with a few favorites if none.
static bool g_shot_favorites_bar = false;
// --shot-toolbar-apps: notepad, calc and mspaint pinned, a separator after notepad.
static bool g_shot_toolbar_apps = false;
// --shot-favorites-hover N: favorite N's ⌄ is hot (-2: the address bar's ★⌄).
static int g_shot_favorites_hover = -1;
// --shot-ai N: made-up AI clients and calls (StageAiShot): 0 busy, 1 waiting, 2 done.
static int g_shot_ai = -1;
// --shot-favorites-drop N: files are dragged over favorite N.
static int g_shot_favorites_drop = -1;
// --shot-favorites-focus N: F6 onto the 收藏栏, then → N times (the focus ring).
static int g_shot_favorites_focus = -1;
// Decorative motion (loading shimmer, folder size skeleton, the pill's ring)
// runs at about 30 fps instead of the display rate (FramePump::ArmPaced).
constexpr double kDecorativeFrameMs = 1000.0 / 30.0;
// While the window is cloaked (another virtual desktop, a shell cloak) frames
// stop; this polls for the uncloak, then repaints once to pick motion up.
constexpr UINT_PTR kTimerCloakPoll = 0x434C;  // 'CL'
constexpr UINT kCloakPollMs = 500;
// Rebuilding the graphics can fail while displays come and go (a monitor
// unplugged): try again instead of waiting for something else to repaint.
constexpr UINT_PTR kTimerGraphicsRetry = 0x4752;  // 'GR'
constexpr UINT kGraphicsRetryMs = 500;
// Frames the pump paints (WM_FRAME_PUMP) are not user activity: counting
// them kept kTimerUi at the display rate for as long as anything moved.
static thread_local bool t_pump_paint = false;
// One frame at a time on a UI thread. Render lays the hosted edits out again
// after Present, and on Windows 10 an IME (Sogou Pinyin) answered that move
// with a COM call whose modal loop dispatched the next WM_FRAME_PUMP or
// WM_PAINT, so a frame started inside the frame still running. Each nested
// frame holds WndProcImpl's whole stack frame, and 47 of them overflowed the
// stack (crash report 2026-10-25). A frame wanted while one runs is put off;
// the outer frame repaints that window once it ends.
static thread_local int t_render_depth = 0;
static thread_local std::vector<HWND> t_deferred_frames;
static void DeferFrame(HWND hwnd) {
    if (hwnd && std::find(t_deferred_frames.begin(), t_deferred_frames.end(), hwnd) == t_deferred_frames.end())
        t_deferred_frames.push_back(hwnd);
}
class FrameScope {
public:
    FrameScope() { ++t_render_depth; }
    ~FrameScope() {
        if (--t_render_depth != 0) return;
        std::vector<HWND> windows;
        windows.swap(t_deferred_frames);
        for (HWND hwnd : windows)
            if (IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
    }
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;
};
static bool TestNestedFrames() {
    static const bool on = [] {
        wchar_t v[4]{};
        return GetEnvironmentVariableW(L"PULSE_TEST_NESTED_FRAME", v, ARRAYSIZE(v)) == 1 && v[0] == L'1';
    }();
    return on;
}
// Keeps frames coming while anything moves: the display rate, or about 30 fps
// when only decorative motion runs. Render and the UI timer both decide here.
static void ArmMotionFrames(AppState& s, bool app_motion) {
    if (!app_motion && s.renderer.DecorativeMotionOnly()) s.framePump.ArmPaced(kDecorativeFrameMs);
    else s.framePump.Arm();
}
static bool WindowCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

static bool IndexStatusVisible(AppState& s) {
    if (s.paletteSearching) return true;
    bool visible = false;
    ForEachPane(s, [&](app::Pane& pane) {
        const app::Tab* tab = pane.ActiveTab();
        std::wstring kind;
        if (tab && app::ParsePulsePath(tab->current_path, &kind, nullptr) &&
            (kind == L"search" || kind == L"saved-search" || kind == L"settings"))
            visible = true;
    });
    return visible;
}

// Landing: once the dropped rows were drawn, send their screen rects to the
// waiting drag cards and reveal the rows as the cards arrive.
static void TickDragLanding(AppState& s) {
    auto& l = s.dragLanding;
    if (l.names.empty()) return;
    const uint64_t now = ui::motion::NowMs();
    if (l.reveal_at) {
        if (now > l.reveal_at + 200) l = {};
        return;
    }
    const auto& rects = s.renderer.LandingRects();
    if (!rects.empty()) {
        std::vector<RECT> screen;
        for (const auto& r : rects) {
            POINT a{std::lround(r.left), std::lround(r.top)};
            POINT b{std::lround(r.right), std::lround(r.bottom)};
            ClientToScreen(s.hwnd, &a);
            ClientToScreen(s.hwnd, &b);
            screen.push_back(RECT{a.x, a.y, b.x, b.y});
        }
        if (ui::LandDragStack(screen)) l.reveal_at = now + 240;
        else l = {};   // the cards are gone: just show the rows
        InvalidateRect(s.hwnd, nullptr, FALSE);
    } else if (now - l.since > 750 || (!ui::DragLandingAwaiting() && now - l.since > 150)) {
        l = {};
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

// cubic-bezier(.3,.6,.2,1), the motion v2 reveal curve.
static float ThemeRevealEase(float t) {
    constexpr float x1 = 0.3f, y1 = 0.6f, x2 = 0.2f, y2 = 1.0f;
    auto bez = [](float a, float b, float u) {
        const float v = 1.0f - u;
        return 3.0f * v * v * u * a + 3.0f * v * u * u * b + u * u * u;
    };
    float lo = 0.0f, hi = 1.0f, u = t;
    for (int i = 0; i < 24; ++i) {
        u = (lo + hi) * 0.5f;
        if (bez(x1, x2, u) < t) lo = u; else hi = u;
    }
    return bez(y1, y2, u);
}

// The old theme's frame, drawn everywhere outside the growing circle.
static void DrawThemeReveal(AppState& s, ID2D1DeviceContext* dc, const D2D1_RECT_F& rect) {
    auto& r = s.themeReveal;
    if (!r.start) return;
    ID2D1Bitmap1* old = s.compositor.HeldFrame();
    constexpr float kRevealMs = 520.0f;
    const float t = static_cast<float>(ui::motion::NowMs() - r.start) / kRevealMs;
    const D2D1_SIZE_U px = old ? old->GetPixelSize() : D2D1_SIZE_U{};
    auto stop = [&] {
        r = {};
        s.compositor.ReleaseHeldFrame();
    };
    if (!old || t >= 1.0f || px.width != static_cast<UINT32>(rect.right) ||
        px.height != static_cast<UINT32>(rect.bottom)) {
        stop();
        return;
    }
    const float far_x = (std::max)(r.x, rect.right - r.x);
    const float far_y = (std::max)(r.y, rect.bottom - r.y);
    const float radius = std::sqrt(far_x * far_x + far_y * far_y) * ThemeRevealEase((std::max)(t, 0.0f)) + 0.5f;
    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    ID2D1RectangleGeometry* box = nullptr;
    ID2D1EllipseGeometry* hole = nullptr;
    ID2D1GeometryGroup* mask = nullptr;
    ID2D1BitmapBrush* brush = nullptr;
    bool ok = factory &&
        SUCCEEDED(factory->CreateRectangleGeometry(D2D1::RectF(-1.0f, -1.0f, rect.right + 1.0f, rect.bottom + 1.0f), &box)) &&
        SUCCEEDED(factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(r.x, r.y), radius, radius), &hole)) &&
        SUCCEEDED(dc->CreateBitmapBrush(old, &brush));
    if (ok) {
        ID2D1Geometry* parts[2] = {box, hole};
        ok = SUCCEEDED(factory->CreateGeometryGroup(D2D1_FILL_MODE_ALTERNATE, parts, 2, &mask));
    }
    if (ok) {
        // Copy, not blend: the frames may be translucent (backdrop), and the
        // old pixels must replace the new ones outside the circle.
        const D2D1_PRIMITIVE_BLEND blend = dc->GetPrimitiveBlend();
        dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        dc->FillGeometry(mask, brush);
        dc->SetPrimitiveBlend(blend);
    }
    for (IUnknown* p : {static_cast<IUnknown*>(mask), static_cast<IUnknown*>(brush),
                        static_cast<IUnknown*>(hole), static_cast<IUnknown*>(box),
                        static_cast<IUnknown*>(factory)})
        if (p) p->Release();
    if (!ok) stop();
}

// The single selected icon-grid item shows its whole name under its cell;
// when that runs past the bottom of the list, glide the list up once, right
// after the selection lands on it (never later: scrolling stays the user's).
static void RevealExpandedName(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    const int sel = tab && tab->SelectedCount() == 1 ? tab->selected_index : -1;
    if (tab != s.revealTab || sel != s.revealIndex) {
        s.revealTab = tab;
        s.revealIndex = sel;
        s.revealSince = GetTickCount64();
        s.revealDone = sel < 0;
        if (s.selectScrollRoom > 0.0f) {
            s.selectScrollRoom = 0.0f;
            ClampScroll(s);
        }
    }
    if (s.revealDone || !tab) return;
    if (s.renameIndex >= 0 || s.marqueeActive || GetTickCount64() - s.revealSince > 600) {
        s.revealDone = true;
        return;
    }
    int shown = -1;
    const float over = s.renderer.ExpandedNameOverflow(&shown);
    if (over <= 0.5f || shown != sel) return;
    s.revealDone = true;
    // Measured at the current position; a glide already under way may go further.
    float want = tab->scroll_y + over + 4.0f * s.scale;
    if (s.scrollAnimating && !s.scrollHorizontal) want = std::max(want, s.scrollTargetY);
    const float most = MaxScrollForActivePane(s);
    if (want > most) s.selectScrollRoom += want - most;
    if (!ui::motion::SystemAnimationsEnabled()) {
        tab->scroll_y = want;
        s.scrollTargetY = want;
        ClampScroll(s);
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    s.scrollTargetY = want;
    s.scrollHorizontal = false;
    s.scrollLastUpdateTime = std::chrono::steady_clock::now();
    s.scrollAnimating = true;
    if (s.framePump.Running()) s.framePump.Arm();
    else if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

// WM_PAINT's partial damage for the Render it calls (null: a whole frame).
static thread_local const RECT* t_paint_damage = nullptr;

// Motion the app runs itself (scrolling, layout, theme): it moves pixels the
// partial-redraw damage does not know about.
static bool AppMotion(const AppState& s) {
    return s.scrollAnimating || s.marqueeAutoScroll || s.themeReveal.start || s.paneLayoutMotion.start ||
           s.trayAnimating;
}

void Render(AppState& s) {
    if (t_render_depth > 0) {
        DeferFrame(s.hwnd);
        return;
    }
    FrameScope frame_scope;
    pulse::app::startup_trace::Mark("frame_begin");
    auto t0 = std::chrono::steady_clock::now();

    if (s.compositor.NeedsRecovery()) {
        if (!s.compositor.Recover()) {
            if (s.hwnd) SetTimer(s.hwnd, kTimerGraphicsRetry, kGraphicsRetryMs, nullptr);
            return;
        }
        if (s.hwnd) KillTimer(s.hwnd, kTimerGraphicsRetry);
        s.compositor.RecreateTextFormats(s.scale);
        s.renderer.SetCompositor(&s.compositor);
        s.renderer.SetScale(s.scale);
    }

    if (s.hwnd) {
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        int w = rc.right;
        int h = rc.bottom;
        if (w != s.compositor.Width() || h != s.compositor.Height()) {
            s.compositor.Resize(w, h);
            // No target to draw into: the repaint it asked for rebuilds first.
            if (s.compositor.NeedsRecovery()) return;
        }
    }

    ID2D1DeviceContext* dc = s.compositor.Dc();
    const auto draw_start = std::chrono::steady_clock::now();
    dc->BeginDraw();

    bool hc = s.shot_high_contrast || ui::IsHighContrast();
    ui::Theme theme = hc ? ui::MakeHighContrastTheme() : ui::MakeTheme(s.darkMode, s.accentColor);

    // The staging tray steps once per frame painted, so motion a click or a
    // drag-over starts begins with this frame instead of the next UI tick;
    // the pump keeps the frames coming while it moves.
    const bool tray_moving = TickTrayDeck(s);
    s.trayAnimating = tray_moving && s.framePump.Running();
    const bool app_motion = AppMotion(s);  // before this frame's scroll step
    if (s.scrollAnimating) UpdateSmoothScroll(s);
    UpdateMarqueeFrame(s);
    UpdateProcessMetrics(s);
    ui::WindowViewModel vm = BuildVm(s);
    pulse::app::startup_trace::Mark("frame_vm");
    vm.backdrop_enabled = !hc && s.compositor.UsesTransparentComposition() && s.backdropActive;
    vm.pane.hover_index = s.hoverRow;

    D2D1_RECT_F rect = D2D1::RectF(0, 0, (float)s.compositor.Width(), (float)s.compositor.Height());
    ui::HoverPreviewView hover_card;
    FillHoverPreviewView(s, vm, hover_card);
    if (hover_card.visible) vm.tooltip_text.clear();  // the card already names the file
    // Partial redraw: only a quiet window repaints just the damage; anything
    // floating or moving besides the hover plates makes it a whole frame.
    RECT damage_px{};
    bool partial = t_paint_damage && !app_motion && s.compositor.CanPresentPartial() && !s.shot.active &&
                   !hover_card.visible && !hover_card.dwell && !s.themeReveal.start &&
                   s.renameIndex < 0 && !s.notification_toast.IsVisible() &&
                   s.renderer.PartialRedrawSafe(vm);
    if (partial) {
        RECT client{0, 0, s.compositor.Width(), s.compositor.Height()};
        partial = IntersectRect(&damage_px, t_paint_damage, &client) && !EqualRect(&damage_px, &client);
    }
    const D2D1_RECT_F damage = D2D1::RectF(static_cast<float>(damage_px.left), static_cast<float>(damage_px.top),
                                           static_cast<float>(damage_px.right), static_cast<float>(damage_px.bottom));
    s.renderer.SetFrameDamage(partial ? &damage : nullptr);
    if (partial) dc->PushAxisAlignedClip(damage, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    s.renderer.Render(vm, rect, theme);
    pulse::app::startup_trace::Mark("frame_scene");
    TickDragLanding(s);
    s.renderer.DrawHoverPreview(hover_card, rect, theme, hc);  // also the dwell line, and stops a video
    s.renderer.DrawStarFlight(theme);
    AfterHoverPreviewDraw(s);
    s.notification_toast.Draw(s.compositor, theme, s.scale, hc);
    DrawThemeReveal(s, dc, rect);
    if (partial) dc->PopAxisAlignedClip();
    s.renderer.SetFrameDamage(nullptr);
    pulse::app::startup_trace::Mark("frame_overlays");

    const HRESULT end_hr = dc->EndDraw();
    pulse::app::startup_trace::Mark("frame_drawn");
    const auto draw_end = std::chrono::steady_clock::now();
    s.timing.draw_ms = std::chrono::duration<double, std::milli>(draw_end - draw_start).count();
    if (end_hr == D2DERR_RECREATE_TARGET || end_hr == DXGI_ERROR_DEVICE_REMOVED ||
        end_hr == DXGI_ERROR_DEVICE_RESET || end_hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        s.compositor.NotifyDeviceLost(end_hr);
        return;
    }
    const auto present_start = std::chrono::steady_clock::now();
    wchar_t hidden_frame[4]{};
    const bool hidden_capture = s.shot.active &&
        GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", hidden_frame, ARRAYSIZE(hidden_frame)) == 1 &&
        hidden_frame[0] == L'1';
    // An occluded flip chain advances to an unpainted buffer after Present.
    if (!hidden_capture) s.compositor.Present(partial ? &damage_px : nullptr);
    else s.compositor.ForceFullFrame();
    pulse::app::startup_trace::Mark("frame_present");
    const auto present_end = std::chrono::steady_clock::now();
    // Test instances report each window's first presented frame to the
    // debugger output on the QPC clock, so a harness can time Ctrl+N to the
    // new window's first frame (multi-window M4f).
    if (s.isolatedTest && !s.first_present_reported && !hidden_capture) {
        s.first_present_reported = true;
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        wchar_t line[96]{};
        swprintf_s(line, L"PulseFirstPresent hwnd=%p qpc=%lld\n", static_cast<void*>(s.hwnd), qpc.QuadPart);
        OutputDebugStringW(line);
    }
    s.timing.present_ms = std::chrono::duration<double, std::milli>(present_end - present_start).count();

    if (s.renameIndex >= 0) LayoutRenameOverlay(s);
    RevealExpandedName(s);
    if (!s.tagRenameId.empty()) LayoutTagRenameOverlay(s);
    if (s.addressEditing) LayoutAddressEditor(s);
    if (s.filterEditing) LayoutFilterEditor(s);
    AiAssistantLayout(s);
    SyncShellWindows(s);
    // Test instances: PULSE_TEST_NESTED_FRAME=1 paints again from inside the
    // frame, the way an IME's modal loop did (stack-overflow regression check).
    if (s.isolatedTest && TestNestedFrames() && s.hwnd)
        RedrawWindow(s.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

    if (s.shot.active && !s.timing.first_frame_recorded) {
        s.timing.first_frame_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - s.shot.start).count();
        s.timing.first_frame_recorded = true;
    }

    auto t1 = std::chrono::steady_clock::now();
    s.lastFrameMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    // FPS = frames presented per second over a sliding ~1 s window of
    // continuous rendering. Pulse paints on demand: 1/dt of two adjacent
    // paints reported the idle gap as "1 FPS" and back-to-back paints as
    // thousands. An idle gap restarts the window instead of being averaged in,
    // and the value only updates once the window spans enough frames.
    {
        using secs = std::chrono::duration<double>;
        constexpr double kIdleGap = 0.25, kWindow = 1.0, kMinSpan = 0.05;
        if (!s.fpsWindow.empty() && secs(t1 - s.fpsWindow.back()).count() > kIdleGap)
            s.fpsWindow.clear();
        s.fpsWindow.push_back(t1);
        while (s.fpsWindow.size() > 2 && secs(t1 - s.fpsWindow.front()).count() > kWindow)
            s.fpsWindow.pop_front();
        const double span = secs(t1 - s.fpsWindow.front()).count();
        if (s.fpsWindow.size() >= 3 && span >= kMinSpan)
            s.lastFps = static_cast<double>(s.fpsWindow.size() - 1) / span;
    }
    s.lastFrameTime = t1;
    // Motion started or still running: the next frames follow the display
    // clock instead of the 16 ms UI timer (see WM_FRAME_PUMP).
    if (s.framePump.Running()) {
        const bool motion = AppMotion(s);
        if (s.renderer.TickMotion(GetTickCount64()) || motion) ArmMotionFrames(s, motion);
    }
    if (const UINT caret = s.renderer.CaretBlinkDueMs()) SetTimer(s.hwnd, kTimerDrawnCaret, caret, nullptr);
    else KillTimer(s.hwnd, kTimerDrawnCaret);
}

// The session used to be written only in WM_DESTROY, so a Windows shutdown,
// logoff, crash or forced exit lost every layout change of that run.
static bool SessionWritable(const AppState& s) {
    return s.startupComplete && !s.shot.active && !s.menushot && (!s.isolatedTest || s.isolatedTestPersist) &&
        !ShellTagHeadlessLaunch();
}

// One window of the session (v8, M4d).
static app::WindowSessionSnapshot CaptureSessionWindow(AppState& w) {
    app::WindowSessionSnapshot snap;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (GetWindowPlacement(w.hwnd, &wp)) {
        snap.window_rect = wp.rcNormalPosition;
        snap.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
    }
    snap.layout = CaptureWindowLayout(w);
    RememberLayoutFocus(w);
    snap.active_layout_tab = static_cast<int>(w.window_tabs.active);
    for (const auto& group : w.window_tabs.tab_groups)
        snap.tab_groups.push_back({group.id, group.name, group.color_rgb, group.collapsed});
    for (const auto& layout : w.window_tabs.items)
        if (layout) snap.layout_tabs.push_back(app::CaptureLayoutTab(*layout));
    snap.identity = w.identity;
    return snap;
}

// The single-window fields describe the most recently used window, which is
// also windows[0]: versions that read one window restore that one.
static app::SessionSnapshot CaptureWindowSession(AppState& requested, HWND requested_hwnd) {
    AppState& s = app::CurrentWindow(requested);
    const HWND hwnd = &s == &requested || !s.hwnd ? requested_hwnd : s.hwnd;
    app::SessionSnapshot snap;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (GetWindowPlacement(hwnd, &wp)) {
        snap.window_rect = wp.rcNormalPosition;
        snap.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
    }
    snap.dark = s.darkMode;
    app::Tab* tab = ActiveTab(s);
    if (tab) snap.active_path = tab->current_path;
    RememberLayoutFocus(s);
    snap.active_layout_tab = static_cast<int>(s.window_tabs.active);
    for (const auto& group : s.window_tabs.tab_groups)
        snap.tab_groups.push_back({group.id, group.name, group.color_rgb, group.collapsed});
    for (const auto& layout : s.window_tabs.items)
        snap.layout_tabs.push_back(app::CaptureLayoutTab(*layout));
    snap.tray = s.tray;
    snap.undo_json = s.ops.UndoToJson();
    snap.sidebar_collapsed = static_cast<int>(s.sidebarCollapsedMask);
    snap.sidebar_hidden = static_cast<int>(s.sidebarHiddenMask);
    snap.sidebar_order = app::NormalizeSidebarOrder(s.sidebarOrder);
    snap.quick_access_hidden = static_cast<int>(s.sidebarQuickAccessHiddenMask);
    snap.starred_expanded = s.starredExpanded;
    snap.details_panel = s.showDetailsPanel;
    snap.details_preview_only = s.detailsPreviewOnly;
    snap.details_preview = s.detailsPreviewEnabled;
    snap.details_panel_width = static_cast<int>(std::lround(s.detailsPanelWidth));
    // Session v8 (M4d): every window, most recent first, then the windows of
    // the last session that have not been opened yet (a launch into the tray).
    snap.windows.push_back(CaptureSessionWindow(s));
    snap.windows.front().window_rect = snap.window_rect;
    snap.windows.front().maximized = snap.maximized;
    app::ForEachWindowSharingCore(s, [&](AppState& window) {
        if (&window != &s && window.hwnd && !window.window_tabs.items.empty())
            snap.windows.push_back(CaptureSessionWindow(window));
    });
    for (const auto& pending : s.core.pending_session_windows) snap.windows.push_back(pending);
    snap.focused_window = 0;
    // M7f: closed windows survive a restart (the writer keeps the newest).
    for (const auto& closed : s.core.closed_windows.Items())
        snap.closed_windows.push_back(app::ClosedWindowToSession(closed));
    return snap;
}

// What reopening a closed window brings back (M4c): its tabs, groups and place
// (M4d: and its sidebar and details pane).
static app::ClosedWindow CaptureClosedWindow(AppState& s, HWND hwnd) {
    app::ClosedWindow closed;
    WINDOWPLACEMENT wp{ sizeof(wp) };
    if (GetWindowPlacement(hwnd, &wp)) {
        closed.rect = app::RestoredWindowRect(wp.rcNormalPosition);
        closed.maximized = wp.showCmd == SW_SHOWMAXIMIZED || IsZoomed(hwnd);
    }
    RememberLayoutFocus(s);
    closed.active_layout_tab = static_cast<int>(s.window_tabs.active);
    for (const auto& group : s.window_tabs.tab_groups)
        closed.tab_groups.push_back({group.id, group.name, group.color_rgb, group.collapsed});
    for (const auto& layout : s.window_tabs.items)
        if (layout) closed.layout_tabs.push_back(app::CaptureLayoutTab(*layout));
    if (s.window_tabs.active < s.window_tabs.items.size() && s.window_tabs.items[s.window_tabs.active])
        closed.name = app::LayoutTabTitle(*s.window_tabs.items[s.window_tabs.active]);
    closed.layout = CaptureWindowLayout(s);
    closed.identity = s.identity;
    return closed;
}

// Rebuilds the window's tabs from snapshots: session restore, and a reopened
// window (M4c).
static void RestoreTabsFromSnapshots(AppState& s, const std::vector<app::LayoutTabSnapshot>& layout_tabs,
                                     const std::vector<app::GroupSessionSnapshot>& groups, int active) {
    s.pane = nullptr;
    s.targetPane = nullptr;
    AppState* const state = &s;
    app::RestoreWindowTabs(
        s.window_tabs, layout_tabs, groups, active,
        [state](app::Tab& tab, const std::wstring& path) {
            WarmupUnc(*state, path);
            StartLoadingPath(*state, tab, path, PathLoadReason::RestoreSession);
        });
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
        app::RebuildLayoutRoot(*s.window_tabs.items[i]);
        if (i < layout_tabs.size() && s.window_tabs.items[i]->root && !layout_tabs[i].split_ratios.empty())
            app::ApplySplitRatios(*s.window_tabs.items[i]->root, layout_tabs[i].split_ratios);
    }
    BindCurrentLayout(s);
}

// Ctrl+Shift+M (M4e, plan §8.4; M5c): the active tab itself moves to a new
// window next to this one, keeping its panes, splits, pin, history, selection
// and scroll position (it leaves its group); its folders show at once. A
// window with one tab keeps it: moving it would only replace the window.
static void MoveTabToNewWindow(AppState& s) {
    auto& tabs = s.window_tabs;
    const size_t index = tabs.active;
    if (tabs.items.size() < 2 || index >= tabs.items.size() || !tabs.items[index]) return;
    WindowInit init;  // no place: OpenPulseWindow cascades from `s`
    if (const app::Tab* view = ActiveTab(s)) init.path = view->current_path;
    init.adopt_from = &s;
    init.adopt_index = index;
    OpenPulseWindow(s, init);
}

// WM_CREATE of a window opened by MoveTabToNewWindow: the tab moves in and
// replaces the placeholder tab, which has loaded nothing. False leaves the
// window to open its start folder instead.
static bool AdoptMovedTab(AppState& s) {
    AppState* const from = s.window_adopt_from;
    s.window_adopt_from = nullptr;
    const app::LayoutTab* placeholder = s.window_tabs.Active();
    if (!from || !placeholder || !MoveLayoutTab(*from, s.window_adopt_index, s, 0)) return false;
    for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
        if (s.window_tabs.items[i].get() != placeholder) continue;
        s.window_tabs.CloseTab(i);
        break;
    }
    BindCurrentLayout(s);
    return true;
}

// Reopens the most recently closed window next to `from` (M4c).
static void ReopenClosedWindow(AppState& from) {
    auto closed = from.core.closed_windows.TakeLatest();
    if (!closed) return;
    DismissReopenToasts(from, WM_REOPEN_CLOSED_WINDOW);  // M7f: it came back
    WindowInit init;
    init.restore = std::move(closed);
    OpenPulseWindow(from, init);
}

// Writes session.json and app.json when they differ from the last write (or always
// when forced). Both writes are atomic, so a kill mid-save keeps the old file.
static bool SaveWindowSession(AppState& s, HWND hwnd, bool force) {
    if (!SessionWritable(s) || s.appPrefs.load_failed) return false;
    auto json = app::SessionToJson(CaptureWindowSession(s, hwnd));
    const bool session_saved = app::SaveChangedSession(
        json, s.sessionSavedJson, force, app::WriteSessionJson);
    auto prefs = s.appPrefs.ToJson();
    const bool prefs_saved = app::SaveChangedSession(
        prefs, s.prefsSavedJson, force,
        [&s](const std::wstring&) { return s.appPrefs.Save(); });
    return session_saved && prefs_saved;
}

bool pulse::PrepareSessionForUpdate(AppState& s) {
    s.updateSessionPrepared = SaveWindowSession(s, s.hwnd, false);
    return s.updateSessionPrepared;
}

// System broadcasts (host_routing.h, multi-window M3c), handled once for the
// process: the shared part runs once, the window part for every window that
// shares `s`'s core (only `s` when it is not registered).
// The windows closed since the last one was last used close along with it
// (closed_windows.h): with "restore last tabs" they come back with it, from
// the tray (WM_ACTIVATE opens pending_session_windows) or at the next launch
// (CaptureWindowSession saves them as open windows).
static void KeepWindowsClosedAlong(AppState& s) {
    if (!app::RestoresLastTabs(s.appPrefs)) return;
    const uint64_t since = app::LastUseBeforeClose(s.last_input_tick, s.previous_use_tick, GetTickCount64());
    auto along = s.core.closed_windows.TakeClosedSince(since);
    if (along.empty()) return;
    DismissReopenToasts(s, WM_REOPEN_CLOSED_WINDOW);
    // Most recently closed first, like the session's windows.
    for (auto it = along.rbegin(); it != along.rend(); ++it)
        s.core.pending_session_windows.push_back(app::ClosedWindowToSession(*it).window);
}

// Close button, Alt+F4, WM_CLOSE (plan §8.1). One of several windows is
// destroyed and takes only its own state along; the last window hides to the
// tray when Pulse keeps running in the background, otherwise it exits.
static void CloseMainWindow(AppState& s) {
    const HWND hwnd = s.hwnd;
    if (!s.core.windows.IsLastWindow(hwnd)) {
        // Pinned tabs outlive their window: they join the window used next.
        if (AppState* next = s.core.windows.MostRecentExcept(hwnd)) MovePinnedTabsInto(s, *next);
        DestroyWindow(hwnd);
        return;
    }
    SuspendContentSearches(s);
    s.globalSearchWindow.Hide();
    KeepWindowsClosedAlong(s);
    if (s.appPrefs.KeepsRunningInBackground()) HideMainWindowToTray(s);
    else DestroyWindow(hwnd);
}

// Exit steps report their milliseconds in runtime events (numbers only), so a
// slow exit on a user's machine names the step that held it.
class ExitTimer {
public:
    ExitTimer() noexcept { QueryPerformanceFrequency(&freq_); QueryPerformanceCounter(&last_); }
    uint64_t Lap() noexcept {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const uint64_t ms = static_cast<uint64_t>((now.QuadPart - last_.QuadPart) * 1000 / freq_.QuadPart);
        last_ = now;
        return ms;
    }
private:
    LARGE_INTEGER freq_{};
    LARGE_INTEGER last_{};
};
static ULONGLONG g_exitStarted = 0;  // GetTickCount64 when the last window began to close

// After the hand-off nothing left matters to the user or to a new Pulse, so a
// service, destructor or DLL that hangs from there must not keep a process
// without windows alive. Saved files are replaced atomically or journaled;
// ending here loses at most an update in flight, never a file.
constexpr DWORD kExitWatchdogMs = 2000;
static SRWLOCK g_exitLogLock = SRWLOCK_INIT;
static bool g_exitLogAlive = true;

static void StartExitWatchdog() {
    // Registered after the runtime log started, so this runs before its
    // logger is destroyed; past that point the watchdog ends without a line.
    std::atexit([] {
        AcquireSRWLockExclusive(&g_exitLogLock);
        g_exitLogAlive = false;
        ReleaseSRWLockExclusive(&g_exitLogLock);
    });
    try {
        std::thread([] {
            Sleep(kExitWatchdogMs);
            AcquireSRWLockExclusive(&g_exitLogLock);  // held until the process ends
            if (g_exitLogAlive) {
                diagnostics::runtime::Event("exit_watchdog", {{"ms", kExitWatchdogMs},
                    {"since_close", GetTickCount64() - g_exitStarted}}, diagnostics::runtime::Level::Warning);
                diagnostics::runtime::Flush(500);
            }
            TerminateProcess(GetCurrentProcess(), 0);
        }).detach();
    } catch (...) {}
}

// The last window's first steps on the way out: what the user sees and what a
// new Pulse would depend on. The tray icon goes, saved state is written, file
// operations stop and leave their recovery journal, and only then is the
// single-instance claim released, so a Pulse started now reads finished files
// instead of being forwarded to this one. Stopping the services comes after.
static void HandOffAtExit(AppState& s, HWND hwnd) {
    g_exitStarted = GetTickCount64();
    ExitTimer timer;
    s.tray_controller.Detach();
    const uint64_t tray_ms = timer.Lap();
    // Closing the search window may still record its size in the preferences.
    ShutdownGlobalSearch(s);
    const uint64_t search_ms = timer.Lap();
    // Visual-regression runs must never overwrite the user's real
    // window, path, tray, or undo session.
    if (s.startupComplete && !s.shot.active && !s.menushot && !s.isolatedTest) {
        // A hidden tag-only launch must not overwrite the real
        // window/tab session (SessionWritable); tags and places still save below.
        // ExitPulse saved every window before closing them (M4d);
        // by now this is the only one left.
        if (!s.updateSessionPrepared && !s.core.exiting) SaveWindowSession(s, hwnd, true);
        s.places.Save();
        s.ctxMenuPrefs.Save();
        if (!s.updateSessionPrepared) s.appPrefs.Save();
    }
    s.searchHistoryWriter.Flush();
    const uint64_t save_ms = timer.Lap();
    // AI calls end before the operations they could start (cancelled, pipe closed).
    pulse::app::ai::StopAiService();
    s.ops.Stop();
    ops::ShutdownElevatedTransferHelper();
    const uint64_t ops_ms = timer.Lap();
    // Sweeps every stage, so it must finish before another Pulse can make one.
    app::SweepDropStages(app::DropStageRoot(), true);
    const uint64_t sweep_ms = timer.Lap();
    s.single_instance.Release();
    StartExitWatchdog();
    diagnostics::runtime::Event("exit_handoff", {{"tray", tray_ms}, {"search", search_ms},
        {"save", save_ms}, {"ops", ops_ms}, {"sweep", sweep_ms}, {"release", timer.Lap()}});
}

// Tray "Exit" and the palette's "Exit Pulse": the session is saved while every
// window is still open (v8, M4d), then every window closes, the most recent
// one last so its WM_DESTROY stops the services (it skips the session write).
// `s` may be gone once its window is destroyed; only the snapshot is used.
static void ExitPulse(AppState& s) {
    if (!s.updateSessionPrepared) SaveWindowSession(s, s.hwnd, true);
    s.core.exiting = true;  // these closes are not undoable (closed_windows)
    const HWND self = s.hwnd;
    for (HWND window : s.core.windows.ExitOrder())
        if (window != self && IsWindow(window)) DestroyWindow(window);
    if (self && IsWindow(self)) DestroyWindow(self);
}

static LRESULT HandleSystemBroadcast(AppState& s, app::SystemBroadcast kind, WPARAM wParam) {
    switch (kind) {
    case app::SystemBroadcast::Theme:
        if (s.themeOverride == ui::ThemeMode::Auto) s.darkMode = ui::ShouldUseDarkMode(s.themeOverride);
        app::ForEachWindowSharingCore(s, [](AppState& window) {
            if (window.appPrefs.accent_rgb.empty()) ApplyAccentFromPrefs(window, true);
            ApplyAppWindowChrome(window);
            InvalidateRect(window.hwnd, nullptr, FALSE);
        });
        return 0;
    case app::SystemBroadcast::Volumes:
        // The sidebar model is built once, so rebuild it here instead of
        // waiting for a language switch or a restart.
        RefreshSidebarModel(s);
        app::ForEachWindowSharingCore(s, [](AppState& window) { InvalidateRect(window.hwnd, nullptr, FALSE); });
        return 0;
    case app::SystemBroadcast::Devices:
        // Listed on the device thread after 500 ms of quiet (device_registry.h).
        app::DeviceRegistry::Instance().OnDevicesBroadcast();
        return 0;
    case app::SystemBroadcast::QueryEndSession:
        // Shutdown/logoff ends the process without WM_DESTROY; store the layout
        // now. session.json holds every window (v8), so it is saved once.
        SaveWindowSession(s, s.hwnd, true);
        return TRUE;
    case app::SystemBroadcast::EndSession:
        if (s.startupComplete && wParam && !s.shot.active && !s.menushot && !s.isolatedTest) {
            SaveWindowSession(s, s.hwnd, true);
            s.places.Save();
            s.ctxMenuPrefs.Save();
        }
        return 0;
    case app::SystemBroadcast::None:
        break;
    }
    return 0;
}

// Checked from the UI timer; skipped while a mouse drag (splitter, sidebar,
// column, window move) is in progress so a half-finished resize is not stored.
constexpr ULONGLONG kSessionAutosaveMs = 3000;
// Without input, paint or a dirty tick in any window since the last pass only
// background work can have changed the session; this slower pass catches it.
constexpr ULONGLONG kSessionAutosaveQuietMs = 60000;
static void TickSessionAutosave(AppState& s, HWND hwnd, ULONGLONG now) {
    if (now < s.sessionAutosaveCheck) return;
    s.sessionAutosaveCheck = now + kSessionAutosaveMs;
    // A drag (splitter, sidebar, column, system move/size loop) holds mouse capture.
    // GetKeyState is not used: its queued button state can stay "down" after a click
    // whose button-up a drag-detect loop consumed, which silently stopped autosave.
    if (GetCapture() != nullptr || s.sidebarResizing) return;
    // Capturing and serializing the session (~300 KB with the undo history)
    // every 3 s kept a quiet window busy for nothing.
    if (s.core.sessionActivity < s.core.sessionAutosaveFull &&
        now - s.core.sessionAutosaveFull < kSessionAutosaveQuietMs) {
        return;
    }
    s.core.sessionAutosaveFull = now;
    SaveWindowSession(s, hwnd, false);
}

// kTimerUi drives animations and light polling. Each window's pacer
// (AppState::uiTimerPacer) keeps the display rate while anything changes and
// polls gently once that window is quiet, minimized or hidden to the tray
// (see ui_timer_pacer.h).
static void SyncUiTimerRate(AppState& s, HWND hwnd, bool visible) {
    UINT period = 0;
    if (s.uiTimerPacer.Update(visible, GetTickCount64(), period))
        SetTimer(hwnd, kTimerUi, period, nullptr);
}

// Input, painting and window-state changes can start motion or schedule
// timer work (hover delays, slow-click rename, search debounce), so each
// restores the fast period before the next tick is due.
static bool IsUiActivityMessage(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
           (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
           (msg >= WM_NCMOUSEMOVE && msg <= WM_NCXBUTTONDBLCLK) ||
           msg == WM_MOUSELEAVE || msg == WM_NCMOUSELEAVE || msg == WM_PAINT ||
           msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ACTIVATE ||
           msg == WM_SIZE || msg == WM_CAPTURECHANGED || msg == WM_DPICHANGED;
}

static void NoteUiActivity(AppState& s, HWND hwnd, bool visible) {
    const ULONGLONG now = GetTickCount64();
    s.uiTimerPacer.NoteActivity(now);
    s.core.sessionActivity = now;
    SyncUiTimerRate(s, hwnd, visible);
}

// The second launch waits for this reply, and opening a folder can touch the
// disk: a drive spinning up, a share that does not answer (~3 s), a BitLocker
// prompt. Run inside WM_COPYDATA, that ran out the sender's time and it
// reported “无法确认运行中的 Pulse 已收到此请求” although the folder opened a
// moment later. So the request is accepted now and opened from the queue.
static void QueueExternalOpen(AppState& s, std::wstring path) {
    auto request = std::make_unique<std::wstring>(std::move(path));
    const HWND host = s.core.host.Hwnd();
    if (host && PostMessageW(host, WM_OPEN_EXTERNAL, 0, reinterpret_cast<LPARAM>(request.get()))) {
        request.release();
        return;
    }
    OpenExternalFolder(s, *request);
}

// A second launch or the Explorer tag verb reached the host window (the
// single-instance endpoint, host_window.h); `s` is the window that handles it.
static BOOL HandleInstanceCopyData(AppState& s, COPYDATASTRUCT* cds) {
    if (app::ShellTagRequest tag_request; app::DecodeShellTagRequest(cds, tag_request)) {
        // Explorer "Pulse tags" verb: tag in place, never raise the window.
        QueueShellTagRequest(s, std::move(tag_request), false);
        return TRUE;
    }
    if (cds && cds->dwData == app::SingleInstanceCoordinator::OpenRequestMessageId()) {
        app::SingleInstanceCoordinator::OpenRequest request;
        if (!app::SingleInstanceCoordinator::DecodeOpenRequest(cds, request)) return FALSE;
        const auto accepted = s.single_instance.AcceptOpenRequest(request, GetTickCount64());
        if (accepted == app::SingleInstanceCoordinator::OpenAcceptance::Invalid) return FALSE;
        if (accepted == app::SingleInstanceCoordinator::OpenAcceptance::New)
            QueueExternalOpen(s, std::move(request.path));  // plan §6: tab, window or the open one
        return TRUE; // acceptance, not a claim that asynchronous enumeration succeeded
    }
    std::wstring path;
    if (!app::SingleInstanceCoordinator::DecodeOpenPath(cds, path)) return FALSE;
    QueueExternalOpen(s, std::move(path));
    return TRUE;
}

// Settings > 快捷键: a key the user assigned reaches the handlers as the
// action's default key (keymap_input.h); a default key that moved to another
// key is swallowed, with the character message it produces. Recorders see
// the raw keys.
LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
// WM_MOUSEACTIVATE on the tab strip: the next caption press only brings the window forward.
static ULONGLONG g_caption_activate_tick = 0;
static int g_shortcut_remap_depth = 0;
static bool g_shortcut_eat_char = false;

static bool IsAltTagKey(WPARAM key) {
    return (GetKeyState(VK_MENU) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000) &&
           !(GetKeyState(VK_SHIFT) & 0x8000) &&
           ((key >= L'0' && key <= L'9') || key == L'T');
}

static bool RouteUserShortcut(AppState& s, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    if (msg == WM_CHAR || msg == WM_SYSCHAR) {
        if (!g_shortcut_eat_char) return false;
        g_shortcut_eat_char = false;
        result = 0;
        return true;
    }
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
        g_shortcut_eat_char = false;
        return false;
    }
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return false;
    g_shortcut_eat_char = false;
    if (g_shortcut_remap_depth > 0 || s.settings.global_search_hotkey_capturing()) return false;
    if (app::IsModifierKey(static_cast<UINT>(wParam))) return false;
    const app::KeyRoute route = app::RouteKey(app::ActiveKeymap(),
        app::CurrentChord(static_cast<UINT>(wParam)), lParam);
    if (route.kind == app::KeyRoute::Pass) return false;
    g_shortcut_eat_char = true;
    if (route.kind == app::KeyRoute::Swallow) {
        result = 0;
        return true;
    }
    ++g_shortcut_remap_depth;
    {
        app::ModifierOverride modifiers(route.mods);
        result = WndProcImpl(hwnd, route.msg, route.vk, route.lparam);
    }
    --g_shortcut_remap_depth;
    return true;
}

// Context-menu caches and the registry watch that keeps them current.
static void StartMenuServices(AppState& s) {
    SeedShellVerbCache(s);
    SeedCtxLayout(s);
    StartShellRegistryWatch(ServiceWindow(s));
}

static void StartSearchServices(AppState& s) {
    const HWND service_window = ServiceWindow(s);
    // Search off: no index client, so nothing starts the service or a helper.
    if (s.appPrefs.search_enabled) s.index.Start(service_window, WM_INDEX_NOTIFY, WM_INDEX_SEARCH);
    StartChangeTracking(s);
    if (s.appPrefs.search_enabled) {
        s.networkIndex.Start(service_window, WM_NETWORK_INDEX_NOTIFY, WM_NETWORK_INDEX_SEARCH);
        s.contentSearch.Start(service_window, WM_CONTENT_SEARCH, s.contentIndexObserver ? index::ContentAgentMode::Observer : index::ContentAgentMode::Instant);
    }
    s.duplicateSearch.Start(service_window, WM_DUPLICATE_SCAN, false);
}

// A hidden launch that only applies a tag (Explorer verb, --tag-*) leaves the
// menu caches and search services out; they start if its window is shown.
// Every one of them already stops safely without having started (search
// off, safe mode, test instances).
static bool g_servicesDeferred = false;

static void StartDeferredServices(AppState& s) {
    if (!g_servicesDeferred) return;
    g_servicesDeferred = false;
    if (!s.safeMode) StartMenuServices(s);
    StartSearchServices(s);
}

LRESULT CALLBACK WndProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    pulse::app::startup_trace::MessageScope trace_message(msg);
    AppState* s = GetAppState(hwnd);
    // A Chinese / Japanese IME in native mode can report a Ctrl or Alt chord
    // as VK_PROCESSKEY; every shortcut below compares the real key.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_PROCESSKEY &&
        ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000))) {
        if (const UINT vk = ImmGetVirtualKey(hwnd); vk && vk != VK_PROCESSKEY) wParam = vk;
    }
    if (s && msg != 0) {
        // Settings > 快捷键 recorder / key tester / search box: raw keys first.
        if (g_shortcut_remap_depth == 0 && ShortcutsPageKey(*s, msg, wParam, lParam)) return 0;
        LRESULT routed = 0;
        if (RouteUserShortcut(*s, hwnd, msg, wParam, lParam, routed)) return routed;
    }
    if (s && msg != 0 && msg == app::UpdateShutdownMessage())
        return CloseForUpdate(*s);
    if (s && IsUiActivityMessage(msg) && !(msg == WM_PAINT && t_pump_paint))
        NoteUiActivity(*s, hwnd, IsWindowVisible(hwnd) && !IsIconic(hwnd));
    if (s && s->notification_toast.HandleMessage(hwnd, msg, wParam, lParam)) return 0;
    if (s && HoverPreviewMessage(*s, hwnd, msg, wParam, lParam)) return 0;
    if (s && WindowSwitcherMessage(*s, hwnd, msg, wParam, lParam)) return 0;  // M7d
    if (LRESULT overview_result = 0; s && WindowOverviewMessage(*s, hwnd, msg, wParam, lParam, overview_result))
        return overview_result;  // M7e
    if (s && GroupWheelMessage(*s, hwnd, msg, wParam, lParam)) return 0;
    // Explorer restarted or came up late. The broadcast reaches every
    // top-level window; the host window handles it, unless there is none.
    if (s && msg == app::TrayController::TaskbarCreatedMessage() && msg != 0 &&
        ServiceWindow(*s) == hwnd) {
        s->tray_controller.HandleTaskbarCreated();
        return 0;
    }
    // M7c: this window's taskbar button exists (again); give it its dot.
    if (s && msg != 0 && msg == TaskbarButtonCreatedMessage()) {
        SyncWindowOverlay(*s);
        return 0;
    }

    switch (msg) {
    case WM_NCACTIVATE:
        // lParam -1: update the activation state without repainting a
        // non-client caption (the window has WS_CAPTION for DWM animations).
        return DefWindowProcW(hwnd, msg, wParam, -1);

    case WM_NCCALCSIZE: {
        if (wParam && IsZoomed(hwnd)) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromRect(&params->rgrc[0],
                    MONITOR_DEFAULTTONEAREST), &monitor)) {
                // Our custom frame has no invisible maximized border to inset.
                // Use the destination monitor's work area, including taskbar offsets.
                params->rgrc[0] = monitor.rcWork;
            }
        }
        return 0;
    }

    case WM_CREATE: {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = reinterpret_cast<AppState*>(cs->lpCreateParams);
        pulse::app::startup_trace::Mark("wm_create");
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        s->hwnd = hwnd;
        s->core.windows.Add(hwnd, s);
        // Later windows (M4a) skip the process-level setup below: services,
        // saved state and the tray already belong to the first window.
        const bool init_core = !s->core.started;
        if (init_core) {
            s->tray_controller.Attach(hwnd, cs->hInstance, ServiceWindow(*s));
            InstallTrayRevealHook(*s);
        }

        s->scale = s->shot_scale_override > 0.0f
            ? s->shot_scale_override : (float)pulse::compat::WindowDpi(hwnd) / 96.0f;
        s->accentColor = ui::GetAccentColor();
        s->darkMode = ui::ShouldUseDarkMode(s->themeOverride);
        s->backdropActive = ui::ApplyWindowEffect(hwnd, ui::WindowEffect::MicaAlt, s->darkMode);

#ifdef PULSE_WITH_SELFTEST
        if (StartupRollbackFaultForTest(1)) return -1;
#endif
#ifdef PULSE_WITH_SELFTEST
        // Fault injection is disabled unless all persistence is redirected.
        wchar_t startup_fault[32]{};
        const bool startup_fixture = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) > 0;
        if (startup_fixture) GetEnvironmentVariableW(L"PULSE_TEST_STARTUP_FAIL", startup_fault, ARRAYSIZE(startup_fault));
        if (wcscmp(startup_fault, L"graphics") == 0) return -1;
#endif
        pulse::app::startup_trace::Mark("pre_graphics");
        if (!s->compositor.Init(hwnd)) {
            const std::wstring message = L"Failed to initialize D3D/D2D/DWrite\n\n" +
                s->compositor.InitializationError() + L"\n\nLog: %LOCALAPPDATA%\\Pulse\\pulse_graphics.log";
            MessageBoxW(hwnd, message.c_str(), L"Pulse", MB_OK | MB_ICONERROR);
            return -1;
        }
#ifdef PULSE_WITH_SELFTEST
        if (wcscmp(startup_fault, L"after-graphics") == 0) return -1;
#endif
        pulse::app::startup_trace::Mark("compositor_init");
        s->compositor.RecreateTextFormats(s->scale);
        pulse::app::startup_trace::Mark("text_formats");
        if (s->shot.active) {
            wchar_t toast_test[2]{};
            if (GetEnvironmentVariableW(L"PULSE_TEST_TOAST", toast_test, 2) == 1 && toast_test[0] == L'1')
                s->notification_toast.Show(hwnd,
                    l10n::Get(l10n::StringId::IndexMigrationIncomplete),
                    l10n::Get(l10n::StringId::IndexMigrationNoSpace));
        }
        s->renderer.SetCompositor(&s->compositor);
        s->renderer.SetScale(s->scale);
        s->renderer.SetIconNotifyWindow(hwnd);
        // The association watch, Quick Look and the operations window wait
        // for WM_DEFERRED_STARTUP, after the first frame: together they cost
        // the cold start about 150 ms. Later windows build Quick Look and the
        // operations window on first use (ToggleQuickPreview,
        // EnsureOperationWindow); each is a hidden window that cost every
        // Ctrl+N about 25 ms.
        if (init_core) PostMessageW(hwnd, WM_DEFERRED_STARTUP, 0, 0);

        s->window_tabs.EnsureDefault();
        BindCurrentLayout(*s);
        pulse::app::startup_trace::Mark("layout_bound");
#ifdef PULSE_WITH_SELFTEST
        if (StartupRollbackFaultForTest(2)) return -1;
#endif
        s->tabs.SetCallbacks({
            [s](app::Tab& tab) { StartLoadingPath(*s, tab, tab.current_path); },
            [hwnd] { InvalidateRect(hwnd, nullptr, FALSE); },
            [s] { BindCurrentLayout(*s); },
            [s] { RememberLayoutFocus(*s); },
            [s](std::wstring& path) {
                if (s->appPrefs.new_tab_open != 1) return false;
                path = app::DefaultLocation(s->appPrefs);
                return true;
            },
            [s] { return s->appPrefs.close_window_with_last_tab; },
            [hwnd] { PostMessageW(hwnd, WM_CLOSE, 0, 0); },
            [s](const std::vector<size_t>& indices) { RememberClosingTabs(*s, indices); },
        });
        if (init_core) {
            if (s->isolatedTest) {
                s->places.persist = false;
                s->appPrefs.persist = s->isolatedTestPersist; // only into PULSE_TEST_DATA_DIR
                s->ctxMenuPrefs.persist = s->isolatedTestPersist;
            }
            s->savedSearches.Load();
            SyncSavedSearchSidebar(*s);
            s->places.Load();
            if (!s->isolatedTest || s->isolatedTestPersist)
                app::AliasStore::Instance().Load(app::GetPulseDataDir() + L"\\aliases.json");
            app::AliasStore::PinyinHook() = [](std::wstring_view name, std::wstring_view query) {
                return index::PinyinEligible(query) && static_cast<bool>(index::FindPinyinMatch(name, query));
            };
            ProbePinnedNetworks(*s);
            s->ctxMenuPrefs.Load();
            if (s->isolatedTest) {
                // GUI verification of the menu settings with a given hidden mask
                // (decimal), without touching the user's context_menu.json.
                wchar_t mask[16]{};
                if (GetEnvironmentVariableW(L"PULSE_TEST_BUILTIN_HIDDEN", mask, ARRAYSIZE(mask)) > 0) {
                    s->ctxMenuPrefs.builtin_hidden = static_cast<uint32_t>(wcstoul(mask, nullptr, 10));
                    s->ctxMenuPrefs.persist = false;
                }
                // PULSE_TEST_BUILTIN_ORDER0/1/2: comma-joined row keys per surface.
                for (int surface = 0; surface < static_cast<int>(app::BuiltinMenuSurface::Count); ++surface) {
                    wchar_t name[32]{};
                    swprintf_s(name, L"PULSE_TEST_BUILTIN_ORDER%d", surface);
                    wchar_t keys[512]{};
                    if (GetEnvironmentVariableW(name, keys, ARRAYSIZE(keys)) == 0) continue;
                    app::BuiltinMenuOrder order;
                    wchar_t* next = nullptr;
                    for (wchar_t* key = wcstok_s(keys, L",", &next); key; key = wcstok_s(nullptr, L",", &next))
                        for (int i = 0; i < app::kBuiltinMenuItemCount; ++i)
                            if (app::BuiltinMenuKey(static_cast<app::BuiltinMenuItem>(i)) == key)
                                order.push_back(static_cast<app::BuiltinMenuItem>(i));
                    s->ctxMenuPrefs.SetBuiltinOrder(static_cast<app::BuiltinMenuSurface>(surface), order);
                    s->ctxMenuPrefs.persist = false;
                }
            }
            s->appPrefs.Load();
            ApplyKeymapPrefs(*s);
            ApplyNetworkPrefs(*s);  // 显示隐藏共享 reaches fs before any share list
            NoteRunningVersion(*s);
            if (!s->shot.active && s->appPrefs.theme_mode >= 0) {
                s->themeOverride = s->appPrefs.theme_mode == 1 ? ui::ThemeMode::Light :
                    s->appPrefs.theme_mode == 2 ? ui::ThemeMode::Dark : ui::ThemeMode::Auto;
                s->darkMode = ui::ShouldUseDarkMode(s->themeOverride);
            }
            s->searchHistory.persist = !s->shot.active && !s->menushot && !s->isolatedTest;
            if (s->searchHistory.persist) s->searchHistory.Load();
            s->showFps = s->forceStatusPerformance || s->appPrefs.show_status_performance;
            s->duplicateScan.scope = static_cast<app::DuplicateScanScope>(
                std::clamp(s->appPrefs.duplicate_scan_scope, 0, 2));
            s->duplicateScan.folder_path = s->appPrefs.duplicate_scan_folder;
            s->duplicateScan.drive_root =
                app::DuplicateScanSession::NormalizeDriveRoot(s->appPrefs.duplicate_scan_drive);
            s->duplicateScan.minimum_file_bytes =
                app::DuplicateScanSession::DefaultMinimumBytes(s->duplicateScan.scope);
            if (s->shot.active && l10n::IsLanguageId(s->shot.language) &&
                s->shot.language != L"system")
                s->appPrefs.language = s->shot.language;
            if (s->shot.active) {
                // GUI verification of hidden / protected item styling (never persisted).
                wchar_t show_hidden[2]{};
                if (GetEnvironmentVariableW(L"PULSE_TEST_SHOW_HIDDEN", show_hidden, 2) == 1 &&
                    show_hidden[0] == L'1') {
                    s->appPrefs.show_hidden_files = true;
                    s->appPrefs.show_protected_os_files = true;
                }
                // GUI verification of the interface font (never persisted).
                wchar_t test_family[128]{};
                if (GetEnvironmentVariableW(L"PULSE_TEST_UI_FONT_FAMILY", test_family,
                        ARRAYSIZE(test_family)) > 0)
                    s->appPrefs.ui_font_family = test_family;
            }
            l10n::Initialize(cs->hInstance, s->appPrefs.language);
            if (s->shot.update_available) {
                s->update_result_ready = true;
                s->update_result.update_available = true;
                s->update_result.version = L"9.8.7";
                s->update_result.download_page = L"https://updates.example.test/pulse";
                s->update_result.installer_sha256 =
                    L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
            }
            ui::typography::SetUiFontScale(s->appPrefs.ui_font_scale);
            ui::typography::SetUiFontFamily(s->appPrefs.ui_font_family);
            ui::typography::InvalidateCaches();
        }
        s->compositor.RecreateTextFormats(s->scale);
        if (init_core) {
            if (s->safeMode) {
                s->appPrefs.window_effect = L"none";
                s->appPrefs.background_image.clear();
            } else if (ShellTagHeadlessLaunch()) {
                g_servicesDeferred = true;
            } else {
                StartMenuServices(*s);
            }
        }
        // Process wide: every window's menus and frame copies follow it.
        ui::FluentMenu::SetAcrylicEnabled(s->appPrefs.menu_acrylic && !s->safeMode);
        s->renderer.SetRowHeightDip(static_cast<float>(
            app::EffectiveRowHeightDip(s->appPrefs.row_height, s->appPrefs.ui_font_scale)));
        ui::SetHideKnownExtensions(!s->appPrefs.show_file_extensions);
        s->renderer.SetListStyle(s->appPrefs.list_smart_date, s->appPrefs.list_zebra_rows,
                                 s->appPrefs.list_size_bar, s->appPrefs.list_tag_name_color,
                                 s->appPrefs.list_selection_outline);
        s->renderer.SetTreeSticky(s->appPrefs.list_tree_sticky);
        s->renderer.SetDetailsColumns(s->appPrefs.details_columns);
        s->renderer.SetRowActions(app::RowActionMask(s->ctxMenuPrefs.builtin_hidden,
            s->ctxMenuPrefs.BuiltinOrder(app::BuiltinMenuSurface::RowButtons)));
        s->renderer.SetThumbnailBadges(s->appPrefs.list_thumbnail_badges);
        s->renderer.SetMtimeHint(app::MtimeHintFromPrefs(s->appPrefs));
        s->renderer.SetItemClick(s->appPrefs.single_click_open, s->appPrefs.name_highlight, s->appPrefs.item_checkboxes);
        app::SetFolderSortMode(app::FolderSortModeFromInt(s->appPrefs.folder_sort_mode));
        ui::typography::SetTextRenderMode(static_cast<ui::typography::TextRenderMode>(s->appPrefs.text_render));
        s->compositor.UpdateTextRenderingParams(nullptr);
        s->renderer.SetSidebarWidthDip(static_cast<float>(s->appPrefs.sidebar_width));
        s->renderer.SetVerticalTabs(s->appPrefs.vertical_tabs);
        s->renderer.SetSidebarCollapsed(s->appPrefs.sidebar_collapsed);
        s->renderer.SetTrayIconDip(static_cast<float>(s->appPrefs.tray_icon_size));
        ApplyAccentFromPrefs(*s, true);
        ApplyAppWindowChrome(*s);
        if (init_core) {
            bool selection_fixture = false;
#ifdef PULSE_WITH_SELFTEST
            wchar_t fixture_scope[32]{};
            selection_fixture = s->isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) &&
                GetEnvironmentVariableW(L"PULSE_TEST_P1_SCOPE", fixture_scope, ARRAYSIZE(fixture_scope)) &&
                (wcscmp(fixture_scope, L"selection") == 0 || wcscmp(fixture_scope, L"menu") == 0 ||
                 wcscmp(fixture_scope, L"layout") == 0);
#endif
            s->renderer.SetSearchHidden(!s->appPrefs.search_enabled);
            if (!selection_fixture) {
                if (ShellTagHeadlessLaunch()) g_servicesDeferred = true;
                else StartSearchServices(*s);
            }
            s->settings.SetServiceInstalled(s->index.ServiceInstalled());
            app::SettingsController::UiCallbacks settings_callbacks;
            settings_callbacks.pick_image = [s](std::wstring& path) {
                return PickImageFile(app::CurrentWindow(*s), path);
            };
            settings_callbacks.pick_folder = [s](std::wstring& path, std::wstring_view title) {
                const std::wstring owned_title(title);
                // Each settings row keeps its own history, keyed by its title;
                // a path the row passes in is where the picker opens.
                const std::wstring initial = path;
                return PickFolder(app::CurrentWindow(*s), path, owned_title.c_str(), L"settings:" + owned_title,
                                  initial);
            };
            settings_callbacks.apply_effects = [s](app::SettingsEffect effects) {
                // Every window shows the changed setting.
                app::ForEachWindowSharingCore(app::CurrentWindow(*s), [&](AppState& window) {
                    ApplySettingsEffects(window, effects);
                });
            };
            settings_callbacks.integration_changing = [s] { StopShellWindows(*s); };
            settings_callbacks.show_error = [s](const std::wstring& message) {
                AppState& window = app::CurrentWindow(*s);
                window.notification_toast.Show(window.hwnd, l10n::Get(l10n::StringId::SettingsGeneral), message);
            };
            if (s->appPrefs.load_failed) {
                settings_callbacks.show_error(l10n::HantText(l10n::Get(l10n::StringId::TheOriginalSettingsCouldNotBeReadSavingIsBlockedToProtectThe).c_str()));
            }
            settings_callbacks.integration_changed = [s] { SyncShellWindows(app::CurrentWindow(*s)); };
            settings_callbacks.task_completion = SettingsCompletion(ServiceWindow(*s));
            settings_callbacks.open_path = [s](const std::wstring& path) {
                ShellExecuteW(app::CurrentWindow(*s).hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            };
            settings_callbacks.open_diagnostics = [s] {
                const std::wstring path = app::GetPulseDataDir() + L"\\Diagnostics";
                CreateDirectoryW((app::GetPulseDataDir() + L"\\Diagnostics").c_str(), nullptr);
                CreateDirectoryW(path.c_str(), nullptr);
                ShellExecuteW(app::CurrentWindow(*s).hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            };
            settings_callbacks.clear_diagnostics = [s](std::wstring& error) {
                if (!ConfirmClearDiagnostics(app::CurrentWindow(*s))) return true;
                if (diagnostics::ClearCrashReports(app::GetPulseDataDir(), &error)) return true;
                error = l10n::Get(l10n::StringId::DiagnosticsClearFailed);
                return false;
            };
            settings_callbacks.thumb_cache_bytes = [](uint64_t& bytes) {
                ui::ThumbnailDiskCache& cache = ui::ThumbnailDiskCache::Shared();
                if (cache.Root().empty()) return false;
                bytes = cache.BytesOnDisk();
                return true;
            };
            settings_callbacks.clear_thumb_cache = [] {
                ui::ThumbnailDiskCache& cache = ui::ThumbnailDiskCache::Shared();
                const uint64_t before = cache.BytesOnDisk();
                cache.Clear();
                const uint64_t after = cache.BytesOnDisk();
                return before > after ? before - after : uint64_t{0};
            };
            settings_callbacks.prepare_diagnostics_export =
                [s](std::wstring& destination, bool& include_service, bool& include_dumps) {
                AppState& window = app::CurrentWindow(*s);
                if (!ConfirmDiagnosticsExport(window, include_service, include_dumps)) return false;
                return PickFolder(window, destination,
                    l10n::Get(l10n::StringId::DiagnosticsExportLocation).c_str(), L"diagnostics_export");
            };
            settings_callbacks.export_diagnostics = ExportAppDiagnostics;
            settings_callbacks.settings_transfer = [s](int action) {
                SettingsTransferAction(app::CurrentWindow(*s), action);
            };
            settings_callbacks.settings_transfer_task = RunSettingsTransferTask;
            s->settings.BindUi(s->appPrefs, s->ctxMenuPrefs, s->index,
                               s->networkIndex, std::move(settings_callbacks));
            ApplyGlobalSearchSettings(*s);

            s->worker.Start([s](app::WorkResult res) { PostWorkerResult(*s, std::move(res)); });
            RefreshSidebarModel(*s);
            if (s->appPrefs.persist) QueueTagAds(*s, {});
            RequestRecycleOccupancy(*s);
            RequestNetworkLocations(*s);
            // Phones: arrival notifications on the host; listed only when one is present.
            app::DeviceRegistry::Instance().Start(ServiceWindow(*s));
            ui::SetDeviceCrumbResolver([](const std::wstring& key) {
                ui::DeviceCrumbInfo info;
                info.known = app::DeviceRegistry::Instance().Describe(key, &info.name, &info.skips_storage);
                return info;
            });
            // Verification hook (isolated test instances only): open the phone
            // help after N ms so its dialog can be captured.
            if (s->isolatedTest) {
                wchar_t ms[16]{};
                if (GetEnvironmentVariableW(L"PULSE_TEST_DEVICE_HELP_MS", ms, 16) > 0 && _wtoi(ms) > 0)
                    SetTimer(hwnd, kTimerDeviceHelpTest, static_cast<UINT>(_wtoi(ms)), nullptr);
            }

            // Ops layer: queue worker + shell host IPC; notify repaints the status bar.
            const std::wstring data_dir = app::GetPulseDataDir();
            if (!s->isolatedTest && !data_dir.empty())
                s->ops.SetJournalPath(data_dir + L"\\operations.json");
            s->ops.SetVerifyCopies(s->appPrefs.verify_copies);
            // Shell dialogs (打开方式…/属性) must be owned by the Pulse window.
            s->ops.SetUiWindow(hwnd);
            s->ops.Start([notify = ServiceWindow(*s)] { PostMessageW(notify, WM_OPS_NOTIFY, 0, 0); });
            // Drop stages of Pulse processes that are gone (#55).
            app::SweepDropStages(app::DropStageRoot(), false);
            const ops::RecoverySnapshot recovery = s->isolatedTest
                ? ops::RecoverySnapshot{} : s->ops.PendingRecovery();
            if (!recovery.entries.empty()) {
                const bool duplicate_cleanup = std::any_of(recovery.entries.begin(), recovery.entries.end(),
                    [](const ops::RecoveryEntry& entry) { return entry.request.duplicate_cleanup; });
                if (AskRetryRecovery(*s, recovery.entries.size(), recovery.has_uncertain_destructive, duplicate_cleanup))
                    s->ops.RetryRecovery();
                else
                    s->ops.DiscardRecovery();
            }
            s->context_menu.SetShellOperations({
                [s](std::vector<std::wstring> paths, HWND owner, bool background, bool extended,
                    std::vector<std::wstring> disabled) {
                    if (s->safeMode) return uint32_t{0};
                    return s->ops.QueryShellMenu(std::move(paths), owner, background, extended,
                                                 std::move(disabled));
                },
                [s](uint32_t token) { s->ops.CloseShellMenu(token); },
                [s](uint32_t token, uint32_t command, std::wstring verb, std::wstring text) {
                    if (ShellVerbMayCreate(verb)) ArmArrivalSelection(app::CurrentWindow(*s), nullptr, 0, 8000);
                    s->ops.InvokeShellMenu(token, command, std::move(verb), std::move(text));
                },
                [s](const std::wstring& path, const std::wstring& verb) {
                    if (ShellVerbMayCreate(verb)) ArmArrivalSelection(app::CurrentWindow(*s), nullptr, 0, 8000);
                    s->ops.ExecuteVerb(path, verb);
                },
                [s](const std::wstring& app_path, const std::wstring& path) {
                    s->ops.OpenWithApp(app_path, path);
                },
                [s](const std::wstring& command, const std::wstring& path) {
                    ArmArrivalSelection(app::CurrentWindow(*s), nullptr, 0, 8000);
                    s->ops.ExecuteCommand(command, path);
                },
            });
            // Explorer verbs arrive on the shell client's reader thread; hop to
            // the UI thread with an owned payload (freed by the WM handler).
            s->ops.SetShellMenuCallback([notify = ServiceWindow(*s)](uint32_t token,
                                               std::vector<ops::ShellMenuItem> items,
                                               bool partial,
                                               ops::ShellMenuAnswer answer) {
                auto* payload = new ShellCtxItemsPayload{
                    std::move(items), partial, std::move(answer) };
                if (!PostMessageW(notify, WM_SHELLCTX_ITEMS, token,
                                  reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            });
            if (!s->pending_undo_json.empty()) s->ops.UndoFromJson(s->pending_undo_json);
        }

        // OLE drop target: list rows, pane headers, breadcrumbs, sidebar, tray.
        {
            ui::DropTargetCallbacks dcb;
            dcb.drag_over = [hwnd](const std::vector<std::wstring>& srcs, POINT pt,
                                   DWORD keys, DWORD allowed, DWORD preferred) -> DWORD {
                AppState* st = GetAppState(hwnd);
                return st ? ResolveDropTarget(*st, srcs, pt, keys, allowed, preferred)
                          : DROPEFFECT_NONE;
            };
            dcb.drag_leave = [hwnd] {
                if (AppState* st = GetAppState(hwnd)) ClearDropFeedback(*st);
            };
            dcb.drop = [hwnd](const std::vector<std::wstring>& srcs, POINT pt,
                              DWORD keys, DWORD allowed, DWORD preferred) -> DWORD {
                AppState* st = GetAppState(hwnd);
                return st ? DropExecute(*st, srcs, pt, keys, allowed, preferred)
                          : DROPEFFECT_NONE;
            };
            s->dropTarget = new ui::WindowDropTarget(hwnd, std::move(dcb));
            // Registered from WM_REGISTER_DROP_TARGET, after the first frame:
            // the process's first RegisterDragDrop goes through RPCSS and the
            // view service and held the cold start's first frame back 30-120 ms.
            if (!PostMessageW(hwnd, WM_REGISTER_DROP_TARGET, 0, 0))
                RegisterDragDrop(hwnd, s->dropTarget);
        }
        // The toolbar's paste button follows the clipboard (WM_CLIPBOARDUPDATE).
        AddClipboardFormatListener(hwnd);

        if (init_core) {
            // "Open the default location" drops only the saved tabs; the rest of the
            // session (sidebar, details pane, window state) still applies.
            const bool open_default_location =
                !s->shot.active && !app::RestoresLastTabs(s->appPrefs, s->restoreUpdateSession);
            if (open_default_location) {
                s->session_layout_tabs.clear();
                s->session_tab_groups.clear();
                s->session_active_layout_tab = 0;
                s->session_path.clear();
            }
            // The session's other windows (M4d) come back only with its tabs.
            if (open_default_location || s->session_layout_tabs.empty() || s->menushot ||
                s->colorpickshot || s->colorpickdialog)
                s->core.pending_session_windows.clear();
            if (!s->shot.active && !s->session_layout_tabs.empty()) {
                RestoreTabsFromSnapshots(*s, s->session_layout_tabs, s->session_tab_groups,
                                         s->session_active_layout_tab);
                if (!s->session_path.empty())
                    RememberPath(*s, s->session_path);
                else if (app::Tab* t = ActiveTab(*s))
                    RememberPath(*s, t->current_path);
            } else {
            // Shots accept the This PC arguments too (GUI checks of the drive view).
            std::wstring startPath = !s->shot.active ? L"C:\\"
                : app::IsThisPcArgument(s->shot.path) ? std::wstring() : s->shot.path;
            if (!s->shot.active && !s->session_path.empty()) startPath = s->session_path;
            else if (!s->shot.active && !s->open_path.empty())
                startPath = app::IsThisPcArgument(s->open_path) ? std::wstring()
                                                                : ResolveOpenFolderPath(s->open_path);
            else if (open_default_location)
                startPath = app::DefaultLocation(s->appPrefs); // empty = This PC
            s->pane->NewTab(startPath);
            if (s->shot.active) {
                s->pane->ActiveTab()->view_mode = s->shot.view_mode;
                s->pane->ActiveTab()->wall = g_shot_wall;
            }
            StartLoadingPath(*s, *s->pane->ActiveTab(), startPath,
                !s->shot.active && !s->session_path.empty()
                    ? PathLoadReason::RestoreSession : PathLoadReason::Navigate);
            RememberPath(*s, startPath);
            if (!s->shot.active && s->session_path.empty() && !s->open_path.empty())
                SelectLaunchedFile(*s, s->open_path);
            }

            if (!s->shot.active && !s->open_path.empty() &&
                (!s->session_layout_tabs.empty() || !s->session_path.empty())) {
                const bool this_pc = app::IsThisPcArgument(s->open_path);
                const std::wstring open_path = this_pc ? std::wstring() : ResolveOpenFolderPath(s->open_path);
                if (this_pc) OpenTabAt(*s, open_path);  // NewTab would open C: for ""
                else if (!open_path.empty() && !ActivateExistingFolderTab(*s, open_path))
                    NewTab(*s, open_path);
                if (!open_path.empty()) SelectLaunchedFile(*s, s->open_path);
            }
        } else if (s->window_restore && !s->window_restore->layout_tabs.empty()) {
            // A reopened window (M4c) gets its tabs back, as session restore does.
            const app::ClosedWindow restore = std::move(*s->window_restore);
            s->window_restore.reset();
            RestoreTabsFromSnapshots(*s, restore.layout_tabs, restore.tab_groups, restore.active_layout_tab);
            if (app::Tab* t = ActiveTab(*s)) RememberPath(*s, t->current_path);
        } else if (s->window_adopt_from && AdoptMovedTab(*s)) {
            // Ctrl+Shift+M (M5c): the moved tab arrived alive; nothing to load.
        } else {
            // A later window opens one tab at its start folder (OpenPulseWindow).
            const std::wstring start = s->window_start_path;
            s->pane->NewTab(start);
            StartLoadingPath(*s, *s->pane->ActiveTab(), start, PathLoadReason::Navigate);
            RememberPath(*s, start);
        }

        // Settings have been loaded (or their own load_failed guard is set),
        // and the saved layouts/undo are now represented by live models.
        if (init_core && !s->isolatedTest && !s->shot.active && !s->menushot) {
            app::CleanupAbandonedUpdateStagesAsync();
            // AI clients' tool broker, when the AI pack and ai.json allow it (worker thread).
            pulse::app::ai::StartAiServiceAsync({s->core.host.Hwnd(), &s->core.ops, &s->core.index});
        } else if (init_core && s->isolatedTest && ::TestAiBroker()) {
            pulse::app::ai::StartAiServiceAsync({s->core.host.Hwnd(), &s->core.ops, &s->core.index});
        }
        s->startupComplete = true;
        s->lastFrameTime = std::chrono::steady_clock::now();
        s->renderer.SetDetailsPanelVisible(s->showDetailsPanel);
        s->renderer.SetDetailsPanelWidth(s->detailsPanelWidth);
        SetTimer(hwnd, kTimerUi, s->uiTimerPacer.Current(), nullptr);
        s->framePump.Start(hwnd, WM_FRAME_PUMP);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
            SWP_NOACTIVATE | SWP_FRAMECHANGED);
        s->core.started = true;
        // M7c: a color for it; with a second window every window shows its name.
        SyncWindowIdentities(*s);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        // Arrives before WM_CREATE; GWLP_USERDATA is not set yet.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        float sc = s ? s->scale : 1.0f;
        // The window may shrink until only the sidebar rail and the file list are
        // left, the way File Explorer does; below this the toolbar starts to clip.
        mmi->ptMinTrackSize.x = (LONG)(320 * sc);
        mmi->ptMinTrackSize.y = (LONG)(420 * sc);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            mmi->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
            mmi->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
            mmi->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
            mmi->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            mmi->ptMaxTrackSize.x = std::max(mmi->ptMaxTrackSize.x, mmi->ptMaxSize.x);
            mmi->ptMaxTrackSize.y = std::max(mmi->ptMaxTrackSize.y, mmi->ptMaxSize.y);
        }
        return 0;
    }

    case WM_NCHITTEST: {
        POINT screenPt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (!IsZoomed(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const UINT dpi = pulse::compat::WindowDpi(hwnd);
            const int frameX = pulse::compat::SystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                             + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int frameY = pulse::compat::SystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                             + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const bool left = screenPt.x >= wr.left && screenPt.x < wr.left + frameX;
            const bool right = screenPt.x < wr.right && screenPt.x >= wr.right - frameX;
            const bool top = screenPt.y >= wr.top && screenPt.y < wr.top + frameY;
            const bool bottom = screenPt.y < wr.bottom && screenPt.y >= wr.bottom - frameY;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        if (!s) break;
        POINT pt = screenPt;
        ScreenToClient(hwnd, &pt);
        float x = (float)pt.x;
        float y = (float)pt.y;
        float tbH = s->renderer.TitleBarHeight();
        if (y >= 0 && y < tbH) {
            D2D1_RECT_F bounds = D2D1::RectF(0, 0,
                (float)s->compositor.Width(), (float)s->compositor.Height());
            ui::HitTestResult hit = s->renderer.HitTest(BuildVm(*s, false), bounds, x, y);
            if (hit.region == ui::HitTestResult::Maximize) return HTMAXBUTTON;
            if (hit.region == ui::HitTestResult::Minimize) return HTMINBUTTON;
            if (hit.region == ui::HitTestResult::Close) return HTCLOSE;
            if (hit.region != ui::HitTestResult::None) return HTCLIENT;
            return HTCAPTION;
        }
        break;
    }

    case WM_NCMOUSEMOVE: {
        if (!s) break;
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(BuildVm(*s, false), bounds,
            (float)pt.x, (float)pt.y);
        s->hoverPoint = pt;
        const int region = static_cast<int>(hit.region);
        if (region != s->hoverRegion || hit.index != s->hoverControlIndex) {
            ApplyHoverTarget(*s, hit);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
        TrackMouseEvent(&tme);
        break;
    }

    case WM_NCMOUSELEAVE:
        if (s) {
            s->hoverRegion = 0;
            s->hoverControlIndex = -1;
            s->hoverSubIndex = -1;
            s->hoverSince = 0;
            s->tooltipText.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_NCLBUTTONDOWN: {
        if (!s) break;
        // A press on the title bar releases the address editor, the search box
        // and the filter box alike, as a press in the client area does.
        if (s->addressEditing) HideAddressEditor(*s, false);
        if (s->filterEditing) HideFilterEditor(*s, true);
        if (wParam == HTCAPTION) {
            // 点空白取消选择 on the tab strip: the system move loop returns on
            // release; a click that neither moved the window nor the pointer
            // (and did not just bring the window forward) clears the selection.
            const bool activates = g_caption_activate_tick != 0 && GetTickCount64() - g_caption_activate_tick < 500;
            g_caption_activate_tick = 0;
            RECT before{};
            GetWindowRect(hwnd, &before);
            POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const LRESULT result = DefWindowProcW(hwnd, msg, wParam, lParam);
            if (activates || !IsWindow(hwnd)) return result;
            RECT after{};
            POINT now{};
            GetWindowRect(hwnd, &after);
            GetCursorPos(&now);
            if (EqualRect(&before, &after) && std::abs(now.x - at.x) < GetSystemMetrics(SM_CXDRAG) &&
                std::abs(now.y - at.y) < GetSystemMetrics(SM_CYDRAG) && ScreenToClient(hwnd, &at))
                BlankClickDeselect(*s, at);
            return result;
        }
        if (wParam == HTMINBUTTON) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }
        if (wParam == HTMAXBUTTON) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        if (wParam == HTCLOSE) {
            CloseMainWindow(*s);
            return 0;
        }
        break;
    }

    case WM_NCLBUTTONDBLCLK:
        // The tab strip's double click maximizes: undo its first click's deselect.
        if (s && wParam == HTCAPTION) {
            POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (ScreenToClient(hwnd, &at)) RestoreBlankDeselect(*s, at);
        }
        break;

    case WM_CLOSE:
        if (!s) break;
        CloseMainWindow(*s);
        return 0;

    case app::TrayController::kCallbackMessage: {
        if (!s) return 0;
        // The host hands tray clicks to the most recent window; "Show Pulse"
        // brings that one back (M4c), not whichever window the tray had.
        if (s->tray_controller.Window() != hwnd) {
            s->tray_controller.Retarget(hwnd);
            InstallTrayRevealHook(*s);
        }
        const auto result = s->tray_controller.HandleCallback(lParam);
        if (result == app::TrayController::CallbackResult::ExitRequested)
            ExitPulse(*s);
        return 0;
    }

    case WM_EXIT_PULSE:
        // Palette "Exit Pulse" (#57): the tray menu's full exit, reachable
        // when the icon is hidden. Posted so the palette unwinds first.
        if (s) ExitPulse(*s);
        else DestroyWindow(hwnd);
        return 0;

    case WM_DPICHANGED: {
        const float old_scale = s->scale;
        s->scale = (float)HIWORD(wParam) / 96.0f;
        if (old_scale > 0.0f && s->scale != old_scale) {
            // Moved to a monitor at another DPI (M6f): pixel scroll positions
            // follow, so every tab keeps the same rows in view.
            const float ratio = s->scale / old_scale;
            for (auto& item : s->window_tabs.items)
                if (item) RescaleLayoutTabScroll(*item, ratio);
            s->scrollTargetX *= ratio;
            s->scrollTargetY *= ratio;
            s->detailsScroll *= ratio;
        }
        RECT* rc = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
            rc->right - rc->left, rc->bottom - rc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        s->compositor.RecreateTextFormats(s->scale);
        s->renderer.SetScale(s->scale);
        if (s->editFont) {
            DeleteObject(s->editFont);
            s->editFont = nullptr;
        }
        EnsureEditVisuals(*s);
        if (s->hwndAddressEdit) SendMessageW(s->hwndAddressEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->hwndRenameEdit) SendMessageW(s->hwndRenameEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        if (s->hwndTagRenameEdit) SendMessageW(s->hwndTagRenameEdit, WM_SETFONT, (WPARAM)s->editFont, TRUE);
        AiAssistantDpiChanged(*s);
        if (s->addressEditing) LayoutAddressEditor(*s);
        if (!s->tagRenameId.empty()) LayoutTagRenameOverlay(*s);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CLIPBOARDUPDATE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SETTINGCHANGE:
    case WM_DEVICECHANGE:
    case WM_QUERYENDSESSION:
    case WM_ENDSESSION: {
        // Every top-level window receives these; the host handles them once
        // for all windows (HandleSystemBroadcast). Only without a host (tests,
        // probes) does the window handle them itself.
        const auto kind = app::ClassifySystemBroadcast(msg, wParam, lParam);
        if (s && kind != app::SystemBroadcast::None && ServiceWindow(*s) == hwnd)
            return HandleSystemBroadcast(*s, kind, wParam);
        return msg == WM_QUERYENDSESSION ? TRUE : 0;
    }

    case WM_SHOWWINDOW:
        // Sent before the window becomes visible; restore full rate right away.
        if (s && wParam && !IsIconic(hwnd)) NoteUiActivity(*s, hwnd, true);
        break;

    case WM_MOUSEACTIVATE:
        // Single click opens: the click that brings the window forward only selects.
        if (s && LOWORD(lParam) == HTCLIENT && GetForegroundWindow() != GetAncestor(hwnd, GA_ROOT))
            s->activateClickTick = GetTickCount64();
        // Likewise on the tab strip: that click does not clear the selection.
        if (s && LOWORD(lParam) == HTCAPTION && GetForegroundWindow() != GetAncestor(hwnd, GA_ROOT))
            g_caption_activate_tick = GetTickCount64();
        break;

    case WM_ACTIVATE:
        if (s && LOWORD(wParam) != WA_INACTIVE) {
            s->core.windows.Touch(hwnd);
            RetargetTagRules(hwnd);
            // The session's other windows open once the first one is on screen
            // (M4d): at launch, or when a launch into the tray is revealed.
            if (!s->core.pending_session_windows.empty() && s->startupComplete)
                PostMessageW(hwnd, WM_OPEN_SESSION_WINDOWS, 0, 0);
            // File operation progress follows the window used last (M4c).
            if (AppState* ui = GetAppState(s->ops.UiWindow()); ui && ui != s) MoveOperationUi(*ui, *s);
        }
        if (s) s->renderer.NotifyPreviewActivate(LOWORD(wParam) != WA_INACTIVE);
        if (s && LOWORD(wParam) != WA_INACTIVE) {
            std::wstring kind, page;
            const auto* tab = ActiveTab(*s);
            if (tab && app::ParsePulsePath(tab->current_path, &kind, &page) && kind == L"settings" &&
                app::SettingsController::PageFromName(page) == 5)
                ui::DetectPreviewCodecs(true, hwnd);
        }
        break;

    case WM_ACTIVATEAPP:
        if (s && !wParam) {
            s->detailsPreviewPanning = false;
            s->renderer.EndDetailsPreviewPan();
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        if (s) s->renderer.NotifyPreviewActivate(wParam != 0);
        return 0;

    case WM_MOVE:
        if (s) {
            s->compositor.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
            s->renderer.NotifyPreviewOwnerMoved();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_SIZE: {
        if (s) {
            s->compositor.Resize(LOWORD(lParam), HIWORD(lParam));
            s->maximized = (wParam == SIZE_MAXIMIZED);
            if (wParam != SIZE_MINIMIZED && IsWindowVisible(hwnd)) NoteUiActivity(*s, hwnd, true);
            if (s->addressEditing) LayoutAddressEditor(*s);
            if (s->filterEditing && !s->filterFocusPending) LayoutFilterEditor(*s);
            if (!s->tagRenameId.empty()) LayoutTagRenameOverlay(*s);
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        RECT damage{};
        const bool partial = app::TakePartialDamage(hwnd, ps.rcPaint, &damage);
        if (s) {
            t_paint_damage = partial ? &damage : nullptr;
            Render(*s);
            t_paint_damage = nullptr;
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT: {
        if (!s) break;
        if (HBRUSH brush = AiAssistantCtlColor(*s, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam)))
            return reinterpret_cast<LRESULT>(brush);
        EnsureEditVisuals(*s);
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, ui::EditTextColor(s->darkMode));
        SetBkColor(hdc, ui::EditBackColor(s->darkMode));
        SetBkMode(hdc, OPAQUE);
        return reinterpret_cast<LRESULT>(ui::EditBackBrush(s->editBrush));
    }

    case WM_EXPLORER_TAKEOVER: {
        std::unique_ptr<app::ExplorerTakeoverRequest> request(
            reinterpret_cast<app::ExplorerTakeoverRequest*>(lParam));
        if (s && request) HandleExplorerTakeover(*s, *request);
        return 0;
    }

    case WM_REOPEN_CLOSED_WINDOW:
        if (s) ReopenClosedWindow(*s);
        return 0;

    case WM_REOPEN_CLOSED_TABS:
        if (s) ReopenClosedTabs(*s);
        return 0;

    case WM_DETAILS_TREE_UNDO:
        if (s) UndoDetailsTreeExpand(*s);
        return 0;

    case WM_FAVORITE_UNDO:
        if (s) UndoFavoriteRemoval(*s);
        return 0;

    case WM_TAG_RULE_HIT:
        if (s) HandleTagRuleHits(*s);
        return 0;
    case WM_TAG_RULE_ACCEPT:
        if (s) AcceptTagRuleSuggestion(*s);
        return 0;
    case WM_GIT_RESULT:
        if (s) ReceiveGitResult(*s, lParam);
        return 0;
    case WM_CLOUD_PIN_DONE:
        // Toast for 始终保留在此设备上 / 释放空间 (app_commands.cpp RunCloudPin).
        if (s && s->hwnd) {
            const bool freed = wParam == 1; // fs::CloudPinAction::Free
            std::wstring title;
            ui::SnackIcon icon = ui::SnackIcon::Suggest;
            if (lParam > 0) {
                title = l10n::Get(l10n::StringId::CloudPinFailed);
                if (const size_t at = title.find(L"{n}"); at != std::wstring::npos)
                    title.replace(at, 3, std::to_wstring(static_cast<long long>(lParam)));
            } else {
                title = l10n::Get(freed ? l10n::StringId::CloudFreedToast : l10n::StringId::CloudKeptToast);
            }
            s->notification_toast.ShowSnack(s->hwnd, icon, std::move(title), {}, {}, {}, 3000);
        }
        return 0;

    case WM_CLOUD_DOWNLOAD:
        if (s) HandleCloudDownloadMessage(*s, wParam, lParam);
        return 0;

    case WM_DEVICES_CHANGED:
        // The phone list changed (DeviceRegistry): sidebar rows and This PC.
        if (s) app::ForEachWindowSharingCore(*s, [](AppState& window) {
            SyncDeviceSidebar(window);
            RefreshPath(window, std::wstring(), RefreshReason::ShellNotification);
            RefreshDeviceViews(window);
            InvalidateRect(window.hwnd, nullptr, FALSE);
        });
        return 0;

    case WM_UI_KICK:
        // Results for the shared queues (UiTimerPacer): run the tick now
        // rather than at the next quiet poll.
        return s ? WndProcImpl(hwnd, WM_TIMER, kTimerUi, 0) : 0;
    case WM_TAG_DROP_UNDO:
        if (s) UndoDropTags(*s);
        return 0;

    case WM_UNDO_DELETE:
        if (s && s->ops.CanUndo()) s->ops.Undo();
        return 0;
    case WM_RECYCLE_REFUSED:
        if (s) OfferPermanentDeleteForRefused(*s);
        return 0;

    case WM_TRAY_UNDO:
    case WM_TRAY_OPEN_DEST:
    case WM_TRAY_RESTORE:
    case WM_TRAY_TAKE_CLIP:
        if (s) HandleTrayMessage(*s, msg);
        return 0;

    case WM_AI_SNACK_UNDO:
    case WM_AI_SNACK_OPEN:
        if (s) pulse::HandleAiSnackMessage(*s, msg);
        return 0;

    case WM_ZIP_COMPRESS:
    case WM_ZIP_CANCEL:
        if (s) HandleZipMessage(*s, msg, wParam, lParam);
        return 0;

    case WM_OPEN_SESSION_WINDOWS:
        if (s) OpenSessionWindows(*s);
        return 0;

    case WM_MOVE_TAB_TO_NEW_WINDOW:
        if (s) MoveTabToNewWindow(*s);
        return 0;

    case WM_ENTERSIZEMOVE:
        if (s) BeginWindowMove(*s);  // a title-bar drag may end on another window's strip (M6c)
        break;

    case WM_MOVING:
        if (s) TrackTabDrop(*s);  // a dragged window over another window's strip (M6b, M6c)
        break;

    case WM_EXITSIZEMOVE:
        if (s) EndWindowMove(*s);
        break;

    case WM_MERGE_WINDOW:
        // Destroys this window when the merge happens: touch nothing after.
        if (s) FinishWindowMerge(*s, reinterpret_cast<HWND>(wParam), static_cast<size_t>(lParam));
        return 0;

    case WM_UNDO_WINDOW_MERGE:
        if (s) UndoWindowMerge(*s);
        return 0;

    case WM_CONVERT_EXTERNAL_OPEN:
        // May destroy this window ("make it a tab"): touch nothing after.
        if (s) ConvertExternalOpen(*s);
        return 0;

    case WM_ADOPT_OPEN_POLICY:
        if (s) AdoptSuggestedOpenPolicy(*s);
        return 0;

    case WM_BEGIN_TAB_DETACH:
        // May destroy this window (Esc puts the tab back): touch nothing after.
        if (s) RunDetachedTabMove(*s, reinterpret_cast<HWND>(wParam), static_cast<size_t>(lParam));
        return 0;

    case WM_SHELL_SELECT: {
        std::unique_ptr<app::ShellSelectRequest> request(reinterpret_cast<app::ShellSelectRequest*>(lParam));
        if (s && request) HandleShellSelect(*s, *request);
        return 0;
    }

    case WM_FRAME_PUMP: {
        if (!s) return 0;
        s->framePump.FrameConsumed();
        if (t_render_depth > 0) {  // dispatched by a modal loop inside a frame
            DeferFrame(hwnd);
            return 0;
        }
        if (IsWindowVisible(hwnd) && !IsIconic(hwnd) && WindowCloaked(hwnd)) {
            s->framePump.Disarm();
            SetTimer(hwnd, kTimerCloakPoll, kCloakPollMs, nullptr);
            return 0;
        }
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd) ||
            (!s->scrollAnimating && !s->marqueeAutoScroll && !s->themeReveal.start && !s->paneLayoutMotion.start &&
             !s->trayAnimating && !s->renderer.TickMotion(GetTickCount64()))) {
            s->framePump.Disarm();  // WM_PAINT re-arms once shown again
            return 0;
        }
        // The assistant's queued steps keep time while frames run.
        AiChatRunDue(*s);
        AiVoiceRunDue(*s);
        // Frames keep the queue busy, so kTimerUi can starve (UiTimerPacer::TickOverdue).
        if (s->uiTimerPacer.TickOverdue(GetTickCount64())) WndProcImpl(hwnd, WM_TIMER, kTimerUi, 0);
        // Paint now rather than queue a low-priority WM_PAINT; Render re-arms
        // while anything still moves. Posted messages outrank input, so when
        // input is waiting the frame becomes an ordinary WM_PAINT instead:
        // a slow frame must never starve the mouse and keyboard.
        // Only hover plates gliding: their paths are all that changes.
        D2D1_RECT_F plates{};
        const bool plates_only = !AppMotion(*s) && s->renderer.HoverMotionDamage(&plates);
        if (plates_only) app::InvalidatePartial(hwnd, app::DamagePixels(plates));
        if (HIWORD(GetQueueStatus(QS_INPUT)) != 0) {
            if (!plates_only) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // Motion can move state the session keeps (a scroll gliding to rest).
        s->core.sessionActivity = GetTickCount64();
        t_pump_paint = true;
        RedrawWindow(hwnd, nullptr, nullptr, plates_only ? RDW_UPDATENOW : RDW_INVALIDATE | RDW_UPDATENOW);
        t_pump_paint = false;
        return 0;
    }

    case WM_TIMER: {
        if (s && wParam == 0x4457) {  // kTimerDeviceWatch (app_runtime.cpp)
            FireDeviceFolderWatch(*s);
            return 0;
        }
        if (s && wParam == kTimerDeviceHelpTest) {
            KillTimer(hwnd, kTimerDeviceHelpTest);
            ShowDeviceHelp(*s);
            return 0;
        }
        if (s && wParam == kTimerGraphicsRetry) {
            KillTimer(hwnd, kTimerGraphicsRetry);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (s && wParam == kTimerCloakPoll) {
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
                KillTimer(hwnd, kTimerCloakPoll);  // WM_PAINT re-arms once shown again
            } else if (!WindowCloaked(hwnd)) {
                KillTimer(hwnd, kTimerCloakPoll);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (s && wParam == kTimerMarqueeScroll) {
            TickMarqueeAutoScroll(*s);
            return 0;
        }
        if (s && wParam == kTimerDrawnCaret) {
            KillTimer(hwnd, kTimerDrawnCaret);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (s && wParam == kTimerWallHint) {
            WallHintTimer(*s);
            return 0;
        }
        if (s && wParam == kTimerUncProbeRetry) {
            KillTimer(hwnd, kTimerUncProbeRetry);
            PumpUncProbe(*s);
            return 0;
        }
        if (s && wParam == kTimerUi) {
            SyncUiTimerRate(*s, hwnd, IsWindowVisible(hwnd) && !IsIconic(hwnd));
            s->uiTimerPacer.NoteTick(GetTickCount64());
            bool dirty = false;
            if (TickChangeTracking(*s)) dirty = true;
            if (s->renderer.MtimeBoundaryDue()) dirty = true;   // 修改时间提示: a mark aged
            static bool folderSizeResortWaiting = false;
            if (s->folderSizes.TakeChanged()) {
                dirty = true;
                folderSizeResortWaiting = true;
                if (s->hoverRegion == static_cast<int>(ui::HitTestResult::RowFolderSize) && !s->tooltipText.empty())
                    s->tooltipText = TooltipForHover(*s);
            }
            // New totals re-sort Size-ordered listings, paced (#58).
            if (folderSizeResortWaiting) folderSizeResortWaiting = ResortForFolderSizes(*s);
            if (DrainDirNotifies(*s)) {
                s->uiTimerPacer.NoteActivity(GetTickCount64());
                SyncUiTimerRate(*s, hwnd, IsWindowVisible(hwnd) && !IsIconic(hwnd));
            }
            // Sync states of on-screen cloud rows came back: repaint once.
            if (const uint64_t cloud_sync = app::CloudSyncProbe::Instance().Revision();
                cloud_sync != s->cloudSyncRevision) {
                s->cloudSyncRevision = cloud_sync;
                dirty = true;
            }
            const ULONGLONG now = GetTickCount64();
            TickUpdates(*s, now);
            TickQuickPreviewSelection(*s, now);
            if (TickSidebarRefresh(*s, now)) {
                // The sidebar model is shared: whichever window's timer picks
                // up the new drives, every window shows them.
                dirty = true;
                app::ForEachWindowSharingCore(*s, [s](AppState& window) {
                    if (&window != s) InvalidateRect(window.hwnd, nullptr, FALSE);
                });
            }
            TickSessionAutosave(*s, hwnd, now);
            if (s->renderer.TickDetailsPreview(now)) dirty = true;
            const bool app_motion = s->themeReveal.start || s->paneLayoutMotion.start;
            if (s->renderer.TickMotion(now) || app_motion) {
                // Paced by the frame pump when it runs; the timer only
                // re-arms it so the two never both render the same motion.
                if (s->framePump.Running()) ArmMotionFrames(*s, app_motion);
                else dirty = true;
            }
            if (pulse::TickSidebarPeek(*s, now)) dirty = true;
            TickShellTagMenu(*s, now);
            // A hidden tag launch whose window the user opened meanwhile.
            if (!ShellTagHeadlessLaunch()) StartDeferredServices(*s);
            if (s->detailsPreviewFoldStart) {
                const float t = std::min(1.0f, static_cast<float>(now - s->detailsPreviewFoldStart) / 150.0f);
                const float eased = t * t * (3.0f - 2.0f * t);
                const float target = s->detailsPreviewOnly ? 1.0f : 0.0f;
                s->detailsPreviewExpansion = s->detailsPreviewExpansionFrom +
                    (target - s->detailsPreviewExpansionFrom) * eased;
                if (t >= 1) s->detailsPreviewFoldStart = 0;
                dirty = true;
            }
            if (TickAddressSearch(*s, now)) dirty = true;
            if (RefreshContentResults(*s)) dirty = true;
            const int shell_refreshes = s->context_menu.ConsumeDueRefreshes(now);
            for (int i = 0; i < shell_refreshes; ++i) {
                RefreshActiveTab(*s, RefreshReason::ShellNotification);
                dirty = true;
            }
            if (PumpRecycleRefresh(*s, now)) dirty = true;
            MaybePrefetchHoverCtxMenu(*s);
            s->places.FlushPendingSave(false);
            if (s->renameClickCandidate && s->renameClickDue != 0 &&
                now >= s->renameClickDue) {
                app::Tab* tab = ActiveTab(*s);
                const int index = s->renameClickIndex;
                const bool valid = s->pane == s->renameClickPane &&
                    tab == s->renameClickTab && tab &&
                    tab->SelectedCount() == 1 && tab->selected_index == index &&
                    index >= 0 && index < tab->CountBound() &&
                    (s->renameClickPath.empty() ||
                     EntryFullPath(*tab, index) == s->renameClickPath) &&
                    s->renameIndex < 0 && s->tagRenameId.empty() &&
                    !s->addressEditing && !s->filterEditing &&
                    !s->dragPending && !s->marqueePending && !s->marqueeActive &&
                    !s->scrollbarDragging && !s->splitterDragging;
                CancelRenameClick(*s);
                if (valid) RenameSelected(*s);
            }
            if (TickHoverSelect(*s, now)) dirty = true;
            // Search affordance: expand only the focused pane, then reveal
            // the hosted edit once it has enough room for stable text layout.
            for (auto& pane : Panes(*s)) {
                if (pane->header_animation.Tick(pane.get() == s->pane, now))
                    dirty = true;
                if (app::TickFilterAnimation(*pane, s->filterEditing && pane.get() == s->pane, now))
                    dirty = true;
            }
            const float focusedExpand = s->pane ? s->pane->filter_expand : 0.0f;
            if (s->filterFocusPending && focusedExpand >= 1.0f &&
                s->hwndFilterEdit) {
                if (s->pane) s->pane->filter_expand = 1.0f;
                LayoutFilterEditor(*s);
                s->filterIgnoreKillFocus = true;
                ShowWindow(s->hwndFilterEdit, SW_SHOW);
                SetForegroundWindow(GetAncestor(s->hwndFilterEdit, GA_ROOT));
                SetFocus(s->hwndFilterEdit);
                SendMessageW(s->hwndFilterEdit, EM_SETSEL, 0, -1);
                s->filterFocusPending = false;
                s->filterIgnoreKillFocus = false;
                dirty = true;
            }
            // Scrollbar hover expansion.
            float target = s->scrollbarHovered ? (s->renderer.Margin() * 1.5f - 6.0f * s->scale) : 0.0f;
            target = std::max(0.0f, target);
            float step = (target - s->scrollbarHoverWidth) * 0.25f;
            if (std::abs(step) > 0.1f) {
                s->scrollbarHoverWidth += step;
                dirty = true;
            } else if (s->scrollbarHoverWidth != target) {
                s->scrollbarHoverWidth = target;
                dirty = true;
            }
            // Sidebar scrollbar: shown while the sidebar scrolls or the bar is
            // hovered or dragged, faded out otherwise (#44).
            if (s->sidebarScroll != s->sidebarScrollSeen) {
                s->sidebarScrollSeen = s->sidebarScroll;
                s->sidebarScrollbarFade.Reveal(now);
            }
            s->sidebarScrollbarFade.SetHot(s->sidebarScrollbarHot ||
                (s->scrollbarDragging && s->scrollbarSidebar), now);
            if (s->sidebarScrollbarFade.Tick(now)) dirty = true;
            if (s->sidebarScrollbarFade.Moving(now)) s->uiTimerPacer.NoteActivity(now);
            // Scroll positions advance at paint time, once per displayed frame.
            if (s->scrollAnimating) {
                if (s->framePump.Running()) s->framePump.Arm();
                else dirty = true;
            }
            if (!s->tabTracks.empty() || !s->chipTracks.empty()) {
                TickTabTransitions(*s);
                dirty = true;
            }
            // Without the pump (or until it takes over) the timer steps the tray.
            if (!s->trayAnimating && TickTrayDeck(*s)) {
                s->trayAnimating = s->framePump.Running();
                if (s->trayAnimating) ArmMotionFrames(*s, true);
                else dirty = true;
            }
            if (TickTabFlash(*s)) dirty = true;
            if (SyncTagGroups(*s)) {
                // Tags changed: a tag-grouped folder re-sorts into its new groups,
                // and a tag view picks up newly tagged (or untagged) items.
                if (app::Tab* tab = ActiveTab(*s); tab && (tab->EffectiveGroup() ==
                        static_cast<int>(app::GroupBy::Tag) || tab->current_path.starts_with(L"pulse:tag")))
                    RefreshActiveTab(*s);
                dirty = true;
            }
            if (app::Tab* tab = ActiveTab(*s)) {
                std::wstring kind, rest;
                if (app::ParsePulsePath(tab->current_path, &kind, &rest) &&
                    kind == L"settings") {
                    const int page = app::SettingsController::PageFromName(rest);
                    if (page == 0 && s->bloom_accent.Tick(0.016f)) dirty = true;
                    if (page == 4 && s->duplicateScan.scanning) dirty = true;
                }
            }
            // 150 ms: the icon rail relies on the hint to name each row, and the
            // old 400 ms delay read as "no tooltip at all".
            // No hover tooltip under an open popup menu (it would sit beneath it).
            if (s->hoverRegion != 0 && s->tooltipText.empty() && s->hoverSince != 0 &&
                !(s->menu && s->menu->IsOpen()) &&
                GetTickCount64() - s->hoverSince >= 150) {
                s->tooltipText = TooltipForHover(*s);
                dirty = !s->tooltipText.empty() || dirty;
            }
            if (UpdateTeachTip(*s)) dirty = true;
            if (s->renderer.SettleBreadcrumbScroll(GetTickCount64())) dirty = true;   // 地址栏 back to its end
            // Staging tray: notice items moved or deleted outside Pulse.
            if (!s->tray.batches().empty() && GetTickCount64() - s->trayProbeAt >= 2000) {
                s->trayProbeAt = GetTickCount64();
                if (s->tray.RefreshExists()) dirty = true;
            }
            QueueVisibleTagDiscovery(*s);
            UpdateOperationWindow(*s, false);
            if (dirty) {
                s->uiTimerPacer.NoteActivity(GetTickCount64());
                s->core.sessionActivity = GetTickCount64();  // hidden windows get no paint
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_SETCURSOR: {
        if (!s || LOWORD(lParam) != HTCLIENT) break;
        if (s->starDragActive || s->tabDragging) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (!s->sidebarResizing && AiAssistantSetCursor(*s, pt.x, pt.y)) return TRUE;
        if (s->sidebarResizing || SidebarResizeHit(*s, pt.x, pt.y)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        ui::WindowViewModel vm = BuildVm(*s, false);
        D2D1_RECT_F bounds = D2D1::RectF(0, 0,
            (float)s->compositor.Width(), (float)s->compositor.Height());
        ui::HitTestResult hit = s->renderer.HitTest(vm, bounds, (float)pt.x, (float)pt.y);
        if (s->renderer.DetailsPreviewDragging() ||
            (hit.region == ui::HitTestResult::DetailsPreview && s->renderer.CanDetailsPreviewPan() &&
             !s->renderer.DetailsPreviewIsArchive())) {
            SetCursor(ui::PreviewGrabCursor(s->renderer.DetailsPreviewDragging()));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::DetailsPreviewToggle) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (s->detailsPanelResizing || hit.region == ui::HitTestResult::DetailsResize) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        if (s->columnResizing || hit.region == ui::HitTestResult::ColumnDivider ||
            s->stripResizing || hit.region == ui::HitTestResult::ColumnStripDivider) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        const bool splitter = s->splitterDragging || hit.region == ui::HitTestResult::Splitter;
        if (splitter && !s->splitterDragging && hit.index >= ui::kSplitterSwapIndex) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (splitter) {
            const bool vertical = s->splitterDragging
                ? (s->splitterOrientation == app::SplitOrientation::Vertical)
                : (hit.index >= 0 && hit.index < static_cast<int>(vm.splitters.size()) &&
                   vm.splitters[static_cast<size_t>(hit.index)].vertical);
            SetCursor(LoadCursorW(nullptr, vertical ? IDC_SIZEWE : IDC_SIZENS));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::RowFolderSize ||
            hit.region == ui::HitTestResult::FilterClear ||
            hit.region == ui::HitTestResult::AddressSearch ||
            hit.region == ui::HitTestResult::AddressSearchScope ||
            hit.region == ui::HitTestResult::AddressSearchClear ||
            hit.region == ui::HitTestResult::AddressSearchClose ||
            hit.region == ui::HitTestResult::StatusHintAction ||
            hit.region == ui::HitTestResult::SearchEmptyAction ||
            hit.region == ui::HitTestResult::TeachPrimary ||
            hit.region == ui::HitTestResult::TeachDismiss ||
            hit.region == ui::HitTestResult::TeachNever ||
            hit.region == ui::HitTestResult::FilterEmptyAction) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::AddressSearchMode ||
            hit.region == ui::HitTestResult::AddressSearchContent ||
            hit.region == ui::HitTestResult::AddressSearchOptions) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::AddressBar) {
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::StatusBarTask || hit.region == ui::HitTestResult::StatusBarCancelSearch) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::SettingsAccent ||
            (hit.region == ui::HitTestResult::Row && s->appPrefs.single_click_open &&
             !IsSettingsTab(ActiveTab(*s)))) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        if (s->settings.menu_drag_live()) {
            // Rows only slide along one axis: up/down, or left/right for 行按钮.
            SetCursor(LoadCursorW(nullptr, s->settings.ContextTab() == 2 ? IDC_SIZEWE : IDC_SIZENS));
            return TRUE;
        }
        if (hit.region == ui::HitTestResult::SettingsToggle && hit.index >= ui::kSettingsMenuRowHit) {
            // Hand, not IDC_SIZEALL: some pointer themes draw "move" like "no".
            SetCursor(LoadCursorW(nullptr, IDC_HAND)); // movable Pulse menu preview row
            return TRUE;
        }
        break;
    }

    case WM_CANCELMODE:
        HandleSidebarResize(s, hwnd, msg, lParam);
        if (s) s->settings.EndMenuDrag();
        if (s) { s->detailsPreviewPanning = false; s->renderer.EndDetailsPreviewPan(); }
        if (GetCapture() == hwnd) ReleaseCapture();
        break;
    case WM_MOUSEMOVE:
        if (s) pulse::UpdateSidebarPeek(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (HandleSidebarResize(s, hwnd, msg, lParam)) return 0;
        if (HandleDetailsPreviewPointer(s, hwnd, msg, wParam, lParam)) return 0;
        return HandleMouseMove(s, hwnd, msg, wParam, lParam);

    case WM_MOUSELEAVE:
        if (s) pulse::CloseSidebarPeek(*s);
        return HandleMouseLeave(s, hwnd, msg, wParam, lParam);

    case WM_LBUTTONDOWN:
        if (HandleSidebarResize(s, hwnd, msg, lParam)) return 0;
        if (HandleDetailsPreviewPointer(s, hwnd, msg, wParam, lParam)) return 0;
        return HandleLButtonDown(s, hwnd, msg, wParam, lParam);

    case WM_LBUTTONDBLCLK:
        return HandleLButtonDblClk(s, hwnd, msg, wParam, lParam);

    case WM_LBUTTONUP:
        if (HandleSidebarResize(s, hwnd, msg, lParam)) return 0;
        if (HandleDetailsPreviewPointer(s, hwnd, msg, wParam, lParam)) return 0;
        return HandleLButtonUp(s, hwnd, msg, wParam, lParam);

    case WM_CAPTURECHANGED:
        HandleSidebarResize(s, hwnd, msg, lParam);
        if (s) { s->detailsPreviewPanning = false; s->renderer.EndDetailsPreviewPan(); }
        return HandleCaptureChanged(s, hwnd, msg, wParam, lParam);

    case WM_RBUTTONDOWN:
        return HandleRButtonDown(s, hwnd, msg, wParam, lParam);

    case WM_MBUTTONUP:
        if (s && pulse::HandleTabMiddleClick(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;
        if (s && pulse::HandleFolderMiddleClick(*s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;
        break;

    case WM_RBUTTONUP:
        return HandleRButtonUp(s, hwnd, msg, wParam, lParam);

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:   // only the 地址栏 takes the sideways wheel
        return HandleMouseWheel(s, hwnd, msg, wParam, lParam);

    case WM_APPCOMMAND:
        // DefWindowProc translates side-button releases, including child controls.
        if (s && HandleBrowserNavigation(*s, lParam)) return TRUE;
        break;

    case WM_HOTKEY:
        if (s && wParam == GlobalSearchHotkey::kId) {
            if (ShortcutsPageHotkey(*s, lParam)) return 0;  // 快捷键 recorder: our own hotkey pressed
            if (s->settings.global_search_hotkey_capturing() && IsSettingsTab(ActiveTab(*s))) {
                s->settings.CaptureGlobalSearchHotkey(HIWORD(lParam), LOWORD(lParam));
                InvalidateRect(hwnd, nullptr, FALSE);
            } else ToggleGlobalSearch(*s);
            return 0;
        }
        break;

    case WM_DOUBLE_CTRL:
        if (s) ToggleGlobalSearch(*s, true);
        return 0;

    case WM_DIALOG_JUMP_CONTEXT:
        if (s) SendDialogJumpContext(*s);
        return 0;

    case WM_SYSKEYDOWN:
        if (s && HandleGlobalSearchHotkeyCapture(*s, static_cast<UINT>(wParam))) return 0;
        // 收藏栏 holding the F6 focus: Alt+↓ (the subfolders), Shift+F10.
        if (s && FavoritesBarHasFocus(*s) && (wParam == VK_DOWN || wParam == VK_F10) &&
            HandleFavoritesBarKey(*s, wParam))
            return 0;
        if (s && wParam == L'D' && (GetKeyState(VK_MENU) & 0x8000)) {
            ShowOmnibar(*s, OmnibarMode::Path);
            return 0;
        }
        // Alt+Enter arrives as WM_SYSKEYDOWN, so HandleKeyDown's advertised
        // Properties shortcut (alt && VK_RETURN) was unreachable.
        if (s && wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))
            return HandleKeyDown(s, hwnd, msg, wParam, lParam);
        // Same for Alt+Left/Right/Up (back, forward, up) and F10 (Shift+F10 is
        // the context menu); unhandled keys still reach DefWindowProc there.
        if (s && (wParam == VK_F10 || ((GetKeyState(VK_MENU) & 0x8000) &&
                  (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP))))
            return HandleKeyDown(s, hwnd, msg, wParam, lParam);
        // Alt+0..9 (favorite tags, clear tags) and Alt+T (tag panel).
        if (s && IsAltTagKey(wParam)) return HandleKeyDown(s, hwnd, msg, wParam, lParam);
        break;

    case WM_SYSCHAR:
        // Alt+Enter and the Alt tag keys are handled above; no default beep.
        if (wParam == L'\r' || IsAltTagKey(wParam == L't' ? WPARAM{L'T'} : wParam)) return 0;
        break;

    case WM_CHAR:
        if (s && HandleListCharacter(*s, static_cast<wchar_t>(wParam))) return 0;
        break;

    case WM_KEYDOWN:
        if (s && HandleGlobalSearchHotkeyCapture(*s, static_cast<UINT>(wParam))) return 0;
        if (s && wParam == VK_ESCAPE && s->detailsPreviewPanning) {
            s->detailsPreviewPanning = false;
            s->renderer.EndDetailsPreviewPan();
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        // Shift flips the shelf's copy / move label live.
        if (s && wParam == VK_SHIFT && !s->tray.batches().empty()) {
            s->uiTimerPacer.NoteActivity(GetTickCount64());
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return HandleKeyDown(s, hwnd, msg, wParam, lParam);

    case WM_KEYUP:
        if (s && wParam == VK_SHIFT && !s->tray.batches().empty()) {
            s->uiTimerPacer.NoteActivity(GetTickCount64());
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        break;

    case WM_COMMAND: {
        if (s && reinterpret_cast<HWND>(lParam) == s->hwndFilterEdit &&
            HIWORD(wParam) == EN_CHANGE) {
            SyncFilterEditor(*s);
            return 0;
        }
        if (s && reinterpret_cast<HWND>(lParam) == s->hwndAddressEdit &&
            (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == EN_UPDATE)) {
            if (HIWORD(wParam) == EN_CHANGE) {
                QueueAddressSearch(*s);
                UpdateAddressSuggestions(*s);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (s && wParam == 1001) {
            RefreshActiveTab(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    }

    case WM_WORKER_RESULT: {
        if (s) {
            ProcessPendingResults(*s);
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_NETWORK_LOCATIONS:
        if (s) ApplyNetworkLocations(*s);
        return 0;

    case WM_LAN_DISCOVERY:
        if (s) ApplyLanDiscovery(*s);
        return 0;

    case WM_NETWORK_MAP_DONE: {
        std::unique_ptr<NetworkMapResult> result(reinterpret_cast<NetworkMapResult*>(lParam));
        if (s && result) ApplyNetworkMapDone(*s, *result);
        return 0;
    }

    case WM_DRIVE_VERB: {
        std::unique_ptr<app::DriveVerbResult> result(reinterpret_cast<app::DriveVerbResult*>(lParam));
        if (s && result) ApplyDriveVerbResult(*s, *result);
        return 0;
    }

    case WM_BITLOCKER_UNLOCK: {
        std::unique_ptr<app::BitLockerUnlockResult> result(
            reinterpret_cast<app::BitLockerUnlockResult*>(lParam));
        // Every window showing the drive reloads or updates its banner.
        if (s && result)
            app::ForEachWindowSharingCore(*s, [&](AppState& window) {
                ApplyBitLockerUnlock(window, result->root, result->unlocked);
            });
        return 0;
    }

    case WM_NETWORK_CONNECT: {
        std::unique_ptr<app::NetworkConnectResult> result(
            reinterpret_cast<app::NetworkConnectResult*>(lParam));
        if (s && result)
            app::ForEachWindowSharingCore(*s, [&](AppState& window) {
                ApplyNetworkConnect(window, result->share, result->error);
            });
        return 0;
    }

    case WM_RECYCLE_INFO: {
        auto* info = reinterpret_cast<fs::RecycleBinInfo*>(lParam);
        if (s && info && ApplyQueriedRecycleInfo(*s, *info))
            InvalidateRect(hwnd, nullptr, FALSE);
        delete info;
        return 0;
    }

    case WM_OPS_NOTIFY: {
        if (s) {
            ops::OpStatus st = s->ops.Status();
            UpdateOperationWindow(*s, true);
            if (s->ops.TakeCtxInvokeDone()) {
                if (app::Tab* tab = ActiveTab(*s)) s->store.MarkDirty(tab->current_path);
                RefreshActiveTab(*s, RefreshReason::ShellNotification);
                ScheduleRecycleRefresh(*s);
                RefreshRecycleViews(*s, false);
            }
            // A phone verb the shell could not run (device gone, no app for
            // the type): say so instead of doing nothing.
            if (std::wstring error = s->ops.TakeShellVerbError(); !error.empty())
                s->notification_toast.ShowSnack(hwnd, ui::SnackIcon::Suggest, std::move(error), {}, {}, {}, 4000);
            // Take terminal IDs first: a success queued between these drains
            // must still be applied before its matching intent is released.
            const auto finished_tasks = s->ops.DrainFinishedTasks();
            const auto completions = s->ops.DrainCompletions();
            if (st.completed_ops != s->opsCompleted || !completions.empty() || !finished_tasks.empty()) {
                const bool refresh = st.completed_ops != s->opsCompleted || !completions.empty();
                s->opsCompleted = st.completed_ops;
                if (refresh) RequestSidebarRefresh(*s);
                std::vector<std::wstring> tag_metadata_paths;
                for (const auto& completed : completions) {
                    // Ask for confirmed index changes on the next tick. Keep any
                    // in-flight request and never invent a deletion before capture.
                    s->changes.last_query = 0;
                    s->changes.last_detail_refresh = 0;
                    // Invalidate both sides of every successful mutation. The
                    // focused tab refreshes below; background tabs must not
                    // reuse a stale snapshot when they are shown later.
                    for (const auto& source : completed.sources) {
                        const std::wstring parent = fs::ParentPath(source);
                        if (!parent.empty()) s->store.MarkDirty(parent);
                    }
                    for (const auto& destination : completed.destinations) {
                        const std::wstring parent = fs::ParentPath(destination);
                        if (!parent.empty()) s->store.MarkDirty(parent);
                    }
                    for (const auto& directory : completed.refresh_directories)
                        s->store.MarkDirty(directory);
                    if (completed.refresh_only) continue;
                    ApplyOperationIntentCompletion(*s, completed);
                    NoteArrivalCompletion(*s, completed);
                    if (completed.type == ops::OpType::Copy) {
                        for (size_t i = 0; i < completed.sources.size() &&
                                           i < completed.destinations.size(); ++i) {
                            s->places.CloneAssignments(completed.sources[i],
                                                       completed.destinations[i]);
                            tag_metadata_paths.push_back(completed.destinations[i]);
                        }
                    } else if (completed.type == ops::OpType::Move ||
                               completed.type == ops::OpType::Rename ||
                               completed.type == ops::OpType::BatchRename) {
                        // Tags, stars and pins follow in one pass and one save.
                        std::vector<std::pair<std::wstring, std::wstring>> remaps;
                        for (size_t i = 0; i < completed.sources.size() &&
                                           i < completed.destinations.size(); ++i)
                            remaps.emplace_back(completed.sources[i], completed.destinations[i]);
                        s->places.RemapPaths(remaps);
                        for (size_t i = 0; i < completed.sources.size() &&
                                           i < completed.destinations.size(); ++i) {
                            // 别名 follow renames, drags, cut-paste and their undo.
                            app::AliasStore::Instance().Move(completed.sources[i],
                                                             completed.destinations[i]);
                            FollowMovedFolder(*s, completed.sources[i], completed.destinations[i]);
                            // Only the items that really moved / were renamed:
                            // every rename entry (list, tray) leaves the tray
                            // to this, so failed or cancelled items keep their names.
                            s->tray.ReplacePath(completed.sources[i], completed.destinations[i]);
                            tag_metadata_paths.push_back(completed.destinations[i]);
                        }
                        if (completed.type == ops::OpType::Move &&
                            s->pendingCutClipboardSequence != 0) {
                            s->completedCutClipboardPaths.insert(
                                s->completedCutClipboardPaths.end(), completed.sources.begin(),
                                completed.sources.end());
                            // Keys equal where _wcsicmp is: one lookup per cut item
                            // rather than a search of everything moved so far.
                            std::unordered_set<std::wstring> moved;
                            moved.reserve(s->completedCutClipboardPaths.size());
                            for (const auto& actual : s->completedCutClipboardPaths)
                                moved.insert(pulse::path::FoldedKey(actual));
                            const bool all_done = std::all_of(
                                s->pendingCutClipboardPaths.begin(),
                                s->pendingCutClipboardPaths.end(), [&](const std::wstring& expected) {
                                    return moved.contains(pulse::path::FoldedKey(fs::NormalizePath(expected)));
                                });
                            if (all_done) {
                                ops::CompleteCutClipboard(s->pendingCutClipboardSequence,
                                                          s->pendingCutClipboardPaths);
                                s->pendingCutClipboardSequence = 0;
                                s->pendingCutClipboardPaths.clear();
                                s->completedCutClipboardPaths.clear();
                            }
                        }
                    } else if (completed.type == ops::OpType::RecycleDelete ||
                               completed.type == ops::OpType::RealDelete) {
                        s->places.RemoveAssignments(completed.sources, true);
                        s->duplicateScan.RemoveDeleted(completed.sources);
                        s->tray.RemoveDeleted(completed.sources);
                    }
                    if (completed.type == ops::OpType::RecycleDelete ||
                        completed.type == ops::OpType::RealDelete ||
                        completed.type == ops::OpType::RestoreRecycle ||
                        completed.type == ops::OpType::EmptyRecycle) {
                        bool recycle_visible = false;
                        ForEachPane(*s, [&](app::Pane& pane) {
                            if (IsRecycleTab(pane.ActiveTab())) recycle_visible = true;
                        });
                        const auto n = static_cast<int64_t>(completed.sources.size());
                        ScheduleRecycleRefresh(*s);
                        if (completed.type == ops::OpType::RecycleDelete)
                            BumpRecycleOccupancy(*s, n);
                        else if (completed.type == ops::OpType::EmptyRecycle)
                            ClearRecycleOccupancy(*s);
                        else if (completed.type == ops::OpType::RestoreRecycle ||
                                 (completed.type == ops::OpType::RealDelete &&
                                  recycle_visible))
                            BumpRecycleOccupancy(*s, -n);
                        RefreshRecycleViews(*s, false);
                    }
                    RefreshStarredViews(*s);
                    RefreshRecentViews(*s);
                }
                if (!tag_metadata_paths.empty())
                    QueueTagAds(*s, BuildTagAdsUpdates(s->places, tag_metadata_paths, true));
                for (const auto& finished : finished_tasks) {
                    FinishOperationIntents(*s, finished);
                    NoteArrivalFinished(*s, finished);
                    NoteDeleteFinished(*s, finished);
                    NoteBatchRenameFinished(*s, finished);
                    NoteTrayReleaseFinished(*s, finished);
                    if (finished.type == ops::OpType::EmptyRecycle)
                        ApplyEmptyRecycleFinished(*s, finished.success);
                }
                // An op finished: refresh the view (watcher also fires, this is immediate).
                app::Tab* tab = ActiveTab(*s);
                if (tab && refresh) {
                    s->store.MarkDirty(tab->current_path);
                    RefreshActiveTab(*s, RefreshReason::OperationCompleted);
                }
                // The assistant's 查看 tabs have no watch; Ctrl+Z brings files back too.
                if (refresh && !finished_tasks.empty()) AiAssistantRefreshLists(*s);
                // A drag's cards are waiting over another pane: list the
                // dropped rows now rather than on the watcher's next beat.
                if (refresh && !s->dragLanding.names.empty() && !s->dragLanding.reveal_at) {
                    s->store.MarkDirty(s->dragLanding.dir);
                    RefreshPath(*s, s->dragLanding.dir, RefreshReason::OperationCompleted);
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_SHELLCTX_ITEMS: {
        auto* payload = reinterpret_cast<ShellCtxItemsPayload*>(lParam);
        const uint32_t token = static_cast<uint32_t>(wParam);
        if (s && payload) {
            const auto completed = s->context_menu.CompleteComQuery(
                token, std::move(payload->items), GetTickCount64(), payload->partial,
                std::move(payload->answer.handlers));
            if (completed.accepted) {
                bool prefs_changed = false;
                if (!completed.partial) {
                    // The host names only a handler provably hung on its own
                    // at its stuck limit (collect_hung); three such hangs
                    // disable it. Slow-but-finished handlers are not reported:
                    // the menu streams rows as they arrive and never waits.
                    for (const auto& clsid : payload->answer.slow_clsids) {
                        // SendTo is exempt from the auto-disable (#77): it
                        // serializes the whole default menu.
                        if (ipc::IsSendToHandlerClsid(clsid)) continue;
                        if (s->ctxMenuPrefs.RecordComTiming(
                                ipc::HandlerCatalogKey(clsid), 5000))
                            prefs_changed = true;
                    }
                }
                if (prefs_changed) s->ctxMenuPrefs.Save();
                // Partial answers with handler states update the layout too.
                SaveCtxLayoutIfDirty(*s);
                RefreshOpenCtxMenu(*s);
            } else if (!payload->partial) {
                // Late answers of closed menus still feed the layout.
                s->ops.CloseShellMenu(token);
                SaveCtxLayoutIfDirty(*s);
            }
        }
        delete payload;
        return 0;
    }

    case WM_SHELL_VERBS: {
        auto* result = reinterpret_cast<ShellVerbsResult*>(lParam);
        if (s && result) {
            if (s->context_menu.CompleteStaticVerbs(
                    result->ext, std::move(result->verbs), result->generation)) {
                RefreshOpenCtxMenu(*s);
            }
        }
        delete result;
        return 0;
    }

    case WM_CTX_LAYOUT_SEED: {
        auto* seed = reinterpret_cast<CtxLayoutSeed*>(lParam);
        if (s && seed) ApplyCtxLayoutSeed(*s, *seed);
        delete seed;
        return 0;
    }

    case WM_SHELL_VERB_SEED: {
        auto* seed = reinterpret_cast<ShellVerbSeed*>(lParam);
        if (s && seed) ApplyShellVerbSeed(*s, *seed);
        delete seed;
        return 0;
    }

    case WM_SHELL_CACHE_INVALIDATE: {
        if (s) {
            s->context_menu.InvalidateCaches();
            DiscardCtxLayoutSave();
            SeedShellVerbCache(*s);
            SeedCtxLayout(*s);
            // Broad Classes registry changes also include unrelated shell verbs.
            // Application badges refresh on SHCNE_ASSOCCHANGED instead.
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_DEFERRED_STARTUP:
        // Posted by the first window's WM_CREATE, so it runs once the first
        // frame is on screen; wParam is the step. One step per message lets
        // input and frames through between them (each costs 25-35 ms). Quick
        // Look and the operations window stay warm for the first use; both
        // are also built on demand if used earlier.
        if (s) {
            switch (wParam) {
            case 0:
                pulse::app::startup_trace::Mark("deferred_begin");
                RegisterAssocChangeNotify(ServiceWindow(*s));
                pulse::app::startup_trace::Mark("deferred_assoc");
                break;
            case 1:
                s->quickPreview.Initialize(hwnd, WM_QUICK_PREVIEW_NAVIGATE,
                                           WM_QUICK_PREVIEW_OPEN, WM_QUICK_PREVIEW_COMMAND);
                pulse::app::startup_trace::Mark("deferred_quick_look");
                break;
            default:
                EnsureOperationWindow(*s);
                StartTagRules(hwnd);   // 自动规则 watch while Pulse runs
                pulse::app::startup_trace::Mark("deferred_end");
                pulse::app::startup_trace::ScheduleFlush(hwnd, 3000);
                return 0;
            }
            PostMessageW(hwnd, WM_DEFERRED_STARTUP, wParam + 1, 0);
        }
        return 0;

    case WM_REGISTER_DROP_TARGET:
        // Posted by WM_CREATE; WM_DESTROY revokes it.
        if (s && s->dropTarget) RegisterDragDrop(hwnd, s->dropTarget);
        return 0;

    case WM_DEVICE_WATCH_READY:
        if (s) OnDeviceWatchReady(*s, wParam, lParam);
        else CoTaskMemFree(reinterpret_cast<void*>(lParam));
        return 0;
    case WM_DEVICE_FOLDER_CHANGED:
        if (s) OnDeviceFolderChanged(*s, wParam, lParam);
        return 0;
    case WM_ASSOC_CHANGED:
        // Another program (or Settings > Default apps) took over a file type:
        // resolve the thumbnail badges again. Bursts only bump an epoch.
        if (s) {
            s->renderer.InvalidateOpenWithIcons();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_DETAILS_META: {
        auto* result = reinterpret_cast<DetailsMetaResult*>(lParam);
        if (s && result && result->path == s->detailsSelPath) {
            s->detailsSelValid = result->attrs_valid;
            if (result->attrs_valid) {
                s->detailsCreated = result->created;
                s->detailsModified = result->modified;
                s->detailsAccessed = result->accessed;
            }
            s->detailsTypeName = std::move(result->type_name);
            s->detailsMetaPath = result->path;
            s->detailsOwner = std::move(result->meta.owner);
            s->detailsPermissions = std::move(result->meta.permissions);
            s->detailsDrive = std::move(result->meta.drive);
            s->detailsFileSystem = std::move(result->meta.file_system);
            s->detailsFreeSpace = std::move(result->meta.free_space);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_CHANGE_TRACKING:
        if (s) ReceiveChangeTracking(*s, static_cast<uint32_t>(wParam), lParam != 0);
        return 0;

    case WM_INDEX_NOTIFY: {
        if (s) {
            const bool ready = s->index.PinyinReady();
            if (ready && !s->pinyinReadyLast && s->appPrefs.search_pinyin) {
                ForEachPane(*s, [&](app::Pane& pane) {
                    auto* tab = pane.ActiveTab();
                    std::wstring kind, rest;
                    if (!tab || !app::ParsePulsePath(tab->current_path, &kind, &rest) ||
                        kind != L"search" || app::SplitSearchQueryText(rest).content.present()) return;
                    if (tab->snapshot && tab->selected_index >= 0 &&
                        static_cast<size_t>(tab->selected_index) < tab->EntryCount())
                        tab->search_preserve_selection = tab->EntryAt(static_cast<size_t>(tab->selected_index)).full_path;
                    RequestSearchPage(*s, *tab, rest, true);
                });
            }
            s->pinyinReadyLast = ready;
            if (IndexStatusVisible(*s)) InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_NETWORK_INDEX_NOTIFY: {
        if (s && IndexStatusVisible(*s)) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SETTINGS_TASK_RESULT: {
        auto* result = reinterpret_cast<app::SettingsTaskResult*>(lParam);
        if (s && result) {
            if (!result->ok && result->task.kind ==
                    app::SettingsTaskKind::DiagnosticsExport)
                result->error = l10n::Get(l10n::StringId::DiagnosticsExportFailed);
            const auto effect = s->settings.CompleteTask(*result, s->index.ServiceInstalled());
            if (app::IsSettingsTransferTask(result->task.kind))
                FinishSettingsTransfer(app::CurrentWindow(*s), *result);
            if (!result->ok && result->task.kind == app::SettingsTaskKind::ConfigureIndexPath &&
                !result->error.empty()) {
                s->notification_toast.Show(hwnd, l10n::Get(l10n::StringId::IndexLocation), result->error);
            }
            if (effect.refresh_index) s->index.RefreshVolumesAsync();
            if (!effect.pin_network.empty()) {
                s->places.PinNetwork(effect.pin_network, L"");
                RequestUncProbe(*s, effect.pin_network);
            }
            if (!effect.open_path.empty()) {
                ShellExecuteW(hwnd, L"open", effect.open_path.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete result;
        return 0;
    }

    case WM_INDEX_SEARCH: {
        if (!s) return 0;
        const uint32_t id = static_cast<uint32_t>(wParam);
        if (pulse::app::ai::ClaimAiIndexResult(id)) return 0;   // an AI client's search (ai_tools.h)
        index::SearchResult result;
        if (!s->index.TakeResult(id, result)) return 0;
        AcceptIndexProviderResult(SearchResultWindow(*s, id), id, std::move(result), false);
        return 0;
    }

    case WM_NETWORK_INDEX_SEARCH: {
        if (!s) return 0;
        const uint32_t id = static_cast<uint32_t>(wParam);
        index::SearchResult result;
        if (!s->networkIndex.TakeResult(id, result)) return 0;
        AcceptIndexProviderResult(SearchResultWindow(*s, id), id, std::move(result), true);
        return 0;
    }

    case WM_NETWORK_LIVE_SEARCH: {
        auto* live = reinterpret_cast<std::shared_ptr<LiveNetworkSearch>*>(lParam);
        if (s && live && *live)
            AcceptLiveNetworkProgress(SearchResultWindow(*s, (*live)->latest_id.load()), *live);
        delete live;
        return 0;
    }

    case WM_UPDATE_RESULT:
        if (s) CompleteUpdateCheck(*s);
        return 0;
    case WM_UPDATE_DOWNLOADED:
        if (s) CompleteUpdateDownload(*s);
        return 0;
    case WM_SEARCH_HISTORY:
        if (s && s->addressSearching && GetFocus() == s->hwndAddressEdit)
            ShowAddressSearchHistory(*s);
        return 0;
    case WM_COMMAND_PALETTE:
        if (s && s->addressEditing && !s->addressSearching) ShowOmnibar(*s, OmnibarMode::Command);
        return 0;
    case WM_UPDATE_INSTALL:
        if (s) InstallUpdate(*s);
        return 0;
    case WM_SHOW_RELEASE_NOTES:
        if (s) ShowReleaseNotes(*s);
        return 0;

    case WM_CONTENT_SELECTION:
        if(s) CompleteContentSelection(*s);
        return 0;
    case WM_CONTENT_SEARCH: {
        if (!s) return 0;
        // Each update goes to the window whose tab runs that search (M5a).
        std::vector<AppState*> updated{s};
        index::ContentSearchUpdate update;
        while (s->contentSearch.TakeUpdate(update)) {
            AppState& owner = ContentSearchWindow(*s, update.progress.generation);
            ApplyContentSearchUpdate(owner, std::move(update));
            if (std::find(updated.begin(), updated.end(), &owner) == updated.end())
                updated.push_back(&owner);
        }
        for (AppState* window : updated) RefreshContentResults(*window);
        return 0;
    }

    case WM_DUPLICATE_SCAN: {
        if (!s) return 0;
        index::ContentSearchUpdate update;
        while (s->duplicateSearch.TakeUpdate(update))
            s->duplicateScan.ApplyUpdate(update.progress, update.hits);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DUP_VOLUMES: {
        auto* payload = reinterpret_cast<std::vector<index::VolumeInfo>*>(lParam);
        if (s && payload) ApplyDuplicateVolumeCache(*s, std::move(*payload));
        delete payload;
        return 0;
    }

    case WM_QUICK_PREVIEW_NAVIGATE:
        if (s) NavigateQuickPreview(*s, static_cast<int>(wParam));
        return 0;

    case WM_QUICK_PREVIEW_OPEN:
        if (s) {
            // wParam 1: an item inside the previewed folder (Quick Look list).
            const std::wstring inner = wParam == 1 ? s->quickPreview.TakeOpenPath() : std::wstring();
            if (!inner.empty()) OpenPath(*s, inner);
            else OpenSelected(*s);
        }
        return 0;

    case WM_QUICK_PREVIEW_COMMAND:
        if (s) {
            ui::QuickPreviewCommand command;
            if (s->quickPreview.TakeCommand(static_cast<UINT_PTR>(lParam), command))
                HandleQuickPreviewCommand(*s, command);
        }
        return 0;

    case WM_NET_PROBE: {
        auto* result = reinterpret_cast<fs::UncProbeResult*>(lParam);
        if (s && result) {
            if (!s->probe_scheduler.AcceptResult(result->unc, result->probe_id, result->final)) {
                delete result;
                return 0;
            }
            const bool completed_current_probe = s->probe_scheduler.Finish(result->probe_id);
            if (completed_current_probe) s->probeUnc.clear();
            if (!s->probe_scheduler.Accept(result->unc, result->probe_id, result->completed)) {
                PumpUncProbe(*s);
                delete result;
                return 0;
            }
            s->places.SetNetworkStatus(result->unc, result->status, result->rtt_ms);
            // The status is shared; every window showing the share updates.
            const bool offline = result->status == fs::NetStatus::Offline;
            app::ForEachWindowSharingCore(*s, [&](AppState& window) {
                ForEachPane(window, [&](app::Pane& pane) {
                    app::Tab* tab = pane.ActiveTab();
                    if (!tab || tab->current_path != result->unc) return;
                    if (offline) {
                        if (tab->snapshot) {
                            tab->net_readonly = true;
                            tab->banner_title = l10n::Get(l10n::StringId::Offline);
                            tab->banner_message = l10n::Get(l10n::StringId::OfflineSnapshot);
                        }
                        return;
                    }
                    tab->net_readonly = false;
                    if (tab->banner_title == l10n::Get(l10n::StringId::Offline)) {
                        tab->banner_title.clear();
                        tab->banner_message.clear();
                    }
                });
                InvalidateRect(window.hwnd, nullptr, FALSE);
            });
            PumpUncProbe(*s);
        }
        delete result;
        return 0;
    }

    case WM_TAG_ADS_WARNING: {
        std::unique_ptr<app::TagAdsResult> result(reinterpret_cast<app::TagAdsResult*>(lParam));
        if (s && result && app::AcceptTagAdsResult(s->tagAdsResultRevision, result->revision)) {
            for (auto& failure : result->failures) failure.path = ClipboardPath(failure.path);
            ForEachPane(*s, [&](app::Pane& pane) {
                if (auto* tab = pane.ActiveTab())
                    app::ApplyTagAdsNotice(tab->tag_ads_notice, result->failures,
                        tab->banner_title, tab->banner_message, tab == ActiveTab(*s));
            });
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_TAG_ADS_DISCOVERED: {
        std::unique_ptr<std::vector<TagAdsDiscovery>> discoveries(
            reinterpret_cast<std::vector<TagAdsDiscovery>*>(lParam));
        if (s && discoveries) ApplyTagAdsDiscoveries(*s, *discoveries);
        return 0;
    }

    case WM_NEW_WINDOW:
        // Another window at this window's current folder.
        if (s && s->startupComplete && !s->shot.active && !s->menushot) {
            const app::Tab* tab = ActiveTab(*s);
            OpenPulseWindow(*s, WindowInit{tab ? tab->current_path : std::wstring()});
        }
        return 0;

    case WM_NCDESTROY:
        // A window opened by OpenPulseWindow owns its state: retired here and
        // freed on the host's next message, after this window is gone.
        if (s && RetireWindowState(*s)) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;

    case WM_DESTROY: {
        ReleaseDeviceFolderWatch(hwnd);
        // Closing one of several windows (M3f) tears down what the window
        // owns; services, saved state and the shared caches go with the last.
        const bool last_window = !s || s->core.windows.IsLastWindow(hwnd);
        if (last_window) app::shell_tags::RevokeCommandServer();
        if (last_window) StopTagRules();
        if (s) {
            s->framePump.Stop();
            CancelZipJobs(*s);
            if (last_window) {
                app::CancelCloudDownloads();  // 下载并预览 reads end with the app
                app::DeviceRegistry::Instance().Stop();
                HandOffAtExit(*s, hwnd);
                s->probe_scheduler.Clear();
                s->probeQueue.clear();
                s->probeUnc.clear();
#ifdef PULSE_WITH_SELFTEST
                extern void RecordP1StartupRollback(const AppState&);
                RecordP1StartupRollback(*s);
#endif
                ExitTimer timer;
                StopShellWindows(*s);
                StopShellRegistryWatch();
                const uint64_t shell_ms = timer.Lap();
                s->watches.Stop();
                const uint64_t watches_ms = timer.Lap();
                s->folderSizes.Stop();
                const uint64_t sizes_ms = timer.Lap();
                s->settings.ResetUi();
                s->settings.Stop();
                const uint64_t settings_ms = timer.Lap();
                s->update_checker.Stop();
                s->update_installer.Stop();
                const uint64_t updates_ms = timer.Lap();
                s->contentSearch.Stop();
                const uint64_t content_ms = timer.Lap();
                s->duplicateSearch.Stop();
                const uint64_t duplicates_ms = timer.Lap();
                s->networkIndex.Stop();
                const uint64_t network_index_ms = timer.Lap();
                s->changes.client.Stop();
                const uint64_t changes_ms = timer.Lap();
                s->index.Stop();
                const uint64_t index_ms = timer.Lap();
                CancelNetworkLocations(*s);
                CancelLanDiscovery(*s);
                CancelSidebarRefresh(*s);
                const uint64_t locations_ms = timer.Lap();
                s->worker.Stop();
                diagnostics::runtime::Event("exit_services", {{"shell", shell_ms}, {"watches", watches_ms},
                    {"folder_sizes", sizes_ms}, {"settings", settings_ms}, {"updates", updates_ms},
                    {"content", content_ms}, {"duplicates", duplicates_ms},
                    {"network_index", network_index_ms}, {"changes", changes_ms}, {"index", index_ms},
                    {"locations", locations_ms}, {"worker", timer.Lap()}});
            } else if (AppState* next = s->core.windows.MostRecentExcept(hwnd)) {
                // Hand the process bindings this window held to the next one
                // (M4a): tray hide/restore and the owner of shell dialogs.
                if (s->tray_controller.Window() == hwnd) {
                    s->tray_controller.Retarget(next->hwnd);
                    InstallTrayRevealHook(*next);
                }
                // File operation progress moves along (M4c).
                if (s->ops.UiWindow() == hwnd) MoveOperationUi(*s, *next);
                // Closing is undoable (plan §8.1): keep the tabs and offer
                // them back in the window used next.
                if (!s->core.exiting) {
                    app::ClosedWindow closed = CaptureClosedWindow(*s, hwnd);
                    if (!closed.layout_tabs.empty()) {
                        std::wstring name = closed.name;
                        closed.closed_seq = ++s->core.close_seq;  // M7f: one timeline with closed tabs
                        closed.closed_tick = GetTickCount64();
                        s->core.closed_windows.Push(std::move(closed));
                        next->notification_toast.ShowSnack(
                            next->hwnd, ui::SnackIcon::Undo, l10n::Get(l10n::StringId::WindowClosedTitle),
                            std::move(name), {{l10n::Get(l10n::StringId::SnackRestore), WM_REOPEN_CLOSED_WINDOW}},
                            L"Ctrl+Shift+T");
                    }
                }
            }
            ShutdownDetailsSizeWalk(*s);

            if (s->dropTarget) {
                RevokeDragDrop(hwnd);
                s->dropTarget->Release();
                s->dropTarget = nullptr;
            }
            s->menu.reset();
            s->operationWindow.reset();
            if (s->hwndAddressEdit) {
                DestroyWindow(s->hwndAddressEdit);
                s->hwndAddressEdit = nullptr;
            }
            if (s->hwndRenameEdit) {
                DestroyWindow(s->hwndRenameEdit);
                s->hwndRenameEdit = nullptr;
            }
            if (s->hwndTagRenameEdit) {
                DestroyWindow(s->hwndTagRenameEdit);
                s->hwndTagRenameEdit = nullptr;
            }
            if (s->hwndFilterEdit) {
                DestroyWindow(s->hwndFilterEdit);
                s->hwndFilterEdit = nullptr;
            }
            AiAssistantDestroy(*s);
            if (s->editFont) {
                DeleteObject(s->editFont);
                s->editFont = nullptr;
            }
            if (s->editBrush) {
                DeleteObject(s->editBrush);
                s->editBrush = nullptr;
            }
            if (last_window) UnregisterAssocChangeNotify();
            s->renderer.SetIconNotifyWindow(nullptr);
            s->renderer.SetCompositor(nullptr);
            if (last_window) ui::ResetRenderCaches(s->core.render_caches);
            s->compositor.Shutdown();
            ReleaseWindowOverlay(*s);
            s->core.windows.Remove(hwnd);
            s->hwnd = nullptr;
            if (!last_window) {
                // Withdraw this window's panes from the shell's window list
                // (M4b) and free select requests it will no longer dispatch.
                MSG pending{};
                while (PeekMessageW(&pending, hwnd, WM_SHELL_SELECT, WM_SHELL_SELECT, PM_REMOVE))
                    delete reinterpret_cast<app::ShellSelectRequest*>(pending.lParam);
                if (AppState* next = s->core.windows.MostRecent()) {
                    SyncShellWindows(*next);
                    SyncVisibleWatches(*next);  // stop watching what only this window showed
                    SyncWindowIdentities(*next);  // M7c: down to one, names and dots go
                }
            }
        }
        if (last_window) PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    __try {
        return WndProcImpl(hwnd, msg, wParam, lParam);
    } __except (pulse::crash::AddBreadcrumb(1, msg, static_cast<int32_t>(GetExceptionCode())),
                pulse::crash::ReportFatal(GetExceptionInformation(), "wndproc")) {
        TerminateProcess(GetCurrentProcess(), GetExceptionCode());
        return 0;
    }
}

// Applies the first window's listing before its first paint, waiting up to
// `budget` for the worker (a local folder takes a few ms); a slow or network
// folder paints empty and fills in later.
static void AwaitFirstListing(AppState& s, std::chrono::milliseconds budget) {
    const app::Tab* tab = ActiveTab(s);
    const bool wait = tab && tab->loading && !fs::IsUncPath(tab->current_path);
    const auto until = std::chrono::steady_clock::now() + budget;
    while (wait && std::chrono::steady_clock::now() < until) {
        {
            std::lock_guard<std::mutex> lock(s.resultMutex);
            if (!s.results.empty()) break;
        }
        Sleep(1);
    }
    ProcessPendingResults(s);
    ClampScroll(s);
    pulse::app::startup_trace::Mark("first_listing");
}

bool WaitForShotReady(AppState& s) {
    auto deadline = s.shot.start + std::chrono::seconds(5);
    MSG msg{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ProcessPendingResults(s);
        app::Tab* tab = ActiveTab(s);
        if (tab && !tab->loading && tab->snapshot) {
            // PULSE_TEST_SHOT_SELECT=name (isolated instances): select that row
            // with the details panel open, so the shot shows its well;
            // PULSE_TEST_SHOT_DOWNLOAD=1 then presses 下载并预览 in it. Retried
            // while settling: the rows can land after the snapshot does.
            static bool shot_selected = false;
            const auto select_for_shot = [&s] {
                wchar_t pick[260]{};
                const DWORD pick_len = GetEnvironmentVariableW(L"PULSE_TEST_SHOT_SELECT", pick, ARRAYSIZE(pick));
                if (shot_selected || !s.isolatedTest || pick_len == 0 || pick_len >= ARRAYSIZE(pick)) return;
                app::Tab* shown = ActiveTab(s);
                if (!shown || shown->loading || !shown->snapshot) return;
                for (size_t i = 0; i < shown->EntryCount(); ++i) {
                    if (shown->EntryAt(i).name != pick) continue;
                    shot_selected = true;
                    shown->SelectIndices({static_cast<int>(i)});
                    s.showDetailsPanel = true;
                    s.renderer.SetDetailsPanelVisible(true);
                    if (GetEnvironmentVariableW(L"PULSE_TEST_SHOT_DOWNLOAD", nullptr, 0) > 0) {
                        std::wstring full = shown->current_path;
                        if (!full.empty() && full.back() != L'\\') full += L'\\';
                        StartCloudPreviewDownload(s, full + shown->EntryAt(i).name, shown->EntryAt(i).size);
                    }
                    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
                    return;
                }
            };
            select_for_shot();
            // PULSE_TEST_SHOT_SETTLE_MS: keep pumping after the folder loaded so
            // asynchronous results (thumbnails from the preview host) land in the shot.
            wchar_t settle[16]{};
            if (GetEnvironmentVariableW(L"PULSE_TEST_SHOT_SETTLE_MS", settle, 16)) {
                const auto until = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds((std::min)(_wtoi(settle), 15000));
                while (std::chrono::steady_clock::now() < until) {
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    ProcessPendingResults(s);
                    select_for_shot();
                    if (s.hwnd) RedrawWindow(s.hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_INTERNALPAINT);
                    Sleep(20);
                }
            }
            return true;
        }
        if (s.hwnd) {
            RedrawWindow(s.hwnd, nullptr, nullptr, RDW_UPDATENOW | RDW_INTERNALPAINT);
        }
        Sleep(20);
    }
    return false;
}

// Staged tag states for GUI-verification shots. Kept out of ShotModeMain's
// SEH frame, which forbids C++ locals with destructors.
void StageTagShotStates(AppState& state) {
    if (state.shot_tag_rename && !state.places.tags.empty()) {
        // Point the rename state at a real loaded tag (read-only): the
        // renderer draws the Fluent field frame on that row.
        state.tagRenameId =
            state.places.tags[1 % state.places.tags.size()].id;
    }
    if (state.shot_tag_drag && state.places.tags.size() >= 4) {
        ui::WindowViewModel vm0 = BuildVm(state);
        int g0 = -1;
        for (int g = 0; g < static_cast<int>(vm0.sidebar.size()); ++g) {
            if (vm0.sidebar[g].id == static_cast<int>(app::SidebarSectionId::Tags)) {
                g0 = g;
                break;
            }
        }
        D2D1_RECT_F a{}, b{};
        const bool okA = g0 >= 0 &&
            state.renderer.TagItemRect(vm0, static_cast<float>(state.compositor.Width()),
                                       static_cast<float>(state.compositor.Height()),
                                       g0, 0, &a);
        const bool okB = g0 >= 0 &&
            state.renderer.TagItemRect(vm0, static_cast<float>(state.compositor.Width()),
                                       static_cast<float>(state.compositor.Height()),
                                       g0, 1, &b);
        if (okA && okB) {
            // The third tag held one row up, mid-drag, on the shared sidebar
            // row gesture: its slot is open above the first tag.
            const float pitch = b.top - a.top;
            D2D1_RECT_F held{};
            if (state.renderer.TagItemRect(vm0, static_cast<float>(state.compositor.Width()),
                                           static_cast<float>(state.compositor.Height()),
                                           g0, 2, &held)) {
                const ui::HitTestResult hit = state.renderer.HitTest(vm0,
                    D2D1::RectF(0, 0, static_cast<float>(state.compositor.Width()),
                                static_cast<float>(state.compositor.Height())),
                    (held.left + held.right) * 0.5f, (held.top + held.bottom) * 0.5f);
                POINT cursor{};
                if (GetCursorPos(&cursor)) ScreenToClient(state.hwnd, &cursor);
                state.pinDragActive = true;
                state.pinDragPath = hit.path;
                state.pinDragRun = hit.index;
                // The renderer reads the travel from the live cursor.
                state.pinDragStartPt = POINT{ cursor.x, cursor.y + static_cast<LONG>(pitch * 1.5f) };
                state.pinDragToIndex = 1;
                state.pinGapVisible = true;
                state.pinGapLineY = b.top;
            }
        }
    }
}

// No C++ objects with destructors here: SEH (__try/__except) forbids unwinding.
// Verification hook for --shot-tray: PULSE_SHOT_TRAY_ACTION=<action>:<ms>.
static void ApplyShotTrayAction(AppState& state) {
    wchar_t tray_action[64]{};
    if (state.shot_tray &&
        GetEnvironmentVariableW(L"PULSE_SHOT_TRAY_ACTION", tray_action, ARRAYSIZE(tray_action))) {
        // Verification hook: <action>:<ms> captures the card stack in the
        // middle of an animation (throw, back, drag:<dx>, dismiss, clear, hover).
        int ms = 0;
        if (wchar_t* colon = wcschr(tray_action, L':')) { ms = _wtoi(colon + 1); *colon = L'\0'; }
        for (int k = 0; k < 60; ++k) { TickTrayDeck(state); Sleep(16); }
        const auto top = TrayDeckEntries(state.tray, static_cast<size_t>(TrayStackTop(state)), 1);
        if (wcscmp(tray_action, L"throw") == 0) {
            ThrowTrayTop(state, 1.0f, 0.0f, 0.0f);
        } else if (wcscmp(tray_action, L"back") == 0) {
            TrayStepBack(state);
        } else if (wcscmp(tray_action, L"drag") == 0 && !top.empty()) {
            state.trayDrag.active = true;
            state.trayDrag.path = top.front().item->path;
            state.trayDrag.dx = static_cast<float>(ms);
            state.trayDrag.dy = 10.0f;
            ms = 400; // let the card settle into the held pose
        } else if (wcscmp(tray_action, L"dismiss") == 0 && !top.empty()) {
            MarkTrayExit(state, { top.front().item->path }, false);
            SpawnTrayPuffs(state);
            state.tray.RemoveItem(static_cast<size_t>(top.front().batch),
                                  static_cast<size_t>(top.front().sub));
        } else if (wcscmp(tray_action, L"clear") == 0) {
            std::vector<std::wstring> all;
            for (const auto& b : state.tray.batches())
                for (const auto& item : b.items) all.push_back(item.path);
            MarkTrayExit(state, all, true);
            SpawnTrayPuffs(state);
            state.tray.Clear();
        } else if (wcscmp(tray_action, L"hover") == 0) {
            state.hoverRegion = static_cast<int>(ui::HitTestResult::TrayCard);
            ms = std::max(ms, 400);
        }
        const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(std::max(0, ms));
        while (GetTickCount64() < until) { TickTrayDeck(state); Sleep(4); }
        TickTrayDeck(state);
    }
}

// GUI verification for two-pane layouts: PULSE_SHOT_SPLIT_PATH opens that
// folder in a second pane (PULSE_SHOT_SPLIT_STACKED=1: top/bottom instead of
// left/right; PULSE_SHOT_SPLIT_FOCUS=1 focuses the second pane).
static void StageSplitShot(AppState& state) {
    wchar_t second[MAX_PATH]{}, flag[4]{};
    if (!state.isolatedTest ||
        !GetEnvironmentVariableW(L"PULSE_SHOT_SPLIT_PATH", second, ARRAYSIZE(second))) return;
    const bool stacked = GetEnvironmentVariableW(L"PULSE_SHOT_SPLIT_STACKED", flag, ARRAYSIZE(flag)) > 0;
    ApplyLayoutPreset(state, stacked ? app::LayoutPreset::TwoHorizontal : app::LayoutPreset::TwoVertical);
    std::vector<app::Pane*> visible;
    if (Root(state)) Root(state)->CollectPanes(visible);
    if (visible.size() != 2 || !visible[1]->ActiveTab()) return;
    app::Tab* tab = visible[1]->ActiveTab();
    StartLoadingPath(state, *tab, second);
    if (GetEnvironmentVariableW(L"PULSE_SHOT_SPLIT_FOCUS", flag, ARRAYSIZE(flag)) > 0)
        FocusPane(state, visible[1]);
    // PULSE_SHOT_SPLIT_SWAP=1 swaps the panes; PULSE_SHOT_SPLIT_HOVER=1|2 shows the
    // divider hover (1) or the swap button hover (2).
    if (GetEnvironmentVariableW(L"PULSE_SHOT_SPLIT_SWAP", flag, ARRAYSIZE(flag)) > 0)
        SwapSplitPanes(state);
    if (GetEnvironmentVariableW(L"PULSE_SHOT_SPLIT_HOVER", flag, ARRAYSIZE(flag)) > 0) {
        state.hoverRegion = static_cast<int>(ui::HitTestResult::Splitter);
        state.hoverControlIndex = flag[0] == L'2' ? ui::kSplitterSwapIndex : 0;
    }
    MSG msg{};
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < until && (tab->loading || !tab->snapshot)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        ProcessPendingResults(state);
        Sleep(20);
    }
}

static void StageLinkPillShot(AppState& state) {
    wchar_t mode[24]{}, index[16]{};
    if (!state.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_LINK_PILL_SHOT", mode, ARRAYSIZE(mode)) ||
        !state.pane || !state.pane->ActiveTab()) return;
    // Apply preference-derived visibility before staging selection: the first
    // view-model fill may clear selection when those preferences change.
    BuildVm(state);
    auto* tab = state.pane->ActiveTab();
    if (!tab->snapshot || tab->snapshot->empty()) return;
    GetEnvironmentVariableW(L"PULSE_TEST_LINK_PILL_INDEX", index, ARRAYSIZE(index));
    const int row = std::clamp(_wtoi(index), 0, static_cast<int>(tab->snapshot->size()) - 1);
    tab->ClearSelection();
    state.hoverRow = -1;
    state.hoverPaneIndex = -1;
    if (wcscmp(mode, L"selected") == 0) tab->SelectOnly(row);
    if (wcscmp(mode, L"hover") == 0 || wcscmp(mode, L"closed") == 0) {
        state.hoverRow = row;
        state.hoverPaneIndex = 0;
    }
    Render(state);
    Sleep(250);
    Render(state);
    if (wcscmp(mode, L"closed") == 0) {
        state.hoverRow = -1;
        state.hoverPaneIndex = -1;
        Render(state);
        Sleep(250);
        Render(state);
    }
}

int ShotModeMain(AppState& state, HWND hwnd) {
    bool ok = false;
    __try {
        WaitForShotReady(state);
        StageTagShotStates(state);
        StageSplitShot(state);
        if (state.shot_details && state.pane && state.pane->ActiveTab() &&
            state.pane->ActiveTab()->snapshot &&
            !state.pane->ActiveTab()->snapshot->empty()) {
            // Select rows after enumeration so the panel has deterministic data.
            state.pane->ActiveTab()->SelectOnly(0);
            if (state.shot_details_multi) {
                const int count = static_cast<int>(state.pane->ActiveTab()->snapshot->size());
                for (int i = 1; i < std::min(3, count); ++i)
                    state.pane->ActiveTab()->ToggleSelect(i);
            }
        }
        MSG msg{};
        for (int i = 0; i < 20; ++i) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(40);
        }
        ApplyShotTrayAction(state); // separate frame: __try forbids unwinding objects
        if (g_shot_ai >= 0) {
            pulse::StageAiShot(g_shot_ai);
            Render(state);   // the snapshot reads the frame before the last Present
        }
        StageLinkPillShot(state);
        if (state.shot_tooltip) {
            // After the pump: a mouse move during startup would clear this.
            state.hoverRegion = static_cast<int>(ui::HitTestResult::SidebarItem);
            state.hoverLabel = L"Project";
            state.hoverPoint = POINT{ 60, 320 };
            state.tooltipText = state.hoverLabel;
        }
        if (g_shot_favorites_hover >= 0 || g_shot_favorites_hover == -2) {
            state.hoverRegion = static_cast<int>(g_shot_favorites_hover == -2 ? ui::HitTestResult::AddressFavorites
                                                                               : ui::HitTestResult::FavoritesBarChevron);
            state.hoverControlIndex = g_shot_favorites_hover;
        }
        if (g_shot_favorites_drop >= 0) state.dropFavorite = g_shot_favorites_drop;
        if (g_shot_favorites_focus >= 0 && CycleFavoritesBarFocus(state))
            for (int step = 0; step < g_shot_favorites_focus; ++step) HandleFavoritesBarKey(state, VK_RIGHT);
        RunHoverBench(state);  // --bench-hover: env PULSE_BENCH_HOVER_LIST / _OUT
        if (state.shot_hover_dwell >= 0 && StageHoverDwellShot(state, state.shot_hover_dwell)) {
            for (int i = 0; i < 2; ++i) {
                Render(state);
                Sleep(20);
            }
        }
        if (state.shot_hover_preview >= 0 && StageHoverPreviewShot(state, state.shot_hover_preview)) {
            // Let the card's thumbnail or text excerpt decode before the shot.
            for (int i = 0; i < (state.hoverPreview.video || state.hoverPreview.folder ? 40 : 15); ++i) {
                Render(state);
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(60);
            }
        }
        Render(state);
        ok = state.compositor.SaveSnapshot(state.shot.output.c_str());
    } __except (pulse::crash::ReportFatal(GetExceptionInformation(), "shot")) {
        return 2;
    }
    if (ok && (state.shot_hover_preview >= 0 || state.shot_hover_dwell >= 0)) WriteHoverVideoProbe(state, state.shot.output);
    if (ok) {
        // Drop timing numbers next to the shot for the phase report.
        wchar_t timingPath[MAX_PATH]{};
        wcsncpy_s(timingPath, state.shot.output.c_str(), _TRUNCATE);
        if (wchar_t* dot = wcsrchr(timingPath, L'.')) *dot = L'\0';
        wcscat_s(timingPath, L".timing.txt");
        if (HANDLE f = CreateFileW(timingPath, GENERIC_WRITE, 0, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); f != INVALID_HANDLE_VALUE) {
            char buf[256];
            int n = snprintf(buf, sizeof(buf),
                "path=%ls\nfirst_frame_ms=%.2f\nenum_sort_done_ms=%.2f\n",
                state.pane && state.pane->ActiveTab() ? state.pane->ActiveTab()->current_path.c_str() : L"",
                state.timing.first_frame_ms, state.timing.sort_done_ms);
            if (n > 0) {
                DWORD written = 0;
                WriteFile(f, buf, (DWORD)n, &written, nullptr);
            }
            CloseHandle(f);
        }
    }
    DestroyWindow(hwnd);
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return ok ? 0 : 1;
}

// Hidden: pulse.exe --picker-live file|folder|image [--dark] [--init <path>]
// [--out <file>] opens Pulse's open dialog on its own, the way an app's
// IFileOpenDialog would, and writes "picked:<path>..." or "cancel" (UTF-8).
int RunPickerLive(int first) {
    std::wstring kind = first < __argc && __wargv[first][0] != L'-' ? __wargv[first] : L"file";
    bool dark = false;
    std::wstring init, out;
    for (int i = first; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--dark") == 0) dark = true;
        else if (wcscmp(__wargv[i], L"--init") == 0 && i + 1 < __argc) init = __wargv[++i];
        else if (wcscmp(__wargv[i], L"--out") == 0 && i + 1 < __argc) out = __wargv[++i];
    }
    ui::FolderPickerSpec spec;
    spec.mode = kind == L"folder" ? ui::PickerMode::Folder
              : kind == L"image" ? ui::PickerMode::Image : ui::PickerMode::File;
    spec.initial_path = init;
    if (spec.mode == ui::PickerMode::File) {
        spec.allow_multiselect = true;
        spec.filters = {{l10n::Get(l10n::StringId::AllFiles_2).c_str(), L"*.*"},
                        {l10n::Get(l10n::StringId::TextDocumentsTxtMd).c_str(), L"*.txt;*.md"},
                        {l10n::Get(l10n::StringId::PickerPictures).c_str(), L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp"}};
    }
    const D2D1_COLOR_F accent = ui::HexColor(0x0078D4);
    ui::FilePickerResult result;
    const bool picked = ui::ShowFilePicker(nullptr, spec, dark, accent, result);
    std::wstring text = picked ? L"picked:" : L"cancel";
    for (size_t i = 0; i < result.paths.size(); ++i) {
        if (i) text += L"\n";
        text += result.paths[i];
    }
    if (!out.empty()) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(bytes > 1 ? bytes - 1 : 0), '\0');
        if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
        FILE* f = nullptr;
        if (_wfopen_s(&f, out.c_str(), L"wb") == 0 && f) {
            fwrite(utf8.data(), 1, utf8.size(), f);
            fclose(f);
        }
    }
    return picked ? 0 : 1;
}

bool SkipSingletonFromArgv() {
#ifdef PULSE_WITH_SELFTEST
    // Exercise the normal persistence branch without forwarding a fault test
    // to a real running instance. GetPulseDataDir redirects every fixture.
    if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) > 0 &&
        GetEnvironmentVariableW(L"PULSE_TEST_STARTUP_FAIL", nullptr, 0) > 0) return true;
#endif
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--selftest") == 0 ||
            wcscmp(__wargv[i], L"--picker-live") == 0 ||
            wcscmp(__wargv[i], L"--material-selftest") == 0 ||
            wcscmp(__wargv[i], L"--seed-shell-verbs") == 0 ||
            wcscmp(__wargv[i], L"--shot") == 0 ||
            wcscmp(__wargv[i], L"--menushot") == 0 ||
            wcscmp(__wargv[i], L"--test-instance") == 0 ||
            wcscmp(__wargv[i], L"--colorpickshot") == 0 ||
            wcscmp(__wargv[i], L"--tagmanagershot") == 0 ||
            wcscmp(__wargv[i], L"--colorpickdialog") == 0)
            return true;
    }
    return false;
}

// A switch, not a folder: Pulse's own options, and the OLE ones COM passes
// to a local server (-Embedding, /Automation, ...). The last argument used to
// be taken as the folder whatever it was, so `pulse.exe -Embedding` opened
// <current directory>\-Embedding (#248, #250).
static bool IsLaunchSwitch(const wchar_t* arg) {
    if (!arg || !*arg) return true;
    if (arg[0] == L'-') return true;
    if (arg[0] != L'/') return false;
    for (const wchar_t* name : {L"/Embedding", L"/Automation", L"/RegServer", L"/UnregServer"})
        if (_wcsicmp(arg, name) == 0) return true;
    return false;
}

#ifdef PULSE_WITH_TEST_HOOKS
int RunFilePickerDialogTest(int argc, wchar_t** argv);
int RunFilePickerOperationsTest();
#endif
#ifdef PULSE_WITH_SELFTEST
int RunUiModuleAuditTest();
int RunQuickPreviewAuditTest();
#endif

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    // pulse.exe --mcp-stdio, started by AI clients: only the launcher, before
    // anything of the app (no window, no D2D, no single instance).
    if (pulse::app::ai::IsMcpStdioLaunch(__argc, __wargv)) return pulse::app::ai::RunMcpStdioLauncher(hInstance);
    pulse::app::startup_trace::Mark("wwinmain");
#ifdef PULSE_WITH_SELFTEST
    std::optional<CLSID> tag_com_audit_class;
    if (!app::shell_tags::PrepareCommandAudit(__argc, __wargv, tag_com_audit_class)) return 2;
#endif
#ifdef PULSE_WITH_SELFTEST
    if (__argc == 2 && wcscmp(__wargv[1], L"--quick-preview-audit") == 0)
        return RunQuickPreviewAuditTest();
    if (__argc == 2 && wcscmp(__wargv[1], L"--ui-module-audit") == 0)
        return RunUiModuleAuditTest();
    if (__argc == 2 && wcscmp(__wargv[1], L"--startup-rollback-test") == 0)
        return RunStartupRollbackTest(hInstance, WndProc);
#endif
#ifdef PULSE_WITH_TEST_HOOKS
    if (__argc == 2 && wcscmp(__wargv[1], L"--picker-operations-test") == 0)
        return RunFilePickerOperationsTest();
    if (__argc >= 3 && wcscmp(__wargv[1], L"--picker-dialog-test") == 0)
        return RunFilePickerDialogTest(__argc - 1, __wargv + 1);
#else
    // Release builds leave the picker suites out; fail fast instead of
    // opening a window that a test script would wait on.
    if (__argc >= 2 && (wcscmp(__wargv[1], L"--picker-operations-test") == 0 ||
                        wcscmp(__wargv[1], L"--picker-dialog-test") == 0))
        return 2;
#endif
    pulse::crash::Initialize({pulse::crash::ProcessRole::App, false, {}});
    pulse::app::startup_trace::Mark("crash_handler");
    pulse::compat::EnableDpiAwareness();
    // OLE init (drag & drop + clipboard); implies STA COM init.
    OleInitialize(nullptr);
    pulse::app::startup_trace::Mark("ole");
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--material-selftest") == 0) {
            const int rc = ui::RunMaterialSelfTest();
            OleUninitialize();
            return rc;
        }
        if (wcscmp(__wargv[i], L"--picker-live") == 0) {
            l10n::Initialize(hInstance, L"system");
            const int rc = RunPickerLive(i + 1);
            OleUninitialize();
            return rc;
        }
        if (wcscmp(__wargv[i], L"--seed-shell-verbs") == 0) {
            // The cache stores display text, and the synthesized rows (打开方式…)
            // come from the resource strings: seeding without them wrote empty
            // rows that the menu then dropped.
            l10n::Initialize(hInstance, L"system");
            const int rc = app::SeedMachineStaticVerbCache() ? 0 : 1;
            OleUninitialize();
            return rc;
        }
    }

    AppState state;
    pulse::app::startup_trace::Mark("appstate");
    state.safeMode = pulse::crash::SafeModeRequested();
    for (int i = 1; i < __argc; ++i)
        if (wcscmp(__wargv[i], L"--test-instance") == 0) state.isolatedTest = true;
#ifdef PULSE_WITH_SELFTEST
    // Persistence checks: a test instance may save, but only into its own data dir.
    if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) > 0 &&
        GetEnvironmentVariableW(L"PULSE_TEST_PERSIST", nullptr, 0) > 0)
        state.isolatedTestPersist = true;
    if (tag_com_audit_class) state.isolatedTestPersist = false;
#endif
    for (int i = 1; i < __argc; ++i)
        if (state.isolatedTest && wcscmp(__wargv[i], L"--content-index-observer") == 0) state.contentIndexObserver = true;

#ifdef PULSE_WITH_SELFTEST
    // Optional headless suite: excluded from production builds together with
    // the in-process index engine it exercises.
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--selftest") == 0) {
            int rc = app::RunSelfTest1B2();
            OleUninitialize();
            return rc;
        }
    }
#endif

    // Load previous session before parsing overrides.
    app::SessionSnapshot session;
    if ((!state.isolatedTest || state.isolatedTestPersist) && app::LoadSession(session)) {
        state.session_path = session.active_path;
        state.session_layout_tabs = std::move(session.layout_tabs);
        state.session_tab_groups = std::move(session.tab_groups);
        state.session_active_layout_tab = session.active_layout_tab;
        // Session v8 (M4d): the single-window fields above are the focused
        // window; the others open after the first window is shown.
        if (session.focused_window >= 0 && session.focused_window < static_cast<int>(session.windows.size()))
            state.identity = session.windows[static_cast<size_t>(session.focused_window)].identity;  // M7c
        for (size_t i = 0; i < session.windows.size(); ++i)
            if (static_cast<int>(i) != session.focused_window)
                state.core.pending_session_windows.push_back(std::move(session.windows[i]));
        // M7f: Ctrl+Shift+T and the overview can still bring these back.
        for (const auto& closed : session.closed_windows)
            state.core.closed_windows.Push(app::ClosedWindowFromSession(closed, ++state.core.close_seq));
        state.tray = session.tray;
        state.darkMode = session.dark;
        state.sidebarCollapsedMask = static_cast<uint32_t>(session.sidebar_collapsed);
        state.sidebarHiddenMask = static_cast<uint32_t>(session.sidebar_hidden);
        state.sidebarOrder = session.sidebar_order.empty()
            ? app::DefaultSidebarOrder() : session.sidebar_order;
        state.sidebarQuickAccessHiddenMask = static_cast<uint32_t>(session.quick_access_hidden);
        state.starredExpanded = session.starred_expanded;
        state.pending_undo_json = session.undo_json;
        state.showDetailsPanel = session.details_panel;
        state.detailsPreviewOnly = session.details_preview_only;
        state.detailsPreviewEnabled = session.details_preview;
        state.detailsPreviewExpansion = session.details_preview_only ? 1.0f : 0.0f;
        state.detailsPanelWidth = static_cast<float>(session.details_panel_width);
        state.renderer.SetDetailsPanelWidth(state.detailsPanelWidth);
        if (state.darkMode) state.themeOverride = ui::ThemeMode::Dark;
    }

    pulse::app::startup_trace::Mark("session");
    // Parse command line.
    unsigned long restart_after = 0;  // --restart-after <pid>
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--shot") == 0 && i + 1 < __argc) {
            state.shot.active = true;
            state.shot.output = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--menushot") == 0 && i + 1 < __argc) {
            state.menushot = true;
            state.menushot_out = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--menushot-search") == 0) {
            state.menushot_search = true;
        } else if ((wcscmp(__wargv[i], L"--colorpickshot") == 0 ||
                    wcscmp(__wargv[i], L"--tagmanagershot") == 0) && i + 1 < __argc) {
            state.colorpickshot = true;
            state.colorpickshot_tags = wcscmp(__wargv[i], L"--tagmanagershot") == 0;
            state.colorpickshot_out = __wargv[++i];
            if (i + 1 < __argc && wcscmp(__wargv[i + 1], L"light") == 0) {
                state.colorpickshot_dark = false;
                ++i;
            }
        } else if (wcscmp(__wargv[i], L"--colorpickdialog") == 0) {
            state.colorpickdialog = true;
        } else if (wcscmp(__wargv[i], L"--shot-tray") == 0) {
            state.shot_tray = true;
            // Optional item count: --shot-tray 1 stages a single file.
            if (i + 1 < __argc && __wargv[i + 1][0] >= L'0' && __wargv[i + 1][0] <= L'9')
                state.shot_tray_count = std::max(1, _wtoi(__wargv[++i]));
        } else if (wcscmp(__wargv[i], L"--shot-tab-colors") == 0) {
            state.shot_tab_colors = true;
        } else if (wcscmp(__wargv[i], L"--shot-pinned-tab") == 0) {
            state.shot_pinned_tab = true;
        } else if (wcscmp(__wargv[i], L"--shot-tag-rename") == 0) {
            state.shot_tag_rename = true;
        } else if (wcscmp(__wargv[i], L"--shot-tag-drag") == 0) {
            state.shot_tag_drag = true;
        } else if (wcscmp(__wargv[i], L"--shot-favorites-bar") == 0) {
            g_shot_favorites_bar = true;
        } else if (wcscmp(__wargv[i], L"--shot-toolbar-apps") == 0) {
            g_shot_toolbar_apps = true;
        } else if (wcscmp(__wargv[i], L"--shot-ai") == 0 && i + 1 < __argc) {
            g_shot_ai = _wtoi(__wargv[++i]);
        } else if (wcscmp(__wargv[i], L"--shot-favorites-hover") == 0 && i + 1 < __argc) {
            g_shot_favorites_hover = _wtoi(__wargv[++i]);
        } else if (wcscmp(__wargv[i], L"--shot-favorites-drop") == 0 && i + 1 < __argc) {
            g_shot_favorites_drop = _wtoi(__wargv[++i]);
        } else if (wcscmp(__wargv[i], L"--shot-favorites-focus") == 0 && i + 1 < __argc) {
            g_shot_favorites_focus = _wtoi(__wargv[++i]);
        } else if (wcscmp(__wargv[i], L"--shot-rail") == 0) {
            // GUI verification for the icon rail: every section folded, so a
            // narrow window shows one row per section.
            state.sidebarCollapsedMask = (1u << app::kSidebarSectionCount) - 1u;
        } else if (wcscmp(__wargv[i], L"--shot-tooltip") == 0) {
            // GUI verification for a rail hint; staged right before the shot so
            // the message pump cannot clear it (see ShotModeMain).
            state.shot_tooltip = true;
            state.hoverRegion = static_cast<int>(ui::HitTestResult::SidebarItem);
            state.hoverLabel = L"Project";
            state.hoverPoint = POINT{ 60, 320 };
            state.tooltipText = state.hoverLabel;
        } else if (wcscmp(__wargv[i], L"--shot-hover-preview") == 0 && i + 1 < __argc) {
            // GUI verification for the hover card: over the Nth file (0-based).
            state.shot_hover_preview = std::max(0, _wtoi(__wargv[++i]));
        } else if (wcscmp(__wargv[i], L"--shot-hover-dwell") == 0 && i + 1 < __argc) {
            state.shot_hover_dwell = std::max(0, _wtoi(__wargv[++i]));
        } else if (wcscmp(__wargv[i], L"--shot-details") == 0) {
            state.shot_details = true;
        } else if (wcscmp(__wargv[i], L"--shot-group") == 0 && i + 1 < __argc) {
            state.shot_group = std::clamp(_wtoi(__wargv[++i]), 0, 6);
        } else if (wcscmp(__wargv[i], L"--shot-details-multi") == 0) {
            state.shot_details = true;
            state.shot_details_multi = true;
        } else if (wcscmp(__wargv[i], L"--shot-scale") == 0 && i + 1 < __argc) {
            state.shot_scale_override = std::clamp(
                static_cast<float>(_wtof(__wargv[++i])), 1.0f, 2.5f);
        } else if (wcscmp(__wargv[i], L"--shot-language") == 0 && i + 1 < __argc) {
            state.shot.language = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--shot-update-available") == 0) {
            state.shot.update_available = true;
        } else if (wcscmp(__wargv[i], L"--shot-update-state") == 0 && i + 1 < __argc) {
            state.shot.update_available = true;
            state.shot.update_state = __wargv[++i];
        } else if (wcscmp(__wargv[i], L"--shot-high-contrast") == 0) {
            state.shot_high_contrast = true;
        } else if (wcscmp(__wargv[i], L"--restore-update-session") == 0) {
            state.restoreUpdateSession = true;
        } else if (wcscmp(__wargv[i], kRestartAfterArgument) == 0 && i + 1 < __argc) {
            restart_after = wcstoul(__wargv[++i], nullptr, 10);  // settings_transfer_ui.h
        } else if (wcscmp(__wargv[i], L"--dark") == 0) {
            state.shot.force_dark = true;
            state.themeOverride = ui::ThemeMode::Dark;
        } else if (wcscmp(__wargv[i], L"--light") == 0) {
            state.shot.force_dark = false;
            state.themeOverride = ui::ThemeMode::Light;
        } else if (wcscmp(__wargv[i], L"--test-instance") == 0 || wcscmp(__wargv[i], L"--content-index-observer") == 0 ||
                   wcscmp(__wargv[i], L"--hang-watch") == 0 || wcscmp(__wargv[i], L"--test-ai-broker") == 0 ||
                   wcscmp(__wargv[i], app::kStartupArgument) == 0) {
            continue;
#ifdef PULSE_WITH_SELFTEST
        } else if ((wcscmp(__wargv[i], L"--tag-com-audit-class") == 0 ||
                    wcscmp(__wargv[i], L"--tag-com-audit-dir") == 0) && i + 1 < __argc) {
            ++i;
#endif
        } else if (wcscmp(__wargv[i], L"--tag") == 0 ||
                   wcscmp(__wargv[i], L"--tag-add") == 0 ||
                   wcscmp(__wargv[i], L"--tag-remove") == 0) {
            if (i + 2 >= __argc) return 1;
            i += 2;
        } else if (wcscmp(__wargv[i], L"--fps") == 0) {
            state.forceStatusPerformance = true;
            state.showFps = true;
        } else if (wcscmp(__wargv[i], L"--view") == 0 && i + 1 < __argc) {
            const wchar_t* view = __wargv[++i];
            g_shot_wall = wcscmp(view, L"wall") == 0;
            state.shot.view_mode = g_shot_wall ? ui::ViewMode::LargeIcons : ui::ParseViewMode(view);
        } else if (wcscmp(__wargv[i], L"--size") == 0 && i + 1 < __argc) {
            int requestedWidth = 0;
            int requestedHeight = 0;
            if (swscanf_s(__wargv[++i], L"%dx%d", &requestedWidth, &requestedHeight) == 2) {
                state.shot.width = std::max(320, requestedWidth);
                state.shot.height = std::max(240, requestedHeight);
            }
        } else if (i == __argc - 1 && !IsLaunchSwitch(__wargv[i])) {
            state.shot.path = __wargv[i];
        } else if (!IsLaunchSwitch(__wargv[i]) && state.shot.path.empty()) {
            state.shot.path = __wargv[i]; // tolerate path not being the last argument
        }
    }
    if (state.shot.active && state.shot.path.empty()) {
        state.shot.path = L"C:\\";
    }
    if (!state.shot.active && !state.menushot && !state.colorpickshot &&
        !state.colorpickdialog) {
        state.open_path = state.shot.path;
        state.shot.path.clear();
    }
    state.shot.start = std::chrono::steady_clock::now();

    std::optional<app::ShellTagRequest> shell_tag = app::ParseShellTagArgs(__argc, __wargv);
    for (int i = 1; i < __argc; ++i) {
        const std::wstring_view arg(__wargv[i]);
        if ((arg == L"--tag" || arg == L"--tag-add" || arg == L"--tag-remove") && !shell_tag)
            return 1;
    }
    bool shell_tag_com = false;
    for (int i = 1; i < __argc; ++i) if (wcscmp(__wargv[i], L"--shell-tag-com") == 0) shell_tag_com = true;
    if (shell_tag || shell_tag_com) state.open_path.clear();
    std::wstring normalized_launch;
    if (!app::SingleInstanceCoordinator::NormalizeLaunchPath(state.open_path, normalized_launch)) {
        MessageBoxW(nullptr, L"Invalid launch path.", L"Pulse", MB_OK | MB_ICONERROR);
        return 1;
    }
    state.open_path = std::move(normalized_launch);
    if (shell_tag) {
        if (!app::SingleInstanceCoordinator::NormalizeLaunchPath(shell_tag->path, normalized_launch) ||
            normalized_launch.empty() || normalized_launch.starts_with(L"pulse:") ||
            app::IsThisPcArgument(normalized_launch)) return 1;
        shell_tag->path = std::move(normalized_launch);
    }
    // Launched by the HKCU Run value (app_prefs.cpp), not by the user.
    const bool startup_launch = app::HasStartupArgument(__argc, __wargv);
    // Restarted after 导入设置: the old Pulse still holds the endpoint and
    // writes its last files; take over once it has gone.
    if (restart_after) WaitForRestartingPulse(restart_after);
    if (!SkipSingletonFromArgv()) {
        const auto result = state.single_instance.Acquire();
        if (result == app::SingleInstanceCoordinator::AcquireResult::Failed) {
            MessageBoxW(nullptr, L"Could not establish the Pulse instance endpoint.", L"Pulse", MB_OK | MB_ICONERROR);
            return 1;
        }
        if (result == app::SingleInstanceCoordinator::AcquireResult::Existing) {
            bool forwarded = true;
            if (shell_tag) forwarded = app::ForwardShellTagRequest(*shell_tag);
            // A sign-in launch must not pop up the window that is already running.
            else if (!startup_launch && !shell_tag_com) forwarded = state.single_instance.ForwardOpenPath(state.open_path, 10000);
            if (!forwarded) {
                // This process exits before WM_CREATE initializes localization.
                // Read the preference without repairing startup registration.
                app::AppPrefs failure_prefs;
                failure_prefs.persist = false;
                failure_prefs.Load();
                l10n::Initialize(hInstance, failure_prefs.language);
                const std::wstring message = l10n::HantText(l10n::Get(l10n::StringId::PulseIsBusyTheFolderWillShowUpInTheOpenPulseWindowShortlyIfI).c_str());
                MessageBoxW(nullptr, message.c_str(), L"Pulse", MB_OK | MB_ICONINFORMATION);
            }
            OleUninitialize();
            return forwarded ? 0 : 1;
        }
    }

    // This process owns the window now: create the D3D device and warm the
    // shell's icon cache on worker threads while the main thread builds the
    // windows. On the main thread they cost about 250 ms (NVIDIA's driver
    // load) and 100 ms (the first system image list icon) of the cold start.
    // Started by the Explorer tag verb while Pulse was closed: stay hidden,
    // apply the tag batch, then exit (shell_tag_menu.cpp). Known before the
    // window exists, so its creation can leave out what such a launch never
    // uses until the window is shown.
    // A staged settings import (备份与迁移) lands before anything reads the data dir.
    if (!SkipSingletonFromArgv() && !state.isolatedTest && !state.shot.active && !state.menushot &&
        !shell_tag && !shell_tag_com)
        ApplyStagedSettingsAtStartup();
    if (shell_tag) QueueShellTagRequest(state, *shell_tag, true);
    if (shell_tag_com) BeginShellTagComLaunch();
    pulse::app::startup_trace::Mark("singleton");
    ui::Compositor::PrefetchSharedDevice();
    if (!ShellTagHeadlessLaunch()) ui::ShellIconCache::PrewarmSystemImageList();
    if (!state.isolatedTest && !state.shot.active) app::StartUpdateDownloadCleanup();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_PULSE));
    wc.hIconSm = reinterpret_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(IDI_PULSE), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = app::SingleInstanceCoordinator::WindowClassName();
    RegisterClassExW(&wc);

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = (int)(1600 * state.scale), h = (int)(960 * state.scale);
    if (session.window_rect.right > session.window_rect.left) {
        // Saved from rcNormalPosition: workspace coordinates (window_placement.h).
        RECT restored = app::RestoredWindowRect(session.window_rect);
        // Its monitor is gone (M4d): the primary monitor's work area.
        if (!MonitorFromRect(&restored, MONITOR_DEFAULTTONULL)) {
            MONITORINFO primary{sizeof(primary)};
            if (GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &primary))
                restored = app::PlaceSessionWindow(restored, false, primary.rcWork, {}, 0);
        }
        x = restored.left;
        y = restored.top;
        w = restored.right - restored.left;
        h = restored.bottom - restored.top;
    }
    if (state.shot.active && state.shot.width > 0 && state.shot.height > 0) {
        w = state.shot.width;
        h = state.shot.height;
    }

    // kMainWindowStyle (window_factory.h) is shared with later windows.
    static_assert((kMainWindowStyle & WS_CAPTION) == WS_CAPTION &&
                  (kMainWindowStyle & WS_SYSMENU) == 0,
                  "keep the system animations without DWM's duplicate caption buttons");
    // Hidden process window (host_window.h), created before the main window
    // so WM_CREATE can point services at it. It owns the single-instance
    // endpoint (Pulse windows do not accept WM_COPYDATA) and forwards the
    // process-level notifications in host_routing.h to the most recently
    // active window (WindowRegistry).
    pulse::app::startup_trace::Mark("class_registered");
    const bool host_created = state.core.host.Create(hInstance,
        [&state](HWND host, UINT message, WPARAM wParam, LPARAM lParam) -> std::optional<LRESULT> {
            AppState* const target = state.core.windows.MostRecent();
            if (message == WM_AI_UI_TASK) {
                // An AI tool reading or showing something in a window (ai_tools_ui.h).
                pulse::app::ai::HandleAiUiTask(target, lParam);
                return 0;
            }
            constexpr UINT_PTR kAiChipTimer = 0x41494348;   // 'AICH'
            if (message == WM_AI_STATUS || (message == WM_TIMER && wParam == kAiChipTimer)) {
                // The AI chip in each status bar (ai_status.h); "done" fades by itself.
                if (message == WM_AI_STATUS) pulse::app::ai::AckAiStatusMessage();
                else KillTimer(host, kAiChipTimer);
                state.core.windows.ForEach([](HWND window, AppState&) { InvalidateRect(window, nullptr, FALSE); });
                if (const uint64_t wait = pulse::app::ai::AiChipSettlesIn(GetTickCount64()))
                    SetTimer(host, kAiChipTimer, static_cast<UINT>(wait + 50), nullptr);
                return 0;
            }
            if (app::IsHostRoutedMessage(message))
                return target ? SendMessageW(target->hwnd, message, wParam, lParam) : 0;
            if (const auto kind = app::ClassifySystemBroadcast(message, wParam, lParam);
                kind != app::SystemBroadcast::None) {
                if (target) return HandleSystemBroadcast(*target, kind, wParam);
                return message == WM_QUERYENDSESSION ? TRUE : 0;
            }
            if (message != 0 && message == app::TrayController::TaskbarCreatedMessage()) {
                state.tray_controller.HandleTaskbarCreated();  // the icon lives on the host
                return 0;
            }
            if (message == WM_RELEASE_WINDOW_STATES) {
                ReleaseRetiredWindowStates();
                return 0;
            }
            if (message == WM_OPEN_EXTERNAL) {
                // QueueExternalOpen: the window most recently used now.
                std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(lParam));
                if (path && target) OpenExternalFolder(*target, *path);
                return 0;
            }
            if (message != WM_COPYDATA) return std::nullopt;
            // A --test-instance publishes no endpoint (a second launch would
            // reach the user's own Pulse), so test scripts send the open
            // message straight to its host window (M7a end-to-end checks).
            if (!target || (!state.single_instance.OwnsEndpoint(host) && !state.isolatedTest)) return FALSE;
            return HandleInstanceCopyData(*target, reinterpret_cast<COPYDATASTRUCT*>(lParam));
        });
    if (!host_created) return 1;
    pulse::app::startup_trace::Mark("host_window");
    // The first frame goes up before the window is activated: activation
    // loads the IME's text services (about 90 ms with a Chinese IME, per the
    // startup trace), which used to sit between ShowWindow and the first
    // paint. No ShowWindow command maximizes without activating, so a
    // maximized start is created maximized (WS_MAXIMIZE) and shown as is;
    // without a saved rect it keeps the old path, since WS_MAXIMIZE would
    // pin the restored window to 0,0 instead of the default position.
    if (session.maximized) nCmdShow = SW_SHOWMAXIMIZED;
    const bool show_before_activate = (nCmdShow != SW_SHOWMAXIMIZED || x != CW_USEDEFAULT) &&
        !state.shot.active && !state.menushot && !state.colorpickshot &&
        !state.colorpickdialog && !shell_tag && !shell_tag_com &&
        !app::StartsHiddenInTray(startup_launch, state.appPrefs.start_in_tray) &&
        (nCmdShow == SW_SHOWNORMAL || nCmdShow == SW_SHOW || nCmdShow == SW_SHOWDEFAULT ||
         nCmdShow == SW_SHOWMAXIMIZED);
    const bool create_maximized = show_before_activate && nCmdShow == SW_SHOWMAXIMIZED;
    HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName,
        L"Pulse",
        // Pulse paints the entire title bar (WM_NCCALCSIZE makes the whole
        // window client area). WS_CAPTION is what makes DWM play the system
        // minimize / maximize / restore animations; WM_NCACTIVATE keeps the
        // classic caption from being repainted. No WS_SYSMENU: with it DWM
        // also draws its own min / max / close buttons in the extended frame,
        // and they show through the Mica title bar behind Pulse's buttons.
        // Minimize / maximize / close commands, Alt+F4 and resizing do not
        // need it.
        kMainWindowStyle | (create_maximized ? WS_MAXIMIZE : 0),
        x, y, w, h,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) return 1;
    pulse::app::startup_trace::Mark("create_window");
    if (!SkipSingletonFromArgv() && !state.single_instance.PublishEndpoint(state.core.host.Hwnd())) {
        DestroyWindow(hwnd);
        return 1;
    }
    // Diagnostics: sample thread stacks while the UI thread is stalled.
    for (int i = 1; i < __argc; ++i) {
        if (wcscmp(__wargv[i], L"--hang-watch") != 0) continue;
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring log = exe;
        log = log.substr(0, log.find_last_of(L'\\') + 1) + L"pulse_hang.log";
        pulse::app::hang::Start(hwnd, log);
        break;
    }
    if (wc.hIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    if (wc.hIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    wchar_t hidden_shot[4]{};
    const bool test_hidden = (state.shot.active || state.menushot || state.colorpickshot) &&
        GetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", hidden_shot, ARRAYSIZE(hidden_shot)) == 1 &&
        hidden_shot[0] == L'1';
    const CLSID* tag_com_class = nullptr;
    bool tag_com_allowed = !state.isolatedTest;
#ifdef PULSE_WITH_SELFTEST
    if (tag_com_audit_class) { tag_com_class = &*tag_com_audit_class; tag_com_allowed = true; }
#endif
    if (tag_com_allowed && state.appPrefs.shell_tag_menu) {
        const HRESULT registered = app::shell_tags::RegisterCommandServer(tag_com_class);
        if (shell_tag_com && FAILED(registered)) { DestroyWindow(hwnd); OleUninitialize(); return 1; }
    } else if (shell_tag_com) { DestroyWindow(hwnd); OleUninitialize(); return 1; }
    // Sign-in launch with 开机自启时隐藏到托盘: only the tray icon shows; a
    // click restores the window (maximized if the session was).
    const bool start_in_tray = !test_hidden && !shell_tag && !shell_tag_com && !state.shot.active && !state.menushot &&
        !state.colorpickshot && !state.colorpickdialog &&
        app::StartsHiddenInTray(startup_launch, state.appPrefs.start_in_tray) &&
        state.tray_controller.StartHidden(nCmdShow == SW_SHOWMAXIMIZED, state.appPrefs.notify_icon_mode != 2);
    if (!start_in_tray)
        ShowWindow(hwnd, test_hidden || shell_tag || shell_tag_com ? SW_HIDE
                                                  : state.shot.active ? SW_SHOWNORMAL
                                                  : create_maximized ? SW_SHOWNA
                                                  : show_before_activate ? SW_SHOWNOACTIVATE : nCmdShow);
    pulse::app::startup_trace::Mark("show_window");
    // The first listing started in WM_CREATE: put it in the first frame
    // rather than after WM_DEFERRED_STARTUP. Its WM_WORKER_RESULT then finds
    // the queue empty.
    if (show_before_activate) AwaitFirstListing(state, std::chrono::milliseconds(60));
    UpdateWindow(hwnd);
    pulse::app::startup_trace::Mark("update_window");
    if (show_before_activate) {
        SetForegroundWindow(hwnd);
        pulse::app::startup_trace::Mark("activated");
    }

    if (state.menushot) {
        // Render the built-in item menu to a PNG (GUI verification for 1B-2).
        ui::FluentMenu m;
        bool ok = m.Create(hwnd, &state.compositor, state.scale);
        if (ok && state.menushot_search && state.pane && state.pane->ActiveTab()) {
            // BuildFinderItemMenu only checks current_path for the search bit.
            state.pane->ActiveTab()->current_path = L"pulse:search:widgets.json";
        }
        // PULSE_TEST_MENU_SELECT=name|name...: let the folder on the command line
        // list, then select those rows, so the shot shows the rows that depend
        // on the selection (网盘: 始终保留在此设备上 / 释放空间).
        wchar_t menu_select[512]{};
        const DWORD select_len = GetEnvironmentVariableW(L"PULSE_TEST_MENU_SELECT", menu_select, ARRAYSIZE(menu_select));
        if (ok && select_len > 0 && select_len < ARRAYSIZE(menu_select)) {
            if (!state.shot.path.empty() && app::IsThisPcArgument(state.shot.path))
                OpenTabAt(state, std::wstring());  // NewTab would open C: for ""
            else if (!state.shot.path.empty()) NewTab(state, state.shot.path);
            const ULONGLONG until = GetTickCount64() + 5000;
            app::Tab* tab = nullptr;
            while (GetTickCount64() < until) {
                MSG pending;
                while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&pending);
                    DispatchMessageW(&pending);
                }
                ProcessPendingResults(state);
                tab = ActiveTab(state);
                if (tab && !tab->loading && tab->snapshot && tab->EntryCount() > 0) break;
                Sleep(20);
            }
            if (tab && tab->snapshot) {
                const std::wstring wanted = L"|" + std::wstring(menu_select) + L"|";
                std::vector<int> picks;
                for (size_t i = 0; i < tab->EntryCount(); ++i)
                    if (wanted.find(L"|" + tab->EntryAt(i).name + L"|") != std::wstring::npos)
                        picks.push_back(static_cast<int>(i));
                tab->SelectIndices(picks);
            }
        }
        if (ok) {
            m.SetTheme(state.darkMode, state.accentColor);
            auto debug_items = BuildFinderItemMenu(state, true, L"撤销移动 a.txt");
            wchar_t quick_menu[4]{};
            if (GetEnvironmentVariableW(L"PULSE_TEST_QUICK_MENU", quick_menu, ARRAYSIZE(quick_menu)) == 1 &&
                quick_menu[0] == L'1') {
                debug_items = app::BuildBackgroundMenu(false, true, L"撤销移动 a.txt");
                app::AppendBackgroundViewCommands(debug_items, {});
                AppendQuickAccessCommand(state, debug_items,
                    state.shot.path.empty() ? QuickAccessTargets(ActiveTab(state), true)
                                            : std::vector<std::wstring>{state.shot.path});
            }
            for (auto& item : debug_items) {
                if (item.quick_swatches.empty()) continue;
                item.quick_swatches.front().checked = true;
                if (item.quick_swatches.size() > 1) item.quick_swatches[1].mixed = true;
                break;
            }
            // Sample Explorer section: a flat verb plus a software-owned
            // flyout group (renders the › chevron in the shortcut column).
            app::ShellMenuEntry group;
            group.text = L"Bandizip";
            group.children = { { app::CmdShellComBase + 1, L"压缩为 zip", true },
                               { app::CmdShellComBase + 2, L"用 Bandizip 打开", true } };
            std::vector<app::ShellMenuEntry> shell_rows = {
                { app::CmdShellStaticBase + 0, L"打印", true }, group };
            // Long third-party verbs must ellipsize inside the menu width.
            wchar_t long_verb[256]{};
            const DWORD verb_len = GetEnvironmentVariableW(L"PULSE_TEST_MENU_VERB", long_verb,
                                                           ARRAYSIZE(long_verb));
            if (verb_len > 0 && verb_len < ARRAYSIZE(long_verb))
                shell_rows.push_back({ app::CmdShellStaticBase + 1, long_verb, true });
            app::AppendShellSection(debug_items, shell_rows);
            if (GetEnvironmentVariableW(L"PULSE_TEST_SPLIT_MENU", quick_menu, ARRAYSIZE(quick_menu)) == 1) {
                debug_items = app::BuildSplitMenu(static_cast<int>(app::LayoutPreset::TwoVertical));
                debug_items.back().separator_after = true;
                ui::FluentMenuItem compare;
                compare.command = app::CmdCompareToggle;
                compare.text = l10n::Get(l10n::StringId::CompareMenu);
                compare.glyph = L"\xE89F";
                debug_items.push_back(std::move(compare));
            }
            ok = m.SaveDebugSnapshot(state.menushot_out.c_str(), std::move(debug_items));
            // PULSE_TEST_MENU_RUN=<command id>: then run that command on the
            // selection and keep pumping so its worker reports back (end-to-end
            // check of 始终保留 / 释放空间 on a fixture folder).
            wchar_t menu_run[16]{};
            if (ok && GetEnvironmentVariableW(L"PULSE_TEST_MENU_RUN", menu_run, ARRAYSIZE(menu_run)) > 0) {
                DispatchMenuCommand(state, _wtoi(menu_run));
                const ULONGLONG until = GetTickCount64() + 3000;
                while (GetTickCount64() < until) {
                    MSG pending;
                    while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&pending);
                        DispatchMessageW(&pending);
                    }
                    Sleep(20);
                }
            }
        }
        DestroyWindow(hwnd);
        OleUninitialize();
        return ok ? 0 : 1;
    }

    if (state.colorpickshot) {
        // Render the color picker popup (or the tag manager) to a PNG (GUI verification).
        const bool ok = state.colorpickshot_tags
            ? SaveTagManagerSnapshot(state, state.colorpickshot_out.c_str(), state.colorpickshot_dark)
            : ui::ColorPickerPopup::SaveDebugSnapshot(
                  &state.compositor, state.colorpickshot_out.c_str(),
                  state.colorpickshot_dark, state.accentColor, state.scale);
        DestroyWindow(hwnd);
        OleUninitialize();
        return ok ? 0 : 1;
    }

    if (state.colorpickdialog) {
        // Interactive smoke test: open the picker centered on the work area.
        uint32_t rgb = 0x2FDFF1;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        POINT pt{ (work.left + work.right) / 2, (work.top + work.bottom) / 2 };
        ui::ColorPickerPopup::Pick(hwnd, &state.compositor, nullptr, state.scale, pt,
                                   rgb, state.darkMode, rgb);
        DestroyWindow(hwnd);
        OleUninitialize();
        return 0;
    }

#ifdef PULSE_WITH_SELFTEST
    wchar_t p1_probe[32768]{};
    if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) > 0 &&
        GetEnvironmentVariableW(L"PULSE_TEST_P1_FLOW", p1_probe, ARRAYSIZE(p1_probe))) {
        extern int RunP1RegressionProbe(AppState&, const wchar_t*);
        const int result = WaitForShotReady(state) ? RunP1RegressionProbe(state, p1_probe) : 4;
        DestroyWindow(hwnd);
        OleUninitialize();
        return result;
    }
#endif
    if (state.shot.active) {
#ifdef PULSE_WITH_TEST_HOOKS
        wchar_t experience_probe[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_SIZE_EXPERIENCE", experience_probe, ARRAYSIZE(experience_probe))) {
            extern int RunFolderSizeExperienceProbe(AppState&, const wchar_t*);
            const int result = WaitForShotReady(state) ? RunFolderSizeExperienceProbe(state, experience_probe) : 4;
            DestroyWindow(hwnd); OleUninitialize(); return result;
        }
        wchar_t folder_probe[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_FOLDER_THUMB_FLOW", folder_probe, ARRAYSIZE(folder_probe))) {
            extern int RunFolderThumbnailProbe(AppState&, const wchar_t*);
            const int result = WaitForShotReady(state) ? RunFolderThumbnailProbe(state, folder_probe) : 4;
            DestroyWindow(hwnd);
            OleUninitialize();
            return result;
        }
        wchar_t selection_probe[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SELECTION_FLOW", selection_probe, ARRAYSIZE(selection_probe))) {
            extern int RunSelectionInputProbe(AppState&, const wchar_t*);
            const int result = WaitForShotReady(state) ? RunSelectionInputProbe(state, selection_probe) : 4;
            DestroyWindow(hwnd);
            OleUninitialize();
            return result;
        }
#endif
        wchar_t settings_fixture[32]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_EXPANDED",settings_fixture,ARRAYSIZE(settings_fixture)))
            state.settingsExpanded=static_cast<unsigned>(wcstoul(settings_fixture,nullptr,10)) & 0x3f0fu;
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_SCROLL",settings_fixture,ARRAYSIZE(settings_fixture))) {
            auto vm=BuildVm(state,false);
            state.settings.SetScroll(static_cast<float>(_wtof(settings_fixture))*state.scale,
                state.renderer.SettingsMaxScroll(vm,static_cast<float>(state.compositor.Width()),static_cast<float>(state.compositor.Height())));
        }
        wchar_t panel_shot[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_OPTIONS_SHOT", panel_shot, ARRAYSIZE(panel_shot))) {
            ShowAddressSearch(state);
            ShowSearchOptions(state);
        }
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_HISTORY_SHOT", panel_shot, ARRAYSIZE(panel_shot))) {
            state.searchHistory.Record(L"setup-dia", app::MakeSearchPath(L"setup-dia"));
            state.searchHistory.Record(L"项目预算", app::MakeSearchPath(L"content:项目预算"));
            ShowAddressSearch(state);
            ShowAddressSearchHistory(state);
        }
#ifdef PULSE_WITH_SELFTEST
        wchar_t settings_interactions[32768]{};
        wchar_t quicklook_settings[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_QUICKLOOK_SETTINGS", quicklook_settings, ARRAYSIZE(quicklook_settings))) {
            extern int RunQuickLookSettingsTest(AppState&, const wchar_t*);
            const int result = RunQuickLookSettingsTest(state, quicklook_settings);
            DestroyWindow(hwnd); OleUninitialize(); return result;
        }
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_INTERACTIONS",settings_interactions,ARRAYSIZE(settings_interactions))) {
            extern int RunSettingsInteractionTest(AppState&,const wchar_t*);
            const int result=RunSettingsInteractionTest(state,settings_interactions);
            DestroyWindow(hwnd);OleUninitialize();return result;
        }
        wchar_t settings_flow[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SETTINGS_FLOW",settings_flow,ARRAYSIZE(settings_flow))) {
            extern int RunSettingsFlowTest(AppState&,const wchar_t*);
            const int result=RunSettingsFlowTest(state,settings_flow);
            DestroyWindow(hwnd);OleUninitialize();return result;
        }
        wchar_t flow_output[32768]{};
        if (state.isolatedTest && GetEnvironmentVariableW(L"PULSE_TEST_SEARCH_FLOW", flow_output, ARRAYSIZE(flow_output))) {
            extern int RunSearchFlowTest(AppState&, const wchar_t*);
            const int result = RunSearchFlowTest(state, flow_output);
            DestroyWindow(hwnd);
            OleUninitialize();
            return result;
        }
#endif
        if (state.shot_tray) {
            // Stage a few real files from the shot folder so the deck is
            // visible in the verification screenshot. Drop any restored
            // session tray first so the shot is deterministic.
            state.tray.Clear();
            std::vector<std::wstring> staged;
            const std::wstring root = fs::NormalizePath(state.shot.path);
            WIN32_FIND_DATAW fd{};
            HANDLE hFind = FindFirstFileW((root + L"\\*").c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                    if (fd.cFileName[0] == L'.') continue;
                    staged.push_back(root + L"\\" + fd.cFileName);
                    if (staged.size() >= static_cast<size_t>(state.shot_tray_count)) break;
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
            if (!staged.empty()) state.tray.Collect(staged, false);
        }
        if (state.shot_tab_colors && state.pane) {
            NewTab(state, state.shot.path);
            NewTab(state, state.shot.path);
            if (state.window_tabs.items.size() >= 3) {
                // Group 1: tabs 0-1, red, named. Group 2: tab 2, blue, unnamed.
                app::TabGroup g1; g1.id = 1; g1.name = L"设计"; g1.color_rgb = 0xE74856;
                app::TabGroup g2; g2.id = 2; g2.color_rgb = 0x0078D4;
                state.window_tabs.tab_groups.push_back(g1);
                state.window_tabs.tab_groups.push_back(g2);
                state.window_tabs.next_tab_group_id = 3;
                state.window_tabs.items[0]->tab_group = 1;
                state.window_tabs.items[1]->tab_group = 1;
                state.window_tabs.items[2]->tab_group = 2;
                SwitchTab(state, 1); // active grouped tab in the middle
            }
        }
        if (g_shot_toolbar_apps) {
            state.appPrefs.persist = false;   // the user's own prefs stay as they are
            state.appPrefs.toolbar_apps = {L"C:\\Windows\\System32\\notepad.exe",
                                           L"C:\\Windows\\System32\\calc.exe",
                                           L"C:\\Windows\\System32\\mspaint.exe"};
            state.appPrefs.toolbar_app_breaks = {state.appPrefs.toolbar_apps.front()};
        }
        if (g_shot_favorites_bar) {
            state.appPrefs.favorites_bar = true;
            if (state.places.starred_items.empty()) {
                for (const wchar_t* folder : {L"C:\\Windows", L"C:\\Users", L"C:\\Program Files",
                                              L"C:\\Windows\\System32", L"Z:\\Archive"})
                    state.places.starred_items.push_back({folder, app::PlaceItemKind::Folder});
                state.places.starred_items.push_back({L"C:\\Windows\\win.ini", app::PlaceItemKind::File});
                // 收藏夹分组与分隔线: a line after Users, Program Files and
                // System32 in a group.
                state.places.starred_items[1].separator_after = true;
                state.places.starred_groups.push_back({L"shot", L"\u7cfb\u7edf"});   // 系统
                state.places.starred_items[2].group = L"shot";
                state.places.starred_items[3].group = L"shot";
                state.starredExpanded = true;
            }
        }
        if (state.shot_pinned_tab && !state.window_tabs.items.empty()) {
            state.window_tabs.items[state.window_tabs.active]->pinned = true;
            state.window_tabs.items[state.window_tabs.active]->title = L"工作";
            state.window_tabs.items[state.window_tabs.active]->marker_rgb = 0x0078D4;
        }
        if (state.shot_group > 0)
            if (app::Tab* grouped = ActiveTab(state)) {
                grouped->group_by = state.shot_group;
                RefreshActiveTab(state);  // the listing was requested before: re-sort it grouped
            }
        if (state.shot_details) {
            state.showDetailsPanel = true;
            state.renderer.SetDetailsPanelVisible(true);
        }
        int rc = ShotModeMain(state, hwnd);
        OleUninitialize();
        return rc;
    }

    MSG msg{};
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) break;
        NoteUserInput(state, msg);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Later windows' states hold references into the core `state` owns.
    ExitTimer tail;
    ReleaseWindowStatesAtExit();
    const uint64_t windows_ms = tail.Lap();
    pulse::app::hang::Stop();
    const uint64_t hang_ms = tail.Lap();
    OleUninitialize();
    const uint64_t ole_ms = tail.Lap();
    if (g_exitStarted)
        diagnostics::runtime::Event("exit_tail", {{"windows", windows_ms}, {"hang", hang_ms},
            {"ole", ole_ms}, {"since_close", GetTickCount64() - g_exitStarted}});
    return (int)msg.wParam;
}
