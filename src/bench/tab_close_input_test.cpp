#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../app/single_instance_coordinator.h"
#include "../app/tab_shortcuts.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_renderer_internal.h"
#include <cstdio>
#include <filesystem>
#include <optional>
#include <type_traits>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int);
extern int RunButtonReleaseTest(pulse::AppState&, const wchar_t*);
namespace {
using namespace pulse;
using H = ui::HitTestResult;
int failed = 0;
ULONGLONG started = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); std::fflush(stdout); failed += !ok;
}
std::optional<POINT> Find(AppState& s, H::Region region, int index = -1) {
    const auto vm = BuildVm(s, false);
    const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    for (int y = static_cast<int>(s.renderer.TitleBarHeight() / 2); y < s.renderer.TitleBarHeight(); y += 4)
        for (int x = 4; x < s.compositor.Width(); x += 4) {
            const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
            if (hit.region == region && (index < 0 || hit.index == index)) return POINT{x, y};
        }
    return std::nullopt;
}
void Input(HWND hwnd, UINT message, POINT point) {
    const WPARAM keys = message == WM_LBUTTONDOWN || (message == WM_MOUSEMOVE && GetCapture() == hwnd) ? MK_LBUTTON : 0;
    SendMessageW(hwnd, message, keys, MAKELPARAM(point.x, point.y));
}
void Exercise(AppState& s) {
    Check(s.isolatedTest && !s.isolatedTestPersist, "isolated production window does not persist user preferences");
    s.appPrefs.tab_layout = 0; s.appPrefs.tab_max_rows = 1;
    s.renderer.SetVerticalTabs(false);
    const auto path = ActiveTab(s)->current_path;
    while (s.window_tabs.items.size() < 20) s.window_tabs.NewTab(path);
    s.window_tabs.SwitchTab(0); BindCurrentLayout(s);
    for (int kind = 0; kind < 6; ++kind) {
        s.hoverRegion = 0; s.hoverControlIndex = -1;
        auto point = Find(s, H::Tab, 1);
        Check(point.has_value(), "inactive crowded tab is reachable"); if (!point) continue;
        Check(!Find(s, H::TabClose, 1).has_value(), "inactive close is hidden until hover (narrow-tab regression fixture)");
        Input(s.hwnd, WM_MOUSEMOVE, *point);
        auto close = Find(s, H::TabClose, 1);
        Check(close.has_value(), "hover reveals real inactive-tab close hit target"); if (!close) continue;
        Input(s.hwnd, WM_MOUSEMOVE, *close);
        const auto count = s.window_tabs.items.size();
        const auto* target = s.window_tabs.items[1].get();
        Input(s.hwnd, WM_LBUTTONDOWN, *close);
        Check(s.window_tabs.items.size() == count && s.buttonPress.active, "close press arms without closing");
        if (kind == 1) Input(s.hwnd, WM_MOUSEMOVE, *close);
        if (kind == 2 || kind == 3) Input(s.hwnd, WM_MOUSEMOVE, {-20, -20});
        if (kind == 3) Input(s.hwnd, WM_MOUSEMOVE, *close);
        if (kind == 4) ReleaseCapture();
        if (kind == 5) SendMessageW(s.hwnd, WM_CANCELMODE, 0, 0);
        Input(s.hwnd, WM_LBUTTONUP, kind == 2 ? POINT{-20, -20} : *close);
        const bool should_close = kind == 0 || kind == 1 || kind == 3;
        bool exists = false;
        for (const auto& tab : s.window_tabs.items) exists |= tab.get() == target;
        Check(s.window_tabs.items.size() == count - (should_close ? 1 : 0) && exists != should_close,
            kind == 0 ? "stationary release closes hovered inactive tab" :
            kind == 1 ? "release after same-position move closes inactive tab" :
            kind == 2 ? "release outside does not close" :
            kind == 3 ? "leave and reenter then release closes inactive tab" :
            kind == 4 ? "capture loss cancels tab close" : "cancel mode cancels tab close");
        const auto after = s.window_tabs.items.size();
        Input(s.hwnd, WM_LBUTTONUP, *close);
        Check(s.window_tabs.items.size() == after, "extra release never closes another tab");
        if (kind == 0 && after == count - 1) {
            const auto* next = s.window_tabs.items[1].get();
            Input(s.hwnd, WM_LBUTTONDOWN, *close); Input(s.hwnd, WM_LBUTTONUP, *close);
            bool remains = false;
            for (const auto& tab : s.window_tabs.items) remains |= tab.get() == next;
            Check(s.window_tabs.items.size() == after - 1 && !remains,
                  "second complete click closes next tab without moving pointer");
            s.window_tabs.SwitchTab(0); BindCurrentLayout(s);
        }
    }
    s.window_tabs.items[1]->pinned = true;
    s.hoverRegion = H::Tab; s.hoverControlIndex = 1;
    Check(!Find(s, H::TabClose, 1), "pinned tab has no close hit target");
    s.window_tabs.items[1]->pinned = false;
    if (auto close = Find(s, H::TabClose, static_cast<int>(s.window_tabs.active))) {
        const auto count = s.window_tabs.items.size();
        Input(s.hwnd, WM_MOUSEMOVE, *close); Input(s.hwnd, WM_LBUTTONDOWN, *close); Input(s.hwnd, WM_LBUTTONUP, *close);
        Check(s.window_tabs.items.size() == count - 1, "active tab close remains functional");
    } else Check(false, "active close target exists");
    // The fix shares release/capture handling with other buttons. Exercise the
    // existing targeted gesture suite before it closes this isolated window.
    wchar_t report[32768]{};
    if (GetEnvironmentVariableW(L"PULSE_TAB_CLOSE_BUTTON_REPORT", report, 32768)) {
        const int result = RunButtonReleaseTest(s, report);
        Check(result == 0, "related button-release and caption cancellation suite");
    }
}

void ExerciseTabStyleAndWheel(AppState& s) {
    const auto base_path = ActiveTab(s) && !ActiveTab(s)->current_path.empty()
        ? ActiveTab(s)->current_path : std::wstring(L"C:\\");
    s.appPrefs.vertical_tabs = false;
    s.appPrefs.tab_layout = 0;
    s.appPrefs.tab_max_rows = 3;
    s.appPrefs.tab_active_style = 0;
    s.renderer.SetVerticalTabs(false);

    // 1) Few tabs (non-overflowing): vertical mouse wheel over the tab strip
    // switches active tabs sequentially and never bleeds through to content.
    while (s.window_tabs.items.size() > 1) CloseLayoutTab(s, s.window_tabs.items.size() - 1);
    while (s.window_tabs.items.size() < 3) s.window_tabs.NewTab(base_path);
    for (auto& lt : s.window_tabs.items) { lt->pinned = false; lt->tab_group = 0; }
    s.window_tabs.tab_groups.clear();
    SwitchTab(s, 0);
    s.renderer.UpdateTabLayout(BuildVm(s, false), static_cast<float>(s.compositor.Width()));

    const auto strip_pt_opt = Find(s, H::Tab, 0);
    Check(strip_pt_opt.has_value(), "wheel: non-overflowing tab 0 is reachable");
    if (strip_pt_opt) {
        POINT scr = *strip_pt_opt;
        ClientToScreen(s.hwnd, &scr);
        const LPARAM wheel_pos = MAKELPARAM(scr.x, scr.y);
        auto send_wheel = [&](int delta) {
            HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(delta)), wheel_pos);
        };
        s.tabWheelRemainder = 0;
        s.tabWheelTick = 0;

        send_wheel(-WHEEL_DELTA);
        Check(s.window_tabs.active == 1, "wheel down selects next tab when strip has only 3 tabs");
        send_wheel(-WHEEL_DELTA);
        Check(s.window_tabs.active == 2, "wheel down continues sequentially to last tab");
        send_wheel(-WHEEL_DELTA);
        Check(s.window_tabs.active == 2, "wheel down clamps at last tab without wrapping");

        send_wheel(WHEEL_DELTA / 2);
        const bool half_waited = (s.window_tabs.active == 2);
        send_wheel(WHEEL_DELTA / 2);
        Check(half_waited && s.window_tabs.active == 1,
              "wheel touchpad half notches accumulate into one tab step up");
        send_wheel(WHEEL_DELTA);
        Check(s.window_tabs.active == 0, "wheel up returns to first tab");
        send_wheel(WHEEL_DELTA);
        Check(s.window_tabs.active == 0, "wheel up clamps at first tab without wrapping");

        // Open Settings > Tabs & Window scrolled slightly down and confirm wheeling
        // on the tab strip does not scroll the underlying settings page.
        OpenSettingsTab(s, 10);
        const float max_scroll = s.renderer.SettingsMaxScroll(
            BuildVm(s, false), static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        s.settings.SetScroll(std::min(120.0f, max_scroll), max_scroll);
        const float scroll_before = s.settings.scroll();
        const size_t tabs_now = s.window_tabs.items.size();
        SwitchTab(s, tabs_now - 1);
        s.renderer.UpdateTabLayout(BuildVm(s, false), static_cast<float>(s.compositor.Width()));
        send_wheel(WHEEL_DELTA);
        Check(s.window_tabs.active == tabs_now - 2 && std::fabs(s.settings.scroll() - scroll_before) < 0.01f,
              "wheel over tab strip switches away from settings without scrolling settings content");
        CloseLayoutTab(s, tabs_now - 1);
    }

    // 2) Collapsed tab group: wheel skips hidden collapsed members.
    while (s.window_tabs.items.size() < 4) s.window_tabs.NewTab(base_path);
    app::TabGroup grp{};
    grp.id = 42;
    grp.name = L"测试组";
    grp.color_rgb = 0xD08A2Eu;
    grp.collapsed = true;
    s.window_tabs.tab_groups.push_back(grp);
    s.window_tabs.items[1]->tab_group = 42;
    s.window_tabs.items[2]->tab_group = 42;
    SwitchTab(s, 0);
    s.renderer.UpdateTabLayout(BuildVm(s, false), static_cast<float>(s.compositor.Width()));
    if (auto p0 = Find(s, H::Tab, 0)) {
        POINT scr = *p0;
        ClientToScreen(s.hwnd, &scr);
        const LPARAM lp = MAKELPARAM(scr.x, scr.y);
        s.tabWheelRemainder = 0;
        s.tabWheelTick = 0;
        HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), lp);
        Check(s.window_tabs.active == 3, "wheel down skips tabs inside a collapsed group");
        HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(WHEEL_DELTA)), lp);
        Check(s.window_tabs.active == 0, "wheel up skips back over collapsed group");
    }
    for (auto& lt : s.window_tabs.items) lt->tab_group = 0;
    s.window_tabs.tab_groups.clear();

    // 3) Overflowing strip + pinned tab auto-scroll into view on wheel selection.
    while (s.window_tabs.items.size() < 22) s.window_tabs.NewTab(base_path);
    s.window_tabs.items[0]->pinned = true;
    SwitchTab(s, 0);
    s.renderer.UpdateTabLayout(BuildVm(s, false), static_cast<float>(s.compositor.Width()));
    if (auto p0 = Find(s, H::Tab, 0)) {
        POINT scr = *p0;
        ClientToScreen(s.hwnd, &scr);
        const LPARAM lp = MAKELPARAM(scr.x, scr.y);
        s.tabWheelRemainder = 0;
        s.tabWheelTick = 0;
        for (int k = 0; k < 18; ++k) {
            HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), lp);
        }
        const auto vm_far = BuildVm(s, false);
        D2D1_RECT_F active_rc{};
        const bool far_ok = s.renderer.TabItemRect(vm_far, static_cast<float>(s.compositor.Width()), 18, &active_rc) &&
                            active_rc.right > active_rc.left;
        Check(s.window_tabs.active == 18 && far_ok,
              "wheel sequentially scrolls active tab across 22 overflowing tabs and keeps it visible");

        for (int k = 0; k < 18; ++k) {
            HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(WHEEL_DELTA)), lp);
        }
        const auto vm_home = BuildVm(s, false);
        D2D1_RECT_F pinned_rc{};
        const bool home_ok = s.renderer.TabItemRect(vm_home, static_cast<float>(s.compositor.Width()), 0, &pinned_rc) &&
                             pinned_rc.right > pinned_rc.left;
        Check(s.window_tabs.active == 0 && home_ok,
              "wheel up scrolls back to pinned first tab in overflowing strip");
    }
    s.window_tabs.items[0]->pinned = false;

    // 4) Settings > Tabs & Window: C/B preview layout, hit testing, click selection, prefs roundtrip.
    while (s.window_tabs.items.size() > 4) CloseLayoutTab(s, s.window_tabs.items.size() - 1);
    OpenSettingsTab(s, 10);
    s.settings.SetScroll(0.0f, 0.0f);
    {
        const auto vm = BuildVm(s, false);
        const D2D1_RECT_F wide = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                              static_cast<float>(s.compositor.Height()));
        ui::fluent::Painter painter(&s.compositor);
        painter.SetScale(s.scale);
        const auto lay_wide = ui::MakeSettingsLayout(vm, wide, s.scale, s.renderer.TitleBarHeight(), 28.0f * s.scale, &painter);
        const auto& c0 = lay_wide.tab_active_style_choices[0];
        const auto& c1 = lay_wide.tab_active_style_choices[1];
        const bool non_overlap = (c0.right <= c1.left || c0.bottom <= c1.top);
        const bool inside_card = c0.left >= lay_wide.tab_active_style_card.left &&
                                 c1.right <= lay_wide.tab_active_style_card.right &&
                                 c1.bottom <= lay_wide.tab_active_style_card.bottom &&
                                 lay_wide.tab_active_style_card.bottom <= lay_wide.tab_layout_card.top;
        Check(c0.right > c0.left && c0.bottom > c0.top && c1.right > c1.left && c1.bottom > c1.top &&
              non_overlap && inside_card,
              "settings: active tab style preview cards sit cleanly inside SettingsSecTabs without overlap");

        const auto narrow_640 = D2D1::RectF(0, 0, 640.0f * s.scale, static_cast<float>(s.compositor.Height()));
        const auto lay_640 = ui::MakeSettingsLayout(vm, narrow_640, s.scale, s.renderer.TitleBarHeight(), 28.0f * s.scale, &painter);
        const bool fit_640 = lay_640.tab_active_style_choices[0].left >= lay_640.tab_active_style_card.left &&
                             lay_640.tab_active_style_choices[0].right <= lay_640.tab_active_style_choices[1].left &&
                             lay_640.tab_active_style_choices[1].right <= lay_640.tab_active_style_card.right &&
                             lay_640.tab_active_style_choices[1].bottom <= lay_640.tab_active_style_card.bottom;
        Check(fit_640, "settings: compact 640px layout fits C/B preview tiles without clipping or overlap");

        const auto narrow_480 = D2D1::RectF(0, 0, 480.0f * s.scale, static_cast<float>(s.compositor.Height()));
        const auto lay_480 = ui::MakeSettingsLayout(vm, narrow_480, s.scale, s.renderer.TitleBarHeight(), 28.0f * s.scale, &painter);
        const bool stack_480 = lay_480.tab_active_style_choices[0].right <= lay_480.tab_active_style_card.right &&
                               lay_480.tab_active_style_choices[1].bottom <= lay_480.tab_active_style_card.bottom &&
                               lay_480.tab_active_style_choices[0].bottom <= lay_480.tab_active_style_choices[1].top;
        Check(stack_480, "settings: narrow 480px layout stacks C/B preview tiles cleanly");

        const float hit_x = (c1.left + c1.right) * 0.5f;
        const float hit_y = (c1.top + c1.bottom) * 0.5f;
        const auto hit_b = s.renderer.HitTest(vm, wide, hit_x, hit_y);
        Check(hit_b.region == H::SettingsTabActiveStyle && hit_b.index == 1,
              "settings: hit test on card B returns SettingsTabActiveStyle index 1");
        HandleSettingsControl(s, hit_b);
        Check(s.appPrefs.tab_active_style == 1 && BuildVm(s, false).tab_active_style == 1,
              "settings: clicking preview B updates appPrefs and VM tab_active_style to 1");

        std::remove_cvref_t<decltype(s.appPrefs)> roundtrip{};
        const std::wstring serialized = s.appPrefs.ToJson();
        Check(roundtrip.FromJson(serialized) && roundtrip.tab_active_style == 1,
              "prefs: AppPreferences JSON round-trip preserves tab_active_style=1");

        const auto hit_c = s.renderer.HitTest(BuildVm(s, false), wide, (c0.left + c0.right) * 0.5f, (c0.top + c0.bottom) * 0.5f);
        HandleSettingsControl(s, hit_c);
        Check(s.appPrefs.tab_active_style == 0,
              "settings: clicking preview C switches tab_active_style back to 0");
    }

    // 5) Optional D2D visual verification screenshots when PULSE_TAB_STYLE_SHOT_DIR is set.
    wchar_t shot_dir_buf[MAX_PATH * 2]{};
    if (GetEnvironmentVariableW(L"PULSE_TAB_STYLE_SHOT_DIR", shot_dir_buf, ARRAYSIZE(shot_dir_buf)) > 0) {
        const std::filesystem::path shot_dir(shot_dir_buf);
        std::filesystem::create_directories(shot_dir);
        const float w = static_cast<float>(s.compositor.Width());
        const float h = static_cast<float>(s.compositor.Height());
        const D2D1_RECT_F win_rc = D2D1::RectF(0, 0, w, h);

        auto draw_frame = [&](ui::WindowViewModel& vm) {
            s.renderer.UpdateTabLayout(vm, w);
            s.compositor.ForceFullFrame();
            auto* dc = s.compositor.Dc();
            dc->BeginDraw();
            s.renderer.Render(vm, win_rc, ui::MakeTheme(vm.dark, s.accentColor));
            const HRESULT hr = dc->EndDraw();
            if (SUCCEEDED(hr)) s.compositor.Present();
            return SUCCEEDED(hr);
        };
        auto render_shot = [&](const wchar_t* name, ui::WindowViewModel& vm) {
            const bool ok = draw_frame(vm);
            const bool saved = ok && s.compositor.SaveSnapshot((shot_dir / name).c_str());
            Check(saved, "visual snapshot saved");
        };

        // Tab bar comparisons (normal + grouped tab) in C and B
        CloseLayoutTab(s, s.window_tabs.items.size() - 1); // back to 4 folder tabs
        SwitchTab(s, 1);
        auto vm_tabs = BuildVm(s, false);
        vm_tabs.settings_open = false;
        if (vm_tabs.tabs.size() >= 3 && vm_tabs.tab_groups.empty()) {
            ui::TabGroupView gv{};
            gv.name = L"工作区";
            gv.color_rgb = 0xD08A2Eu;
            vm_tabs.tab_groups.push_back(gv);
            vm_tabs.tabs[2].group = 0;
            vm_tabs.tabs[2].color_rgb = 0xD08A2Eu;
            vm_tabs.tabs[3].group = 0;
            vm_tabs.tabs[3].color_rgb = 0xD08A2Eu;
        }
        vm_tabs.tab_active_style = 0; vm_tabs.dark = false; vm_tabs.window_active = true;
        draw_frame(vm_tabs); Sleep(220);
        render_shot(L"tab_style_c_light.png", vm_tabs);
        vm_tabs.tab_active_style = 1; vm_tabs.dark = false; vm_tabs.window_active = true;
        render_shot(L"tab_style_b_light.png", vm_tabs);
        vm_tabs.tab_active_style = 1; vm_tabs.dark = false; vm_tabs.window_active = false;
        render_shot(L"tab_style_b_unfocused_light.png", vm_tabs);
        vm_tabs.tab_active_style = 1; vm_tabs.dark = true; vm_tabs.window_active = true;
        render_shot(L"tab_style_b_dark.png", vm_tabs);
        if (vm_tabs.tabs.size() >= 3) {
            for (auto& t : vm_tabs.tabs) t.active = false;
            vm_tabs.tabs[2].active = true;
            vm_tabs.active_tab = 2;
            vm_tabs.tab_active_style = 1; vm_tabs.dark = false; vm_tabs.window_active = true;
            draw_frame(vm_tabs); Sleep(220);
            render_shot(L"tab_style_b_grouped_light.png", vm_tabs);
        }

        // Settings > Tabs & Window preview cards
        OpenSettingsTab(s, 10);
        s.settings.SetScroll(0.0f, 0.0f);
        auto vm_set = BuildVm(s, false);
        vm_set.tab_active_style = 0; vm_set.dark = false; vm_set.window_active = true;
        render_shot(L"settings_tab_style_light.png", vm_set);
        vm_set.tab_active_style = 1; vm_set.dark = true; vm_set.window_active = true;
        render_shot(L"settings_tab_style_dark.png", vm_set);
    }
}
void CALLBACK Probe(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND window = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND candidate, LPARAM context) -> BOOL {
        wchar_t name[100]{}; GetClassNameW(candidate, name, 100);
        if (wcscmp(name, app::SingleInstanceCoordinator::WindowClassName()) == 0) {
            *reinterpret_cast<HWND*>(context) = candidate; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&window));
    AppState* s = window ? GetAppState(window) : nullptr;
    if (s && s->startupComplete && s->first_present_reported) {
        KillTimer(nullptr, timer); Exercise(*s); ExerciseTabStyleAndWheel(*s);
        std::printf("TAB_CLOSE_RESULT failed=%d\n", failed); std::fflush(stdout);
        if (IsWindow(window)) DestroyWindow(window);
        PostQuitMessage(failed ? 1 : 0);
    } else if (GetTickCount64() - started > 30000) {
        KillTimer(nullptr, timer); Check(false, "production test window initialized within timeout"); PostQuitMessage(2);
    }
}
}
int wmain() {
    started = GetTickCount64(); SetTimer(nullptr, 0, 50, Probe);
    return wWinMain(GetModuleHandleW(nullptr), nullptr, GetCommandLineW(), SW_SHOWNORMAL);
}
