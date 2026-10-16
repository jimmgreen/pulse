#include "app_internal.h"
#include "app_input.h"
#include <cmath>
#include <filesystem>
#include <fstream>

#ifdef PULSE_WITH_SELFTEST
using namespace pulse;
void Render(pulse::AppState&);

int RunSettingsScrollbarTest(AppState& s, const wchar_t* output) {
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    s.appPrefs.persist = false;
    OpenSettingsTab(s, 0);
    const float width = static_cast<float>(s.compositor.Width());
    const float height = static_cast<float>(s.compositor.Height());
    const auto bounds = D2D1::RectF(0, 0, width, height);
    D2D1_RECT_F track{}, thumb{};
    float maximum = 0;
    auto geometry = [&] {
        return s.renderer.SettingsScrollbarGeometry(BuildVm(s, false), width, height,
                                                     track, thumb, maximum);
    };
    auto down = [&](int x, int y) {
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    };
    auto move = [&](int x, int y, WPARAM buttons = MK_LBUTTON) {
        HandleMouseMove(&s, s.hwnd, WM_MOUSEMOVE, buttons, MAKELPARAM(x, y));
    };
    auto up = [&](int x, int y) {
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    };
    s.settings.SetScroll(0, 0);
    if (!geometry()) {
        check(false, "general settings has an overflowing scrollbar");
        return failures;
    }
    const float file_scroll = ActiveTab(s)->scroll_y;
    const int x = static_cast<int>((thumb.left + thumb.right) * 0.5f);
    const int y = static_cast<int>((thumb.top + thumb.bottom) * 0.5f);
    auto hit = s.renderer.HitTest(BuildVm(s, false), bounds, static_cast<float>(x), static_cast<float>(y));
    check(hit.region == ui::HitTestResult::Scrollbar && hit.sub_index == 3,
          "visible settings thumb is interactive");
    down(x, y);
    check(s.scrollbarDragging && s.scrollbarSettings && GetCapture() == s.hwnd,
          "thumb press captures settings drag");
    check(s.settings.scroll() == 0, "thumb press does not jump");
    move(x - 100, y + 80);
    check(s.settings.scroll() > 0, "drag scrolls settings even outside the narrow track");
    move(x, static_cast<int>(height + 200));
    check(std::abs(s.settings.scroll() - maximum) < 0.1f, "drag clamps at bottom");
    move(x, 0);
    check(s.settings.scroll() == 0, "drag clamps at top");
    up(x, 0);
    check(!s.scrollbarDragging && !s.scrollbarSettings && GetCapture() != s.hwnd,
          "release ends drag and releases capture");
    down(x, static_cast<int>(track.bottom - 5));
    check(s.settings.scroll() > maximum * 0.5f, "track click jumps toward the clicked position");
    up(x, static_cast<int>(track.bottom - 5));
    geometry();
    down(x, static_cast<int>((thumb.top + thumb.bottom) * 0.5f));
    ReleaseCapture();
    const float after_capture_loss = s.settings.scroll();
    move(x, 0);
    check(!s.scrollbarDragging && !s.scrollbarSettings && s.settings.scroll() == after_capture_loss,
          "capture loss cancels drag without further scrolling");
    check(ActiveTab(s)->scroll_y == file_scroll, "settings scrollbar leaves file pane offset unchanged");
    s.settings.SetScroll(0, 0);
    geometry();
    down(x, y);
    move(x, y + 50, 0);
    check(!s.scrollbarDragging && GetCapture() != s.hwnd, "missing button release cancels drag");
    POINT wheel_point{400, 300};
    ClientToScreen(s.hwnd, &wheel_point);
    HandleMouseWheel(&s, s.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
                     MAKELPARAM(wheel_point.x, wheel_point.y));
    check(s.settings.scroll() > 0, "mouse wheel still scrolls settings after dragging");

    for (float scale : {1.0f, 1.5f, 2.0f}) {
        s.renderer.SetScale(scale);
        s.compositor.RecreateTextFormats(scale);
        for (int page : {0, 1, 2, 3, 4, 5, 6, 7}) {
            auto vm = BuildVm(s, false);
            vm.settings_page = page;
            vm.settings_scroll = 0;
            const auto window = D2D1::RectF(0, 0, 900 * scale, 600 * scale);
            if (s.renderer.SettingsScrollbarGeometry(vm, window.right, window.bottom, track, thumb, maximum)) {
                const auto target = s.renderer.HitTest(vm, window,
                    (thumb.left + thumb.right) * 0.5f, (thumb.top + thumb.bottom) * 0.5f);
                check(target.region == ui::HitTestResult::Scrollbar && target.sub_index == 3,
                      "overflowing settings pages hit the drawn thumb at each DPI");
            }
        }
        auto vm = BuildVm(s, false);
        vm.settings_open = false;
        check(!s.renderer.SettingsScrollbarGeometry(vm, width, height, track, thumb, maximum),
              "closed settings never exposes a scrollbar");
        vm.settings_open = true;
        check(!s.renderer.SettingsScrollbarGeometry(vm, 1400 * scale, 20000 * scale, track, thumb, maximum),
              "content that fits has no interactive scrollbar");
    }
    s.renderer.SetScale(s.scale);
    s.compositor.RecreateTextFormats(s.scale);
    auto snapshot = [&](const wchar_t* name) {
        // Snapshot reads the presented buffer; allow the swap chain to present
        // the new scroll position rather than capturing its previous frame.
        for (int frame = 0; frame < 3; ++frame) {
            Render(s);
            Sleep(30);
        }
        return s.compositor.SaveSnapshot((std::filesystem::path(output).parent_path() / name).c_str());
    };
    s.settings.SetScroll(0, 0);
    check(snapshot(L"settings-scrollbar-top.png"),
          "capture settings top for visual inspection");
    geometry();
    s.settings.SetScroll(maximum, maximum);
    check(snapshot(L"settings-scrollbar-bottom.png"),
          "capture settings bottom for visual inspection");
    return failures;
}
#endif
