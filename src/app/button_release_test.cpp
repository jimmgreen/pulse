#include "app_internal.h"
#include "app_input.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_renderer_internal.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>

int RunButtonReleaseTest(pulse::AppState& s, const wchar_t* output) {
    using namespace pulse;
    using H = ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};
    if (!log) return 2;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    check(s.isolatedTest && !s.isolatedTestPersist, "test runs without user preference persistence");
    if (!s.isolatedTest || s.isolatedTestPersist) return 2;
    auto find = [&](H::Region region, int index = -1) -> std::optional<POINT> {
        auto vm = BuildVm(s, false);
        const int width = s.compositor.Width(), height = s.compositor.Height();
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
        if (region == H::SettingsDisclosure && index == 1) {
            // Layout coordinates already include scroll; measure from an unscrolled VM.
            vm.settings_scroll = 0;
            ui::fluent::Painter painter(&s.compositor);
            const auto layout = ui::MakeSettingsLayout(vm, bounds, s.scale,
                s.renderer.TitleBarHeight(), 28*s.scale, &painter);
            const auto& row = layout.disclosure[1];
            if (row.bottom <= row.top) return std::nullopt;
            const float maximum = s.renderer.SettingsMaxScroll(vm,
                static_cast<float>(width), static_cast<float>(height));
            s.settings.SetScroll(std::max(0.0f, row.top - layout.content_origin - 80*s.scale), maximum);
            vm = BuildVm(s, false);
            const int x = static_cast<int>((row.left + row.right)/2);
            const int y = static_cast<int>((row.top + row.bottom)/2 - vm.settings_scroll);
            if (x < 0 || x >= width || y < 0 || y >= height) return std::nullopt;
            const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
            if (hit.region == region && hit.index == index) return POINT{x, y};
            return std::nullopt;
        }
        const bool title_button = region == H::TabNew || region == H::SettingsButton;
        const int scan_height = title_button ? std::min(height, static_cast<int>(s.renderer.TitleBarHeight()) + 8) : height;
        for (int y = 4; y < scan_height; y += 8) {
            for (int x = 4; x < width; x += 8) {
                const auto hit = s.renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
                if (hit.region != region || (index >= 0 && hit.index != index)) continue;
                if (region == H::Close || region == H::Minimize || region == H::Maximize) {
                    POINT screen{x, y};
                    ClientToScreen(s.hwnd, &screen);
                    const LRESULT expected = region == H::Close ? HTCLOSE :
                        region == H::Minimize ? HTMINBUTTON : HTMAXBUTTON;
                    if (SendMessageW(s.hwnd, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y)) != expected)
                        continue; // Exclude the resize border from native caption clicks.
                }
                return POINT{x, y};
            }
        }
        return std::nullopt;
    };
    const HWND hwnd = s.hwnd;
    auto input = [&](UINT message, POINT point) {
        SendMessageW(hwnd, message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON,
            MAKELPARAM(point.x, point.y));
    };
    const POINT outside{-20, -20};
    if (const auto point = find(H::TabNew)) {
        const auto before = s.window_tabs.items.size();
        input(WM_LBUTTONUP, *point);
        check(s.window_tabs.items.size() == before, "release without prior press does not activate");
    } else {
        check(false, "TabNew exists for release-without-press check");
    }
    auto tab_case = [&](int kind) {
        const auto point = find(H::TabNew);
        check(point.has_value(), "real TabNew hit target exists");
        if (!point) return;
        const auto before = s.window_tabs.items.size();
        input(kind == 7 ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, *point);
        check(s.window_tabs.items.size() == before,
            kind == 7 ? "double-click second press does not activate" : "TabNew press does not activate");
        POINT release = *point;
        if (kind == 1 || kind == 2) {
            input(WM_MOUSEMOVE, outside);
            if (kind == 1) release = outside;
            else input(WM_MOUSEMOVE, *point);
        } else if (kind == 3) {
            const auto other = find(H::SettingsButton);
            check(other.has_value(), "different button B exists");
            release = other.value_or(outside);
            input(WM_MOUSEMOVE, release);
        } else if (kind == 4) {
            check(GetCapture() == s.hwnd, "button press owns capture");
            ReleaseCapture();
        } else if (kind == 5) {
            SendMessageW(s.hwnd, WM_CANCELMODE, 0, 0);
        } else if (kind == 6) {
            SendMessageW(s.hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
        } else if (kind == 8) {
            auto* tab = ActiveTab(s);
            check(tab != nullptr, "active tab exists for generation cancellation");
            if (tab) ++tab->view_generation;
        } else if (kind == 9) {
            check(s.window_tabs.items.size() > 1, "another tab exists for tab-switch cancellation");
            if (s.window_tabs.items.size() > 1) {
                SwitchTab(s, (s.window_tabs.active + 1) % s.window_tabs.items.size());
            }
        }
        const bool activate = kind == 0 || kind == 2 || kind == 7;
        input(WM_LBUTTONUP, release);
        check(s.window_tabs.items.size() == before + (activate ? 1u : 0u),
            kind == 0 ? "same-button release activates exactly once" :
            kind == 1 ? "release outside cancels" :
            kind == 2 ? "leave and reenter then release activates once" :
            kind == 3 ? "A press B release activates neither button" :
            kind == 4 ? "capture loss cancels activation" :
            kind == 5 ? "WM_CANCELMODE cancels activation" :
            kind == 6 ? "WA_INACTIVE cancels activation" :
            kind == 8 ? "view generation change cancels activation" :
            kind == 9 ? "active tab change cancels activation" :
                        "double-click second release activates once");
        const auto after = s.window_tabs.items.size();
        input(WM_LBUTTONUP, release);
        check(s.window_tabs.items.size() == after, "extra release does not activate");
    };
    for (int kind = 0; kind < 7; ++kind) tab_case(kind);
    // A native double-click sequence includes an ordinary first click.
    tab_case(0);
    tab_case(7);
    tab_case(8);
    tab_case(9);

    const bool search_enabled = s.appPrefs.search_enabled;
    s.appPrefs.search_enabled = true;
    OpenSettingsTab(s, 1);
    s.settings.SetScroll(0, 0);
    const unsigned expanded = s.settingsExpanded;
    const auto disclosure = find(H::SettingsDisclosure, 1);
    check(disclosure.has_value(), "real settings disclosure hit target exists");
    if (disclosure) {
        input(WM_LBUTTONDOWN, *disclosure);
        check(s.settingsExpanded == expanded, "settings disclosure press does not toggle");
        input(WM_LBUTTONUP, *disclosure);
        check(s.settingsExpanded == (expanded ^ 2u), "settings disclosure release toggles once");
        const auto moved = find(H::SettingsDisclosure, 1);
        check(moved.has_value(), "settings disclosure remains reachable after toggle");
        if (moved) {
            input(WM_LBUTTONDOWN, *moved);
            input(WM_MOUSEMOVE, outside);
            input(WM_LBUTTONUP, outside);
            check(s.settingsExpanded == (expanded ^ 2u), "settings disclosure outside release cancels");
        }
    }
    s.settingsExpanded = expanded;
    s.appPrefs.search_enabled = search_enabled;
    SendMessageW(s.hwnd, WM_CANCELMODE, 0, 0);
    // Exercise the real nonclient entry point, not just WM_LBUTTONDOWN.
    // Close hides during the matrix; the final case verifies actual destruction.
    s.appPrefs.keep_running_on_close = true;
    for (const auto region : {H::Minimize, H::Maximize, H::Close}) {
        const WPARAM native_hit = region == H::Minimize ? HTMINBUTTON :
            region == H::Maximize ? HTMAXBUTTON : HTCLOSE;
        for (int kind = 0; kind < 8; ++kind) {
            ShowWindow(hwnd, SW_RESTORE);
            const auto point = find(region);
            check(point.has_value(), "caption target exists");
            if (!point) continue;
            POINT screen = *point;
            ClientToScreen(hwnd, &screen);
            check(SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y)) ==
                static_cast<LRESULT>(native_hit), "native caption hit and Snap target preserved");
            SendMessageW(hwnd, kind == 7 ? WM_NCLBUTTONDBLCLK : WM_NCLBUTTONDOWN,
                native_hit, MAKELPARAM(screen.x, screen.y));
            check(IsWindowVisible(hwnd) && !IsIconic(hwnd) && !IsZoomed(hwnd),
                "caption press does not execute action");
            check(s.buttonPress.active && GetCapture() == hwnd, "caption press captures pointer");
            check(BuildVm(s, false).button_pressed, "caption press exposes pressed visual state");
            POINT release = *point;
            if (kind == 1 || kind == 2) {
                input(WM_MOUSEMOVE, outside);
                check(!BuildVm(s, false).button_pressed, "caption drag outside clears pressed visual state");
                if (kind == 1) release = outside;
                else input(WM_MOUSEMOVE, *point);
            } else if (kind == 3) ReleaseCapture();
            else if (kind == 4) SendMessageW(hwnd, WM_CANCELMODE, 0, 0);
            else if (kind == 5) SendMessageW(hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
            else if (kind == 6) {
                release = find(region == H::Close ? H::Minimize : H::Close).value_or(outside);
                input(WM_MOUSEMOVE, release);
            }
            input(WM_LBUTTONUP, release);
            const bool activate = kind == 0 || kind == 2 || kind == 7;
            const bool changed = region == H::Minimize ? IsIconic(hwnd) != FALSE :
                region == H::Maximize ? IsZoomed(hwnd) != FALSE : IsWindowVisible(hwnd) == FALSE;
            check(changed == activate, "caption release obeys inside/cancel/capture/activation state");
            check(!s.buttonPress.active, "caption release clears press before action");
            // Restore geometry before a spurious release, so hit testing sees the same button.
            ShowWindow(hwnd, SW_RESTORE);
            input(WM_LBUTTONUP, *point);
            check(IsWindowVisible(hwnd) && !IsIconic(hwnd) && !IsZoomed(hwnd),
                "caption extra release does not execute again");
        }
    }
    s.appPrefs.keep_running_on_close = false;
    if (const auto point = find(H::Close)) {
        POINT screen = *point;
        ClientToScreen(hwnd, &screen);
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCLOSE, MAKELPARAM(screen.x, screen.y));
        check(IsWindow(hwnd), "close press preserves isolated window");
        input(WM_LBUTTONUP, *point);
        // AppState can be gone here: only the saved HWND and local log are used.
        check(!IsWindow(hwnd), "close release destroys isolated window");
    } else check(false, "close target exists for destruction test");
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
