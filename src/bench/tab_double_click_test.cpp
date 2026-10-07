#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../app/vertical_tabs.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <iostream>

int main() {
    using namespace pulse;
    using H = ui::HitTestResult;
    using I = l10n::StringId;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        if (!ok) ++failures;
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = false;
    s.ctxMenuPrefs.persist = false;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseTabDoubleClickFixture";
    RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 1280, 900, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.renderer.SetCompositor(&s.compositor);
    s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, {});
    s.window_tabs.NewTab(app::MakeSettingsPath(L"general"));
    BindCurrentLayout(s);
    s.settings.SelectPage(0);

    app::AppPrefs loaded;
    loaded.persist = false;
    check(!s.appPrefs.close_tab_on_double_click, "double-click close defaults off");
    for (bool enabled : {false, true}) {
        s.appPrefs.close_tab_on_double_click = enabled;
        check(loaded.FromJson(s.appPrefs.ToJson()) && loaded.close_tab_on_double_click == enabled,
            "double-click preference survives JSON round-trip");
    }
    check(loaded.FromJson(L"{}") && !loaded.close_tab_on_double_click,
        "legacy preferences keep double-click close off");
    loaded.close_tab_on_double_click = true;
    loaded.ResetToDefaults();
    check(!loaded.close_tab_on_double_click, "reset restores double-click close to off");

    const auto output = std::filesystem::absolute(L"../bench_data/tab-double-click");
    std::filesystem::create_directories(output);
    ui::fluent::Painter painter(&s.compositor);
    for (const auto* language : {L"zh-CN", L"zh-TW", L"en-US"}) {
        l10n::SetLanguage(language);
        for (float scale : {1.0f, 1.5f}) for (int width : {760, 1280}) {
            s.scale = scale;
            s.compositor.RecreateTextFormats(scale);
            s.renderer.SetScale(scale);
            painter.SetScale(scale);
            const auto window = D2D1::RectF(0, 0, width * scale, 900 * scale);
            s.compositor.Resize(static_cast<UINT>(window.right), static_cast<UINT>(window.bottom));
            s.settings.SetScroll(0, 0);
            auto vm = BuildVm(s, false);
            auto layout = ui::MakeSettingsLayout(vm, window, scale, s.renderer.TitleBarHeight(), 28 * scale, &painter);
            s.settings.SetScroll(layout.close_last_tab_row.top - layout.content.top,
                s.renderer.SettingsMaxScroll(vm, window.right, window.bottom));
            vm = BuildVm(s, false);
            layout = ui::MakeSettingsLayout(vm, window, scale, s.renderer.TitleBarHeight(), 28 * scale, &painter);
            const auto row = layout.close_tab_double_click_row;
            check(row.top >= layout.close_last_tab_row.bottom && row.bottom <= layout.group[1].bottom,
                "double-click row fits the startup and close card");
            const auto x = static_cast<int>((row.left + row.right) / 2);
            const auto y = static_cast<int>((row.top + row.bottom) / 2);
            const auto hit = s.renderer.HitTest(vm, window, static_cast<float>(x), static_cast<float>(y));
            check(hit.region == H::SettingsToggle && hit.index == 35,
                "settings switch hit target matches every language, width and DPI");
            for (I id : {I::SettingsCloseTabDoubleClick, I::SettingsCloseTabDoubleClickDesc}) {
                const auto& value = l10n::Get(id);
                ui::ComPtr<IDWriteTextLayout> text_layout;
                s.compositor.DwriteFactory()->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                    id == I::SettingsCloseTabDoubleClick ? s.compositor.TextFormat() : s.compositor.SmallFormat(),
                    4000 * scale, 100 * scale, &text_layout);
                DWRITE_TEXT_METRICS metrics{};
                if (text_layout.get()) text_layout->GetMetrics(&metrics);
                check(!value.empty() && text_layout.get() && metrics.widthIncludingTrailingWhitespace <= row.right - row.left - 126 * scale,
                    "localized switch text fits without clipping");
            }
            const bool was = s.appPrefs.close_tab_on_double_click;
            HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, 0, MAKELPARAM(x, y));
            HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            vm = BuildVm(s, false);
            check(s.appPrefs.close_tab_on_double_click != was &&
                vm.settings_close_tab_on_double_click == s.appPrefs.close_tab_on_double_click &&
                loaded.FromJson(s.appPrefs.ToJson()) && loaded.close_tab_on_double_click == s.appPrefs.close_tab_on_double_click,
                "mouse click applies the switch immediately and persists it");
            if (scale == 1.0f && width == 760) for (bool dark : {false, true}) {
                s.darkMode = dark;
                s.compositor.Dc()->BeginDraw();
                s.renderer.Render(vm, window, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
                check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "settings render succeeds");
                const auto file = output / (std::wstring(language) + (dark ? L"-dark.png" : L"-light.png"));
                check(s.compositor.SaveSnapshot(file.c_str()), "settings screenshot captured");
            }
        }
    }

    s.scale = 1.0f;
    s.renderer.SetScale(s.scale);
    s.compositor.RecreateTextFormats(s.scale);
    s.compositor.Resize(1280, 900);
    const auto window = D2D1::RectF(0, 0, 1280, 900);
    auto reset_tabs = [&](bool vertical) {
        s.tabDoubleClickTarget = nullptr;
        s.window_tabs.items.clear();
        s.window_tabs.active = 0;
        s.appPrefs.vertical_tabs = vertical;
        s.appPrefs.close_window_with_last_tab = false;
        for (int i = 0; i < 3; ++i) s.window_tabs.NewTab(L"pulse:starred");
        BindCurrentLayout(s);
    };
    auto point = [&](int index) {
        auto vm = BuildVm(s, false);
        D2D1_RECT_F row{};
        const bool found = s.appPrefs.vertical_tabs
            ? s.renderer.SidebarRowRect(vm, window.right, window.bottom, kVerticalTabsSectionId, index, &row)
            : s.renderer.TabItemRect(vm, window.right, index, &row);
        const POINT pt{static_cast<LONG>((row.left + row.right) / 2), static_cast<LONG>((row.top + row.bottom) / 2)};
        const auto hit = s.renderer.HitTest(vm, window, static_cast<float>(pt.x), static_cast<float>(pt.y));
        size_t vertical_index = s.window_tabs.items.size();
        const bool matches = s.appPrefs.vertical_tabs
            ? hit.region == H::SidebarItem && IsVerticalTabPath(hit.path, &vertical_index) && vertical_index == static_cast<size_t>(index)
            : hit.region == H::Tab && hit.index == index;
        check(found && matches, "tab body has the expected horizontal or vertical hit target");
        return pt;
    };
    auto press = [&](POINT pt) {
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, 0, MAKELPARAM(pt.x, pt.y));
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    };
    auto second = [&](POINT pt) {
        HandleLButtonDblClk(&s, s.hwnd, WM_LBUTTONDBLCLK, 0, MAKELPARAM(pt.x, pt.y));
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    };
    for (bool vertical : {false, true}) {
        reset_tabs(vertical);
        s.appPrefs.close_tab_on_double_click = false;
        const auto pt = point(1);
        press(pt);
        check(s.window_tabs.active == 1 && s.window_tabs.items.size() == 3,
            "single click still activates an inactive tab");
        second(pt);
        check(s.window_tabs.items.size() == 3, "disabled double-click keeps the tab open");
        s.appPrefs.close_tab_on_double_click = true;
        const auto* remaining = s.window_tabs.items[2].get();
        press(pt);
        second(pt);
        check(s.window_tabs.items.size() == 2 && s.window_tabs.items[1].get() == remaining &&
            !s.tabDragPending && !s.tabDragging && !s.pinDragPending && !s.pinDragActive && GetCapture() != s.hwnd,
            "enabled double-click closes exactly the clicked tab and releases drag state");
        reset_tabs(vertical);
        s.window_tabs.items[0]->pinned = true;
        const auto pinned = point(0);
        press(pinned);
        second(pinned);
        check(s.window_tabs.items.size() == 3 && s.window_tabs.items[0]->pinned,
            "double-click preserves pinned tabs");
        reset_tabs(vertical);
        press(point(1));
        second(point(2));
        check(s.window_tabs.items.size() == 3, "two presses on different tabs do not close either tab");
        s.tabDoubleClickTarget = nullptr;
        second(point(1));
        check(s.window_tabs.items.size() == 3, "double-click without a tab-body first press cannot close a relocated tab");
        s.window_tabs.items.resize(1);
        s.window_tabs.active = 0;
        BindCurrentLayout(s);
        const auto last = point(0);
        press(last);
        second(last);
        MSG message{};
        check(s.window_tabs.items.size() == 1 && !PeekMessageW(&message, s.hwnd, WM_CLOSE, WM_CLOSE, PM_REMOVE),
            "last tab stays open when close-with-last-tab is disabled");
        s.appPrefs.close_window_with_last_tab = true;
        press(last);
        second(last);
        check(PeekMessageW(&message, s.hwnd, WM_CLOSE, WM_CLOSE, PM_REMOVE) != FALSE,
            "double-click on the last tab follows the window-close preference");
    }
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    owned.reset();
    CoUninitialize();
    return failures ? 1 : 0;
}
