// menu_mnemonic_test.cpp - Keyboard access keys in the Fluent context menu.
//
// Pure checks: raw "&X" parsing, RSP_CTX_ITEMS packing, row matching and
// Explorer's keys for the built-in rows. GUI checks: a real FluentMenu on a
// real compositor receives posted key messages in its modal loop, so these
// exercise the same dispatch path as a user pressing keys over the menu.
#include "../ui/fluent_menu.h"
#include "../ipc/ctx_menu_util.h"
#include "../ipc/protocol.h"
#include "../app/context_menu.h"

#include <imm.h>

#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace pulse::ui {
struct FluentMenuTestPeer {
    static void HoverAt(FluentMenu& menu, HWND hwnd, POINT point) {
        auto* previous = menu.sub_;
        menu.sub_ = &menu.first_sub_;
        if (hwnd == menu.hwnd_) menu.OnMouse(point, false);
        else {
            for (const auto& state : menu.deeper_subs_)
                if (state->hwnd == hwnd) menu.sub_ = state.get();
            menu.OnSubMouse(point, false);
        }
        menu.sub_ = previous;
    }
    static void SaveCascade(FluentMenu& menu) {
        std::vector<std::pair<const FluentMenu::Surface*, POINT>> surfaces = {
            {&menu.surf_, {menu.base_x_, menu.base_y_}},
            {&menu.first_sub_.surf, {menu.first_sub_.x, menu.first_sub_.y}}};
        for (const auto& state : menu.deeper_subs_)
            if (state->parent_row >= 0) surfaces.push_back({&state->surf, {state->x, state->y}});
        RECT bounds{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
        for (const auto& [surface, point] : surfaces) {
            bounds.left = (std::min)(bounds.left, point.x); bounds.top = (std::min)(bounds.top, point.y);
            bounds.right = (std::max)(bounds.right, point.x + surface->w);
            bounds.bottom = (std::max)(bounds.bottom, point.y + surface->h);
        }
        const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        if (width <= 0 || height <= 0) return;
        std::vector<uint32_t> pixels(static_cast<size_t>(width) * height, 0xFFF0F0F0);
        for (const auto& [surface, point] : surfaces) {
            if (!surface->bits) continue;
            const auto* source = static_cast<const uint32_t*>(surface->bits);
            for (int y = 0; y < surface->h; ++y) for (int x = 0; x < surface->w; ++x) {
                const uint32_t value = source[y * surface->w + x];
                auto& dest = pixels[(y + point.y - bounds.top) * width + x + point.x - bounds.left];
                const unsigned inverse = 255 - (value >> 24);
                uint32_t mixed = 0xFF000000;
                for (unsigned shift : {0u, 8u, 16u}) {
                    const unsigned component = ((value >> shift) & 255) + (((dest >> shift) & 255) * inverse + 127) / 255;
                    mixed |= (std::min)(component, 255u) << shift;
                }
                dest = mixed;
            }
        }
        BITMAPFILEHEADER file{}; file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size() * 4);
        BITMAPINFOHEADER info{sizeof(info)}; info.biWidth = width; info.biHeight = -height;
        info.biPlanes = 1; info.biBitCount = 32;
        HANDLE output = CreateFileW(L"bench_data/nested_menu_fix/cascade.bmp", GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (output != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(output, &file, sizeof(file), &written, nullptr);
            WriteFile(output, &info, sizeof(info), &written, nullptr);
            WriteFile(output, pixels.data(), static_cast<DWORD>(pixels.size() * 4), &written, nullptr);
            CloseHandle(output);
        }
    }
};
}

namespace {

int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

using pulse::ui::FluentMenu;
using pulse::ui::FluentMenuItem;
using pulse::ui::FluentMenuSwatch;

FluentMenuItem Row(int command, const wchar_t* text, wchar_t key = 0, bool enabled = true) {
    FluentMenuItem item;
    item.command = command;
    item.text = text;
    item.mnemonic = key;
    item.enabled = enabled;
    return item;
}

// Layout of an item menu: built-in rows, icon strip, then Explorer rows.
std::vector<FluentMenuItem> SampleMenu() {
    std::vector<FluentMenuItem> items;
    items.push_back(Row(1, L"Open", L'O'));
    FluentMenuItem strip;
    strip.quick_swatches = {
        { 2, {}, false, false, L"\xE8C6", L'T' },
        { 3, {}, false, false, L"\xE8C8", L'C' },
    };
    items.push_back(strip);
    items.push_back(Row(10, L"\x64A4\x6D88", L'U', false));             // disabled undo
    FluentMenuItem send_to = Row(0, L"\x53D1\x9001\x5230(N)");          // 发送到(N)
    send_to.children = { Row(8101, L"Desktop (D)"), Row(8102, L"Documents (O)") };
    items.push_back(send_to);
    items.push_back(Row(8000, L"\x7F16\x8F91(E)"));                    // 编辑(E)
    items.push_back(Row(8002, L"\x6253\x5370(P)"));                    // 打印(P)
    items.push_back(Row(8003, L"Pin to Start (P)"));
    return items;
}

void PureChecks() {
    pulse::ui::FluentMenuModel model;
    auto heading = Row(0, L"Hold Shift to arrange all monitors");
    heading.heading = true;
    model.SetItems({Row(1, L"Arrange"), heading, Row(2, L"Done")});
    model.Layout(nullptr, 1.0f);
    Check(model.HitTestRow(model.RowTopPx(1) + model.RowHeightPx(1)/2) == -1,
          "instruction headings have no mouse hit target");
    Check(model.HitTestRow(model.RowTopPx(0) + model.RowHeightPx(0)/2) == 0 &&
          model.NextEnabled(0, 1) == 2, "action hover remains available and keyboard skips headings");
    using pulse::ipc::MenuMnemonic;
    Check(MenuMnemonic(L"&Edit") == L'E' && MenuMnemonic(L"Cu&t") == L'T' &&
          MenuMnemonic(L"e&x") == L'X', "raw label: access key follows a single ampersand, upper-cased");
    Check(MenuMnemonic(L"\x6253\x5F00\x65B9\x5F0F(&H)...\tCtrl+O") == L'H',
          "raw label: CJK (&H) key is read before the shortcut suffix");
    Check(MenuMnemonic(L"A && B") == 0 && MenuMnemonic(L"A && &B") == L'B',
          "raw label: && is a literal ampersand, not an access key");
    Check(MenuMnemonic(L"Trailing&") == 0 && MenuMnemonic(L"Plain") == 0 &&
          MenuMnemonic(L"& space") == 0 && MenuMnemonic(L"Tab\t&X") == 0,
          "raw label: no key for plain, dangling, blank or shortcut-only ampersands");
    Check(pulse::ipc::CleanMenuText(L"Se&nd to") == L"Send to",
          "visible text still drops the marker");

    using namespace pulse::ipc;
    const uint32_t flags = CTX_ITEM_ENABLED | CTX_ITEM_CHILD | PackCtxItemMnemonic(L'V');
    Check(UnpackCtxItemMnemonic(flags) == L'V' && (flags & 0xFFFFu) == (CTX_ITEM_ENABLED | CTX_ITEM_CHILD),
          "wire: access key rides in the flag high word without touching item bits");
    Check(UnpackCtxItemMnemonic(CTX_ITEM_ENABLED | CTX_ITEM_SEPARATOR_AFTER) == 0,
          "wire: an older host's flags decode as no access key");
    Check(UnpackCtxItemMnemonic(PackCtxItemMnemonic(L'\x0416')) == L'\x0416',
          "wire: non-Latin access keys survive packing");

    using pulse::ui::MenuItemMnemonic;
    Check(MenuItemMnemonic(Row(1, L"Whatever", L'e')) == L'E', "row: explicit key wins, case-insensitive");
    Check(MenuItemMnemonic(Row(1, L"\x7F16\x8F91(E)")) == L'E' &&
          MenuItemMnemonic(Row(1, L"\x6253\x5F00\x65B9\x5F0F(H)...")) == L'H' &&
          MenuItemMnemonic(Row(1, L"\x8FD8\x539F\x4EE5\x524D\x7684\x7248\x672C(v)\x2026")) == L'V',
          "row: trailing (X) kept from Explorer CJK labels is the key");
    Check(MenuItemMnemonic(Row(1, L"\x53D1\x9001\x5230\xFF08N\xFF09")) == L'N',
          "row: full-width parentheses also mark the key");
    Check(MenuItemMnemonic(Row(1, L"Report (final)")) == 0 && MenuItemMnemonic(Row(1, L"\x5C5E\x6027")) == 0 &&
          MenuItemMnemonic(Row(1, L"(\x4E2D)")) == 0, "row: other parentheses and plain text carry no key");

    using pulse::ui::FindMenuMnemonic;
    using pulse::ui::NextMenuMnemonicTarget;
    const auto items = SampleMenu();
    const auto cut = FindMenuMnemonic(items, L't');
    Check(cut.size() == 1 && cut[0].row == 1 && cut[0].swatch == 0, "match: icon-strip swatch by its key");
    Check(FindMenuMnemonic(items, L'U').empty(), "match: disabled rows never match");
    const auto send_to = FindMenuMnemonic(items, L'N');
    Check(send_to.size() == 1 && send_to[0].row == 3 && send_to[0].swatch == -1,
          "match: flyout header matches through its (N)");
    Check(FindMenuMnemonic(items, 0, L'e').size() == 1, "match: layout character is the second candidate");
    const auto print = FindMenuMnemonic(items, L'P');
    Check(print.size() == 2 && NextMenuMnemonicTarget(print, 0, -1) == 0 &&
          NextMenuMnemonicTarget(print, print[0].row, -1) == 1 &&
          NextMenuMnemonicTarget(print, print[1].row, -1) == 0,
          "match: shared keys cycle forward and wrap like native menus");
    Check(FindMenuMnemonic(items, L'Z').empty(), "match: unknown key matches nothing");

    using namespace pulse::app;
    Check(BuiltinMenuMnemonic(CmdOpen) == L'O' && BuiltinMenuMnemonic(CmdCut) == L'T' &&
          BuiltinMenuMnemonic(CmdCopy) == L'C' && BuiltinMenuMnemonic(CmdPaste) == L'P' &&
          BuiltinMenuMnemonic(CmdDelete) == L'D' && BuiltinMenuMnemonic(CmdRename) == L'M' &&
          BuiltinMenuMnemonic(CmdProperties) == L'R' && BuiltinMenuMnemonic(CmdUndo) == L'U' &&
          BuiltinMenuMnemonic(CmdCopyPath) == L'A' && BuiltinMenuMnemonic(CmdRefresh) == L'E' &&
          BuiltinMenuMnemonic(CmdTags) == 0,
          "built-in rows use Explorer's access keys; Pulse-only rows have none");
}

HWND g_owner = nullptr;
FluentMenu* g_menu = nullptr;
int g_owner_keys = 0;
int g_ime_probe = -1; // 1 = owner had an input context while the menu was open
constexpr UINT_PTR kWatchdog = 77;
constexpr UINT kImeProbe = WM_APP + 77;
constexpr UINT_PTR kMouseTreeTimer = 78;
int g_mouse_stage = -1;
int g_mouse_max_windows = 0;
bool g_mouse_click = false;
bool g_pointer_available = true;

BOOL CALLBACK CollectMenuWindows(HWND hwnd, LPARAM data) {
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, 64);
    if (IsWindowVisible(hwnd) && std::wstring_view(name) == L"PulseFluentMenu")
        reinterpret_cast<std::vector<HWND>*>(data)->push_back(hwnd);
    return TRUE;
}


bool HasInputContext(HWND hwnd) {
    HIMC context = ImmGetContext(hwnd);
    if (context) ImmReleaseContext(hwnd, context);
    return context != nullptr;
}

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR || msg == WM_SYSCHAR) ++g_owner_keys;
    if (msg == WM_TIMER && wparam == kMouseTreeTimer && g_mouse_stage >= 0) {
        std::vector<HWND> windows;
        EnumThreadWindows(GetCurrentThreadId(), CollectMenuWindows, reinterpret_cast<LPARAM>(&windows));
        g_mouse_max_windows = (std::max)(g_mouse_max_windows, static_cast<int>(windows.size()));
        if (windows.size() == static_cast<size_t>(g_mouse_stage + 1)) {
            // Newly shown child is the topmost menu in this thread's z-order.
            HWND target = windows.front();
            if (g_mouse_stage == 0) {
                for (HWND window : windows) {
                    wchar_t title[32]{}; GetWindowTextW(window, title, 32);
                    if (std::wstring_view(title) == L"PulseMenu") target = window;
                }
            }
            const LPARAM point = MAKELPARAM(40, 38); // center of first 28-DIP row, including shadow
            POINT screen_point{40, 38};
            ClientToScreen(target, &screen_point);
            const BOOL moved = SetCursorPos(screen_point.x, screen_point.y);
            if (!moved) {
                g_pointer_available = false;
                pulse::ui::FluentMenuTestPeer::HoverAt(*g_menu, target, {40, 38});
            } else {
                PostMessageW(target, WM_MOUSEMOVE, 0, point);
            }
            if (g_mouse_stage == 3) pulse::ui::FluentMenuTestPeer::SaveCascade(*g_menu);
            if (g_mouse_click || g_mouse_stage == 3) PostMessageW(target, WM_LBUTTONUP, 0, point);
            ++g_mouse_stage;
        }
        return 0;
    }
    if (msg == WM_TIMER && wparam == kWatchdog && g_menu) g_menu->Dismiss(); // never hang the test
    if (msg == kImeProbe) g_ime_probe = HasInputContext(hwnd) ? 1 : 0;
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// A typed-name menu like the window identity and new-tag menus: a swatch
// strip with no command of its own, then one command row.
std::vector<FluentMenuItem> NamingMenu() {
    std::vector<FluentMenuItem> items;
    FluentMenuItem colors;
    for (int i = 0; i < 3; ++i) {
        FluentMenuSwatch swatch;
        swatch.command = 10 + i;
        swatch.color = D2D1::ColorF(0.2f * static_cast<float>(i), 0.5f, 0.8f);
        colors.quick_swatches.push_back(swatch);
    }
    items.push_back(std::move(colors));
    FluentMenuItem automatic;
    automatic.command = 1;
    automatic.text = L"Automatic";
    items.push_back(std::move(automatic));
    return items;
}

// Posts the keys, then runs the real modal menu; returns the invoked command.
int Press(FluentMenu& menu, POINT at, std::initializer_list<std::pair<UINT, WPARAM>> keys) {
    g_owner_keys = 0;
    for (const auto& key : keys) PostMessageW(g_owner, key.first, key.second, 0);
    SetTimer(g_owner, kWatchdog, 3000, nullptr);
    const int command = menu.TrackPopup(at, SampleMenu());
    KillTimer(g_owner, kWatchdog);
    MSG left{};
    while (PeekMessageW(&left, g_owner, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {}
    while (PeekMessageW(&left, g_owner, WM_TIMER, WM_TIMER, PM_REMOVE)) {}
    return command;
}

void GuiChecks(bool nested_only = false, float scale = 1.0f, bool dark = false) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = OwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseMenuMnemonicOwner";
    RegisterClassW(&wc);
    const HWND foreground = GetForegroundWindow();
    g_owner = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"", WS_POPUP, -30000, -30000, 700, 300, nullptr, nullptr, wc.hInstance, nullptr);
    {
        pulse::ui::Compositor compositor;
        Check(g_owner && compositor.Init(g_owner), "create menu test compositor");
        if (!compositor.DwriteFactory()) return;
        compositor.RecreateTextFormats(scale);
        FluentMenu menu;
        menu.SetTheme(dark, D2D1::ColorF(0.2f, 0.4f, 0.8f));
        g_menu = &menu;
        Check(menu.Create(g_owner, &compositor, scale), "create real Fluent menu");
        // Open away from the cursor so a resting mouse cannot steal the hover.
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
        const LONG mid_x = (mi.rcWork.left + mi.rcWork.right) / 2;
        const LONG mid_y = (mi.rcWork.top + mi.rcWork.bottom) / 2;
        const POINT at{ cursor.x < mid_x ? mi.rcWork.right - 420 : mi.rcWork.left + 40,
                        cursor.y < mid_y ? mi.rcWork.bottom - 480 : mi.rcWork.top + 40 };

        if (nested_only) {
            menu.SetTopmost(true);
            auto leaf = Row(8123, L"Run", L'R');
            auto third = Row(0, L"Third", L'T'); third.children = {leaf};
            auto second = Row(0, L"Second", L'S'); second.children = {third, Row(8124, L"Sibling", L'B')};
            auto root = Row(0, L"Vendor", L'V'); root.children = {second};
            auto press_tree = [&](std::initializer_list<WPARAM> keys) {
                for (auto key : keys) PostMessageW(g_owner, WM_KEYDOWN, key, 0);
                SetTimer(g_owner, kWatchdog, 1000, nullptr);
                const int command = menu.TrackPopup(at, {root});
                KillTimer(g_owner, kWatchdog);
                MSG left{};
                while (PeekMessageW(&left, g_owner, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {}
                while (PeekMessageW(&left, g_owner, WM_TIMER, WM_TIMER, PM_REMOVE)) {}
                return command;
            };
            Check(press_tree({'V', 'S', 'T', 'R'}) == 8123, "four-level mnemonic cascade invokes exact leaf");
            Check(press_tree({VK_RIGHT, VK_RIGHT, VK_RIGHT, VK_RETURN}) == 8123,
                  "Right opens every nested level and Enter invokes the leaf");
            Check(press_tree({'V', 'S', 'T', VK_LEFT, VK_DOWN, VK_RETURN}) == 8124,
                  "Left closes one level and keeps parent sibling navigation");
            Check(press_tree({'V', 'S', 'T', VK_ESCAPE, VK_DOWN, VK_RETURN}) == 8124,
                  "Escape closes one level without dismissing the whole tree");
            POINT original_cursor{};
            GetCursorPos(&original_cursor);
            for (bool click : {false, true}) {
                g_mouse_stage = 0; g_mouse_max_windows = 0; g_mouse_click = click;
                SetTimer(g_owner, kMouseTreeTimer, 180, nullptr);
                SetTimer(g_owner, kWatchdog, 4000, nullptr);
                const int command = menu.TrackPopup(at, {root});
                KillTimer(g_owner, kMouseTreeTimer); KillTimer(g_owner, kWatchdog);
                g_mouse_stage = -1;
                SetCursorPos(original_cursor.x, original_cursor.y);
                std::printf("[INFO] mouse cascade command=%d visible=%d click=%d\n", command, g_mouse_max_windows, click ? 1 : 0);
                Check(command == 8123 && g_mouse_max_windows == 4,
                      click ? "mouse clicks retain all ancestor windows and invoke fourth-level leaf"
                            : "hover state machine and real dwell timer open four cards and invoke leaf");
                MSG left{};
                while (PeekMessageW(&left, g_owner, WM_TIMER, WM_TIMER, PM_REMOVE)) {}
            }
            if (!g_pointer_available) std::printf("[LIMIT] SetCursorPos failed: physical pointer hover not verified; direct OnMouse/OnSubMouse plus real modal timer verified\n");
            g_menu = nullptr;
        } else {
        Check(Press(menu, at, { { WM_KEYDOWN, VK_RETURN } }) == 1, "Enter still invokes the highlighted row");
        Check(Press(menu, at, { { WM_KEYDOWN, 'T' } }) == 2, "T invokes Cut from the icon strip");
        Check(Press(menu, at, { { WM_KEYDOWN, 'E' } }) == 8000, "E invokes the Explorer row labelled (E)");
        Check(Press(menu, at, { { WM_SYSKEYDOWN, 'E' } }) == 8000, "Alt+E works like E inside the menu");
        Check(Press(menu, at, { { WM_KEYDOWN, VK_NUMPAD0 + 0 }, { WM_KEYDOWN, VK_ESCAPE } }) == 0,
              "unmatched digit key keeps the menu open until Esc");
        Check(Press(menu, at, { { WM_KEYDOWN, 'P' }, { WM_KEYDOWN, 'P' }, { WM_KEYDOWN, VK_RETURN } }) == 8003,
              "shared key P moves between its rows without invoking; Enter picks the second");
        Check(Press(menu, at, { { WM_KEYDOWN, 'N' }, { WM_KEYDOWN, VK_RETURN } }) == 8101,
              "N opens the Send to flyout with its first row highlighted");
        Check(Press(menu, at, { { WM_KEYDOWN, 'N' }, { WM_KEYDOWN, 'O' } }) == 8102,
              "inside the flyout, O picks its own row instead of the parent's Open");
        Check(Press(menu, at, { { WM_KEYDOWN, 'U' }, { WM_KEYDOWN, VK_ESCAPE } }) == 0,
              "disabled row's key does nothing");
        Check(Press(menu, at, { { WM_KEYDOWN, 'Z' }, { WM_KEYDOWN, VK_ESCAPE } }) == 0 && g_owner_keys == 0,
              "letters never leak to the window behind the menu");

        // A filter menu opens with its first row highlighted; when that is a
        // swatch strip, Enter after typing a name used to do nothing at all.
        const auto naming = [](const std::wstring&) { return NamingMenu(); };
        menu.SetInitialFilterText(L"Review");
        PostMessageW(g_owner, WM_KEYDOWN, VK_RETURN, 0);
        SetTimer(g_owner, kWatchdog, 3000, nullptr);
        int command = menu.TrackPopup(at, NamingMenu(), naming);
        KillTimer(g_owner, kWatchdog);
        Check(command == 0 && menu.LastFilterCommitted() && menu.LastFilterQuery() == L"Review",
              "Enter on a highlighted swatch strip commits the typed name");
        menu.SetInitialFilterText(L"Review");
        PostMessageW(g_owner, WM_KEYDOWN, VK_DOWN, 0);
        PostMessageW(g_owner, WM_KEYDOWN, VK_RETURN, 0);
        SetTimer(g_owner, kWatchdog, 3000, nullptr);
        command = menu.TrackPopup(at, NamingMenu(), naming);
        KillTimer(g_owner, kWatchdog);
        Check(command == 1 && !menu.LastFilterCommitted(), "Enter on a highlighted command row still invokes it");
        MSG left{};
        while (PeekMessageW(&left, g_owner, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE)) {}
        while (PeekMessageW(&left, g_owner, WM_TIMER, WM_TIMER, PM_REMOVE)) {}

        // Chinese/Japanese IME: while a command menu is open the focused
        // window behind it has no input context, so a letter is an access key
        // and never opens a candidate window; closing gives the context back.
        SetFocus(g_owner);
        if (GetFocus() == g_owner && HasInputContext(g_owner)) {
            g_ime_probe = -1;
            Press(menu, at, { { kImeProbe, 0 }, { WM_KEYDOWN, VK_ESCAPE } });
            Check(g_ime_probe == 0, "IME is detached from the focused window while the menu is open");
            Check(HasInputContext(g_owner) && GetFocus() == g_owner,
                  "closing the menu gives the window its IME back");
            g_ime_probe = -1;
            Check(Press(menu, at, { { kImeProbe, 0 }, { WM_KEYDOWN, 'E' } }) == 8000 && g_ime_probe == 0 &&
                      HasInputContext(g_owner),
                  "access key with the IME detached invokes, then the IME is restored");
        } else {
            std::printf("[INFO] no focusable input context in this session; IME checks skipped\n");
        }
        }
        g_menu = nullptr;
    }
    DestroyWindow(g_owner);
    Check(GetForegroundWindow() == foreground, "menu tests preserve the foreground window");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool nested_only = argc > 1 && std::wstring(argv[1]) == L"--nested-only";
    if (!nested_only) PureChecks();
    if (argc > 1 && std::wstring(argv[1]) == L"--pure-only") return failures ? 1 : 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GuiChecks(nested_only, argc > 2 ? static_cast<float>(_wtof(argv[2])) : 1.0f,
              argc > 3 && std::wstring(argv[3]) == L"dark");
    CoUninitialize();
    return failures ? 1 : 0;
}
