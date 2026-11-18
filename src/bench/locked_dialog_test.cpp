// File-in-use dialog: Escape cancels, Enter on
// the default focus never ends processes, Tab reaches 结束进程并重试, 跳过此项
// clicks through, and once the item is free the dialog continues on its own
// (Retry) without a click. Optional argv[1] is a folder for screenshots.
#include "../common/windows_compat.h"
#include "../common/localization.h"
#include "../ui/file_operation_dialog.h"

#include <windows.h>
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
std::wstring g_shots; // folder, empty = no screenshots
int g_scenario = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++g_passed : ++g_failed;
}

HWND Dialog() { return FindWindowW(L"PulseLockedItemWindow", nullptr); }

float Scale(HWND hwnd) { return static_cast<float>(pulse::compat::WindowDpi(hwnd)) / 96.0f; }

POINT At(HWND hwnd, float x, float y) {
    const float scale = Scale(hwnd);
    return POINT{ static_cast<LONG>(x * scale), static_cast<LONG>(y * scale) };
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

bool g_dark = false;
HANDLE g_hold = INVALID_HANDLE_VALUE;
std::wstring g_file;
ULONGLONG g_released_at = 0;

void Shot(HWND dialog, const wchar_t* name) {
    if (g_shots.empty()) return;
    const std::wstring path = g_shots + L"\\locked-" + name + L"-" + (g_dark ? L"dark" : L"light") + L"-" +
                              std::to_wstring(static_cast<int>(Scale(dialog) * 100 + 0.5f)) + L".png";
    Check(SaveShot(dialog, nullptr, path), "screenshot saved");
}

void CheckOwnerIcons(HWND dialog) {
    SIZE size{};
    HBITMAP bitmap = Capture(dialog, size);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bitmap);
    POINT origin{};
    ClientToScreen(dialog, &origin);
    RECT window{};
    GetWindowRect(dialog, &window);
    const float scale = Scale(dialog);
    int different = 0;
    for (int y = 0; y < static_cast<int>(24 * scale); ++y) {
        for (int x = 0; x < static_cast<int>(24 * scale); ++x) {
            const int px = origin.x - window.left + static_cast<int>(37 * scale) + x;
            const int py = origin.y - window.top + static_cast<int>(99 * scale) + y;
            if (GetPixel(dc, px, py) != GetPixel(dc, px, py + static_cast<int>(56 * scale))) ++different;
        }
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bitmap);
    std::printf("owner icon differing pixels: %d\n", different);
    Check(different > 40, "the rendered owner icons are distinct");
}

void CALLBACK Step(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = Dialog();
    if (!dialog || !IsWindowVisible(dialog)) {
        if (++g_wait > 40) { Check(false, "the locked dialog opens"); KillTimer(nullptr, timer); PostQuitMessage(1); }
        return;
    }
    const int step = g_step++;
    switch (g_scenario) {
    case 0: // two programs, one with a window: look, then Escape
        if (step < 2) return;
        KillTimer(nullptr, timer);
        Shot(dialog, L"owners");
        CheckOwnerIcons(dialog);
        SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
        return;
    case 1: // closable owners: Enter on the default focus cancels
        if (step < 2) return;
        KillTimer(nullptr, timer);
        Shot(dialog, L"end");
        SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
        return;
    case 2: // Left (Shift+Tab) from 取消 reaches the end-and-retry button
        KillTimer(nullptr, timer);
        SendMessageW(dialog, WM_KEYDOWN, VK_LEFT, 0);
        SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
        return;
    case 4: // release the file: the dialog continues by itself
        if (step == 1) {
            CloseHandle(g_hold);
            g_hold = INVALID_HANDLE_VALUE;
            g_released_at = GetTickCount64();
        }
        if (step == 6) Shot(dialog, L"released"); // 1.5 s: two free reads seen, 900 ms hold running
        if (step > 40) { KillTimer(nullptr, timer); SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0); }
        return;
    default:
        KillTimer(nullptr, timer);
        return;
    }
}

pulse::ui::LockedItemChoice Run(HWND owner, const pulse::ui::LockedItemInfo& info, bool dark) {
    g_step = 0;
    g_wait = 0;
    g_dark = dark;
    SetTimer(nullptr, 0, 250, Step);
    return pulse::ui::ShowLockedItemDialog(owner, info, dark, D2D1_COLOR_F{ 0.0f, 0.47f, 0.83f, 1.0f });
}

PROCESS_INFORMATION Spawn() {
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, ARRAYSIZE(system));
    std::wstring command = L"\"" + std::wstring(system) +
        L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoLogo -NoProfile -NonInteractive -Command \"Start-Sleep -Seconds 120\"";
    CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    return process;
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

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    g_file = std::wstring(temp) + L"locked_dialog_test.txt";
    g_hold = CreateFileW(g_file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    Check(g_hold != INVALID_HANDLE_VALUE, "the test holds its file");

    WNDCLASSW owner_class{};
    owner_class.lpfnWndProc = DefWindowProcW;
    owner_class.hInstance = GetModuleHandleW(nullptr);
    owner_class.lpszClassName = L"PulseLockedDialogTestOwner";
    // A real window-icon fixture exercises the borrowed class-icon path.
    owner_class.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    RegisterClassW(&owner_class);
    HWND owner = CreateWindowExW(0, owner_class.lpszClassName, L"Locked dialog test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 80, 80, 900, 700, nullptr, nullptr, owner_class.hInstance, nullptr);

    PROCESS_INFORMATION helper_process = Spawn();
    Check(helper_process.hProcess != nullptr, "a real Windows PowerShell process runs");

    pulse::ui::LockedItemInfo info;
    info.type = pulse::ops::OpType::RecycleDelete;
    info.path = g_file;
    pulse::ops::LockOwner self;
    self.pid = GetCurrentProcessId();
    self.app_name = L"Pulse dialog test (window icon fixture)";
    self.image_name = L"pulse_locked_dialog_test.exe";
    self.closable = false;
    pulse::ops::LockOwner helper;
    helper.pid = helper_process.dwProcessId;
    helper.app_name = L"Windows PowerShell";
    helper.image_name = L"powershell.exe";
    info.owners = { self, helper };

    using pulse::ui::LockedItemChoice;
    g_scenario = 0;
    Check(Run(owner, info, true) == LockedItemChoice::Cancel, "Escape cancels");

    pulse::ui::LockedItemInfo closable = info;
    closable.owners = { helper };
    closable.can_end = true;
    closable.can_skip = true;
    g_scenario = 1;
    Check(Run(owner, closable, false) == LockedItemChoice::Cancel, "Enter on the default focus does not end processes");
    g_scenario = 2;
    Check(Run(owner, closable, false) == LockedItemChoice::EndAndRetry, "Left, Enter ends and retries");

    // Skip: footer y depends on the height; read it from the window.
    g_scenario = 3;
    {
        // Click at the skip button: the 64-DIP footer, button centred, from x 12.
        struct Local {
            static void CALLBACK SkipStep(HWND, UINT, UINT_PTR timer, DWORD) {
                HWND dialog = Dialog();
                if (!dialog || !IsWindowVisible(dialog)) return;
                KillTimer(nullptr, timer);
                RECT client{};
                GetClientRect(dialog, &client);
                const float scale = Scale(dialog);
                const float footer = client.bottom / scale - 64.0f;
                Click(dialog, At(dialog, 12 + 40, footer + 32));
            }
        };
        g_step = 0;
        g_wait = 0;
        SetTimer(nullptr, 0, 250, Local::SkipStep);
        Check(pulse::ui::ShowLockedItemDialog(owner, closable, false, D2D1_COLOR_F{ 0.0f, 0.47f, 0.83f, 1.0f }) ==
                  LockedItemChoice::Skip, "Skip clicks through");
    }

    g_scenario = 4;
    const ULONGLONG started = GetTickCount64();
    const auto released = Run(owner, info, true);
    const ULONGLONG took = GetTickCount64() - (g_released_at ? g_released_at : started);
    Check(released == LockedItemChoice::Retry, "the dialog continues once the file is free");
    std::printf("continued %llu ms after the release\n", took);
    Check(took >= 900 && took < 4000, "it continues after the 900 ms hold, within a few polls");

    if (g_hold != INVALID_HANDLE_VALUE) CloseHandle(g_hold);
    DeleteFileW(g_file.c_str());
    if (helper_process.hProcess) {
        TerminateProcess(helper_process.hProcess, 0);
        WaitForSingleObject(helper_process.hProcess, 5000);
        CloseHandle(helper_process.hProcess);
        CloseHandle(helper_process.hThread);
    }
    DestroyWindow(owner);
    Gdiplus::GdiplusShutdown(gdiplus);
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
