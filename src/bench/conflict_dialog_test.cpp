// Conflict dialog: hovering a cut-off source or
// destination path shows it whole (#55), R / S / K and Alt+R pick, Tab +
// Enter picks the focused choice, the 对其余 N 个 switch carries apply_to_all,
// Escape cancels. Drives the real dialog; optional argv[1] is a folder that
// receives screenshots (path tip, idle, and each hover preview, light/dark).
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"

#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdlib>
#include <string>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

int g_passed = 0;
int g_failed = 0;
int g_step = 0;
int g_wait = 0;
bool g_tip_shown = false;
std::wstring g_source;
std::wstring g_destination;
std::wstring g_shots; // folder, empty = no screenshots
int g_scenario = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++g_passed : ++g_failed;
}

HWND Dialog() { return FindWindowW(L"PulseFileConflictWindow", nullptr); }

HWND g_found_tip = nullptr;
BOOL CALLBACK FindTip(HWND hwnd, LPARAM owner) {
    wchar_t name[64]{};
    GetClassNameW(hwnd, name, ARRAYSIZE(name));
    if (_wcsicmp(name, TOOLTIPS_CLASSW) == 0 && GetWindow(hwnd, GW_OWNER) == reinterpret_cast<HWND>(owner)) {
        g_found_tip = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND Tooltip(HWND dialog) {
    g_found_tip = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindTip, reinterpret_cast<LPARAM>(dialog));
    return g_found_tip;
}

float Scale(HWND hwnd) { return static_cast<float>(pulse::compat::WindowDpi(hwnd)) / 96.0f; }

POINT At(HWND hwnd, float x, float y) {
    const float scale = Scale(hwnd);
    return POINT{ static_cast<LONG>(x * scale), static_cast<LONG>(y * scale) };
}

// Text of the tool under a client point, or empty when there is none.
std::wstring TipAt(HWND tip, HWND dialog, POINT pt) {
    wchar_t text[1024]{};
    TTHITTESTINFOW hit{};
    hit.hwnd = dialog;
    hit.pt = pt;
    hit.ti.cbSize = sizeof(hit.ti);
    hit.ti.lpszText = text;
    if (!SendMessageW(tip, TTM_HITTESTW, 0, reinterpret_cast<LPARAM>(&hit))) return {};
    TOOLINFOW info{ sizeof(info) };
    info.hwnd = dialog;
    info.uId = hit.ti.uId;
    info.lpszText = text;
    SendMessageW(tip, TTM_GETTEXTW, ARRAYSIZE(text), reinterpret_cast<LPARAM>(&info));
    return text;
}

void Click(HWND dialog, POINT pt) {
    const LPARAM where = MAKELPARAM(pt.x, pt.y);
    SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, where);
    SendMessageW(dialog, WM_LBUTTONUP, 0, where);
    UpdateWindow(dialog);
}

HBITMAP Capture(HWND hwnd, SIZE& size) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    size = SIZE{ rect.right - rect.left, rect.bottom - rect.top };
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, size.cx, size.cy);
    HGDIOBJ old = SelectObject(memory, bitmap);
    PrintWindow(hwnd, memory, PW_RENDERFULLCONTENT);
    SelectObject(memory, old);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return bitmap;
}

// Dialog plus its tooltip, composed where they sit on screen.
bool SaveShot(HWND dialog, HWND tip, const std::wstring& path) {
    RECT dialog_rect{}, tip_rect{};
    GetWindowRect(dialog, &dialog_rect);
    if (tip) GetWindowRect(tip, &tip_rect);
    RECT all = dialog_rect;
    if (tip) UnionRect(&all, &dialog_rect, &tip_rect);
    SIZE dialog_size{}, tip_size{};
    HBITMAP dialog_bits = Capture(dialog, dialog_size);
    HBITMAP tip_bits = tip ? Capture(tip, tip_size) : nullptr;

    Gdiplus::Bitmap canvas(all.right - all.left, all.bottom - all.top, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&canvas);
        graphics.Clear(Gdiplus::Color(255, 96, 96, 96));
        Gdiplus::Bitmap dialog_image(dialog_bits, nullptr);
        graphics.DrawImage(&dialog_image, static_cast<INT>(dialog_rect.left - all.left),
                           static_cast<INT>(dialog_rect.top - all.top));
        if (tip_bits) {
            Gdiplus::Bitmap tip_image(tip_bits, nullptr);
            graphics.DrawImage(&tip_image, static_cast<INT>(tip_rect.left - all.left),
                               static_cast<INT>(tip_rect.top - all.top));
        }
    }
    DeleteObject(dialog_bits);
    if (tip_bits) DeleteObject(tip_bits);
    const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    return canvas.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
}

// Layout in DIPs (file_operation_dialog.cpp ConflictWindow).
constexpr float kSourcePathX = 266.5f + 100, kDestinationPathX = 24 + 100, kPathY = 82 + 90 + 8;
constexpr float kRowX = 280;
float RowY(int index) { return 204 + index * 56.0f + 27; }
constexpr float kApplyY = 390 + 32;

void Shot(HWND dialog, HWND tip, const wchar_t* name) {
    if (g_shots.empty()) return;
    const std::wstring path = g_shots + L"\\conflict-" + name + L"-" + (g_scenario == 0 ? L"light" : L"dark") + L"-" +
                              std::to_wstring(static_cast<int>(Scale(dialog) * 100 + 0.5f)) + L".png";
    Check(SaveShot(dialog, tip, path), "screenshot saved");
}

// Scenario 0: one conflict, light. Path tips, height, then Escape.
void StepTips(HWND dialog, UINT_PTR timer) {
    HWND tip = Tooltip(dialog);
    const POINT source_box = At(dialog, kSourcePathX, kPathY);
    const POINT destination_box = At(dialog, kDestinationPathX, kPathY);
    if (!g_tip_shown && g_step < 1000) {
        Check(tip != nullptr, "the dialog owns a tooltip");
        if (!tip) { KillTimer(nullptr, timer); PostMessageW(dialog, WM_CLOSE, 0, 0); return; }
        UpdateWindow(dialog);
        RECT client{};
        GetClientRect(dialog, &client);
        Check(std::abs(client.bottom - static_cast<LONG>(454 * Scale(dialog))) <= 2,
              "a lone conflict is 454 DIP tall");
        Check(TipAt(tip, dialog, source_box) == g_source, "hovering the source path shows it whole");
        Check(TipAt(tip, dialog, destination_box) == g_destination,
              "hovering the destination path shows it whole");
        Check(TipAt(tip, dialog, At(dialog, kRowX, RowY(0))).empty(), "the choice rows show no path tip");
        g_step = 1000;
        if (!g_shots.empty()) {
            Shot(dialog, nullptr, L"idle");
            // A real hover needs the user's mouse; pin the same tip instead.
            wchar_t text[1024]{};
            TOOLINFOW info{ sizeof(info) };
            info.hwnd = dialog;
            info.uId = 1;
            info.lpszText = text;
            SendMessageW(tip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&info));
            info.uFlags |= TTF_TRACK | TTF_ABSOLUTE;
            SendMessageW(tip, TTM_SETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&info));
            POINT screen = source_box;
            ClientToScreen(dialog, &screen);
            SendMessageW(tip, TTM_TRACKPOSITION, 0, MAKELPARAM(screen.x, screen.y + 22));
            SendMessageW(tip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
            g_tip_shown = true;
            return; // capture on the next tick, once both windows painted
        }
    }
    if (g_tip_shown) {
        Check(IsWindowVisible(tip) != FALSE, "the pinned tip is visible");
        Shot(dialog, tip, L"tip");
        g_tip_shown = false;
    }
    KillTimer(nullptr, timer);
    SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
}

// Scenario 1: conflict 2 of 4 while moving, dark. Keyboard focus previews
// each choice the way hover does (a synthetic WM_MOUSEMOVE is undone by the
// WM_MOUSELEAVE the real cursor triggers), the switch, then K.
void Key(HWND dialog, WPARAM key) { SendMessageW(dialog, WM_KEYDOWN, key, 0); }

void StepPreview(HWND dialog, UINT_PTR timer) {
    switch (g_step++) {
    case 0: {
        RECT client{};
        GetClientRect(dialog, &client);
        Check(std::abs(client.bottom - static_cast<LONG>(454 * Scale(dialog))) <= 2,
              "more conflicts keep the height; the checkbox sits in the footer (454 DIP)");
        Shot(dialog, nullptr, L"idle");
        Key(dialog, VK_TAB);
        return;
    }
    case 2: Shot(dialog, nullptr, L"replace"); Key(dialog, VK_TAB); return;
    case 4: Shot(dialog, nullptr, L"skip"); Key(dialog, VK_TAB); return;
    case 6: Shot(dialog, nullptr, L"keep"); Click(dialog, At(dialog, kRowX, kApplyY)); return;
    case 7: Shot(dialog, nullptr, L"apply"); return;
    case 8:
        KillTimer(nullptr, timer);
        Key(dialog, 'K');
        return;
    default: return;
    }
}

// Scenario 2: Tab, Tab, Enter picks the second choice (skip).
void StepKeyboard(HWND dialog, UINT_PTR timer) {
    KillTimer(nullptr, timer);
    SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
    SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
    SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
}

// Scenario 3: Alt+R replaces.
void StepAlt(HWND dialog, UINT_PTR timer) {
    KillTimer(nullptr, timer);
    SendMessageW(dialog, WM_SYSKEYDOWN, 'R', 1u << 29);
}

void CALLBACK Step(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = Dialog();
    if (!dialog || !IsWindowVisible(dialog)) {
        if (++g_wait > 40) { Check(false, "the conflict dialog opens"); KillTimer(nullptr, timer); PostQuitMessage(1); }
        return;
    }
    if (g_scenario == 0) StepTips(dialog, timer);
    else if (g_scenario == 1) StepPreview(dialog, timer);
    else if (g_scenario == 2) StepKeyboard(dialog, timer);
    else StepAlt(dialog, timer);
}

pulse::ui::ConflictDialogResult Run(HWND owner, const pulse::ops::ConflictItemInfo& conflict, bool dark) {
    g_step = 0;
    g_wait = 0;
    SetTimer(nullptr, 0, g_scenario == 1 ? 300 : 250, Step);
    return pulse::ui::ShowFileConflictDialog(owner, conflict, dark, D2D1_COLOR_F{ 0.0f, 0.47f, 0.83f, 1.0f });
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    pulse::compat::EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gdiplus_input, nullptr);
    if (argc > 1 && argv[1][0]) { g_shots = argv[1]; CreateDirectoryW(g_shots.c_str(), nullptr); }
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), argc > 2 ? argv[2] : L"zh-CN");

    g_source = L"C:\\Users\\Example\\AppData\\Local\\Temp\\PulseDrop\\4242-1-90210\\7zE44D7D628"
               L"\\Tools\\Diagnostics\\Release\\x64\\SFM.dll";
    g_destination = L"D:\\Projects\\Archive\\Third Party\\Long Folder Name For Wrapping\\Release"
                    L"\\x64\\SFM.dll";
    pulse::ops::ConflictItemInfo conflict;
    conflict.source = g_source;
    conflict.destination = g_destination;
    conflict.source_size = 412160;
    conflict.destination_size = 409600;
    GetSystemTimeAsFileTime(&conflict.source_modified);
    conflict.destination_modified = conflict.source_modified;
    conflict.remaining = 1;

    WNDCLASSW owner_class{};
    owner_class.lpfnWndProc = DefWindowProcW;
    owner_class.hInstance = GetModuleHandleW(nullptr);
    owner_class.lpszClassName = L"PulseConflictDialogTestOwner";
    RegisterClassW(&owner_class);
    HWND owner = CreateWindowExW(0, owner_class.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW,
                                 80, 80, 900, 700, nullptr, nullptr, owner_class.hInstance, nullptr);

    g_scenario = 0;
    auto result = Run(owner, conflict, false);
    Check(result.choice == pulse::ops::ConflictChoice::Cancel, "Escape cancels the dialog");

    // 2 of 4 while moving: source newer, keep-both name known.
    pulse::ops::ConflictItemInfo many = conflict;
    many.source = L"C:\\Users\\Example\\Pictures\\Trip\\IMG_2048.jpg";
    many.destination = L"D:\\Backup\\Pictures\\IMG_2048.jpg";
    many.source_size = 5242880;
    many.destination_size = 4718592;
    ULARGE_INTEGER older{};
    older.LowPart = conflict.source_modified.dwLowDateTime;
    older.HighPart = conflict.source_modified.dwHighDateTime;
    older.QuadPart -= 86400ull * 10000000ull * 3;
    many.destination_modified.dwLowDateTime = older.LowPart;
    many.destination_modified.dwHighDateTime = older.HighPart;
    many.remaining = 3;
    many.ordinal = 2;
    many.type = pulse::ops::OpType::Move;
    many.keep_both_name = L"IMG_2048 - \x526F\x672C.jpg";
    g_scenario = 1;
    result = Run(owner, many, true);
    Check(result.choice == pulse::ops::ConflictChoice::KeepBoth, "K keeps both");
    Check(result.apply_to_all, "the switch applies the choice to the rest");

    g_scenario = 2;
    result = Run(owner, many, true);
    Check(result.choice == pulse::ops::ConflictChoice::Skip, "Tab, Tab, Enter picks skip");
    Check(!result.apply_to_all, "the switch starts off");

    g_scenario = 3;
    result = Run(owner, conflict, false);
    Check(result.choice == pulse::ops::ConflictChoice::Replace, "Alt+R replaces");

    DestroyWindow(owner);
    Gdiplus::GdiplusShutdown(gdiplus);
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
