#include "app_internal.h"
#include "extension_store_ui.h"
#include "extension_store_edits.h"
#include "../common/json_value.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_view_model.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>

namespace {
void PumpSwitches() {
    MSG msg{};
    for (int count = 0; count < 64 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++count) {
        if (msg.message == WM_QUIT) { PostQuitMessage(static_cast<int>(msg.wParam)); break; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
}
std::wstring SwitchPreview(const pulse::ui::SevenZipView& z) {
    std::wstring result;
    for (const auto* rows : {&z.archive_rows, &z.file_rows}) {
        for (const auto& row : *rows) result += row.title + L"|" + row.detail + L"\n";
        result += L"---\n";
    }
    return result;
}
}
int RunExtensionStoreSwitchesTest(pulse::AppState& s, const wchar_t* output_dir) {
    using namespace pulse;
    using A = ui::PackAction;
    if (!s.isolatedTest || !output_dir) return 2;
    std::ofstream log(std::filesystem::path(output_dir) / L"switches-mouse.log");
    int failures = 0;
    auto check = [&](bool value, const char* text) {
        log << (value ? "[PASS] " : "[FAIL] ") << text << std::endl;
        if (!value) ++failures;
        return value;
    };
    auto read = [&] { ui::WindowViewModel vm; FillExtensionStoreView(s, vm); return vm.settings_store_view.seven_zip; };
    auto wait = [&](const std::function<bool()>& predicate) {
        const auto until = GetTickCount64() + 3000;
        do {
            PumpSwitches(); TickExtensionStoreUi(s);
            if (predicate()) return true;
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } while (GetTickCount64() < until);
        return predicate();
    };
    const auto initial = read();
    if (!check(initial.available && initial.menu_available, "real installed plugin exposes menu edit capabilities")) return 1;
    const bool old_detail = s.store_detail;
    const int old_tab = s.store_detail_tab;
    const float old_scroll = s.settings.scroll();
    OpenSettingsTab(s, 8);
    s.store_detail = true; s.store_detail_tab = 1;
    DestroyStoreEdits(s);
    const auto bounds = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    auto draw = [&] {
        auto vm = BuildVm(s, false);
        auto* dc = s.compositor.Dc(); dc->BeginDraw();
        const auto theme = ui::MakeTheme(s.darkMode, s.accentColor);
        dc->Clear(theme.bg); s.renderer.Render(vm, bounds, theme);
        const bool ok = SUCCEEDED(dc->EndDraw());
        return std::pair{vm, ok};
    };
    auto click = [&](int action) {
        auto [first, first_ok] = draw();
        if (!first_ok) return false;
        const float maximum = s.renderer.SettingsMaxScroll(first, bounds.right, bounds.bottom);
        s.settings.SetScroll(0, maximum);
        float scroll = 0;
        while (true) {
            auto [vm, ok] = draw();
            if (!ok) return false;
            const auto clip = s.renderer.SettingsStoreClip();
            const float height = clip.bottom - clip.top;
            if (height <= 0) return false;
            // Search the real hit map: fixed interior columns, no inferred widget/DPI geometry.
            for (float fraction : {.25f, .5f, .7f}) {
                const float x = clip.left + (clip.right - clip.left) * fraction;
                for (float y = clip.top + 4; y < clip.bottom - 4; y += 8) {
                    const auto hit = s.renderer.HitTest(vm, bounds, x, y);
                    if (hit.region != ui::HitTestResult::SettingsPackAction || hit.index != action) continue;
                    const int px = static_cast<int>(std::lround(x)), py = static_cast<int>(std::lround(y));
                    const auto rounded = s.renderer.HitTest(vm, bounds, static_cast<float>(px), static_cast<float>(py));
                    if (rounded.region != hit.region || rounded.index != action) continue;
                    log << "click action=" << action << " x=" << px << " y=" << py << " scroll=" << s.settings.scroll() << std::endl;
                    SendMessageW(s.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(px, py));
                    SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px, py));
                    SendMessageW(s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(px, py));
                    PumpSwitches();
                    return true;
                }
            }
            if (scroll >= maximum) break;
            scroll = (std::min)(maximum, scroll + height * .6f);
            s.settings.SetScroll(scroll, maximum);
        }
        return false;
    };
    // Choose the other placement first, then return through the opposite card.
    for (int target : {initial.submenu ? 0 : 1, initial.submenu ? 1 : 0}) {
        check(click(static_cast<int>(A::SzPlacementBase) + target) &&
              wait([&] { return read().submenu == (target == 1); }),
              "placement card mouse down/up commits the actual plugin state");
    }
    for (int bit = 0; bit < 6; ++bit) {
        const auto before = read();
        const auto signature = SwitchPreview(before);
        const uint32_t expected = before.commands ^ (1u << bit);
        check(click(static_cast<int>(A::SzCommandBase) + bit) && wait([&] { return read().commands == expected; }),
              "command row mouse down/up changes the actual plugin mask");
        check(wait([&] { return SwitchPreview(read()) != signature; }),
              "command mouse toggle changes the live selection-specific menu preview");
        check(click(static_cast<int>(A::SzCommandBase) + bit) && wait([&] {
            const auto current = read();
            return current.commands == before.commands && SwitchPreview(current) == signature;
        }), "second real click restores both command state and live preview");
    }
    check(read().commands == initial.commands && read().submenu == initial.submenu,
          "mouse test leaves original menu controls restored");
    s.store_detail = old_detail; s.store_detail_tab = old_tab;
    const auto vm = BuildVm(s, false);
    s.settings.SetScroll(old_scroll, s.renderer.SettingsMaxScroll(vm, bounds.right, bounds.bottom));
    InvalidateRect(s.hwnd, nullptr, FALSE);
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
