// Exercise the real mouse route: search and network switches share numeric IDs.
#include "../common/windows_compat.h"
#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../ui/ui_renderer_internal.h"
#include <cstdio>

int main() {
    using namespace pulse;
    compat::EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.appPrefs.persist = s.ctxMenuPrefs.persist = false;
    s.appPrefs.global_search_enabled = true;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseSearchToggleInputTest";
    RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1400, 1400, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.scale = 1;
    s.compositor.RecreateTextFormats(1);
    s.renderer.SetCompositor(&s.compositor);
    s.renderer.SetScale(1);
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    ActiveTab(s)->current_path = app::MakeSettingsPath(app::SettingsController::PageName(1));
    s.settings.SelectPage(1);
    s.settings.BindUi(s.appPrefs, s.ctxMenuPrefs, s.index, s.networkIndex, {});
    ui::fluent::Painter painter(&s.compositor);
    const auto rect = D2D1::RectF(0, 0, 1400, 1400);
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    auto click = [&](const D2D1_RECT_F& row) {
        const int x = static_cast<int>((row.left + row.right) / 2);
        const int y = static_cast<int>((row.top + row.bottom) / 2);
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    };
    auto vm = BuildVm(s, false);
    auto layout = ui::MakeSettingsLayout(vm, rect, 1, s.renderer.TitleBarHeight(), 28, &painter);
    click(layout.global_search_double_ctrl_row);
    check(!s.appPrefs.global_search_double_ctrl, "mouse click disables double Ctrl");
    check(s.appPrefs.lan_discovery, "search click leaves LAN discovery unchanged");
    click(layout.global_search_double_ctrl_row);
    check(s.appPrefs.global_search_double_ctrl, "second mouse click enables double Ctrl");
    s.settings.BeginGlobalSearchHotkeyCapture();
    s.settings.CaptureGlobalSearchHotkey('C', MOD_CONTROL);
    vm = BuildVm(s, false);
    check(!vm.settings_global_search_error.empty(), "search page exposes recorder conflict text");
    s.compositor.Dc()->BeginDraw();
    s.renderer.Render(vm, rect, ui::MakeTheme(false, ui::HexColor(0x0078D4)));
    check(SUCCEEDED(s.compositor.Dc()->EndDraw()), "conflict settings view renders");
    check(s.compositor.SaveSnapshot(L"search-toggle-conflict.png"), "conflict screenshot saved");
    ActiveTab(s)->current_path = app::MakeSettingsPath(app::SettingsController::PageName(12));
    s.settings.SelectPage(12);
    vm = BuildVm(s, false);
    layout = ui::MakeSettingsLayout(vm, rect, 1, s.renderer.TitleBarHeight(), 28, &painter);
    click(layout.lan_discovery_row);
    check(!s.appPrefs.lan_discovery, "network page still toggles LAN discovery");
    check(s.appPrefs.global_search_double_ctrl, "network click leaves double Ctrl unchanged");
    s.compositor.Shutdown();
    DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    state.reset();
    CoUninitialize();
    return failures ? 1 : 0;
}
