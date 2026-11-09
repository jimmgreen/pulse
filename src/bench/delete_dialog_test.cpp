// Permanent delete dialog: Enter deletes, Tab
// reaches 移到回收站, Escape cancels, the buttons click through, and there is
// no recycle button where the items have no bin. Optional argv[1] is a folder
// that receives screenshots.
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

HWND Dialog() { return FindWindowW(L"PulsePermanentDeleteWindow", nullptr); }

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

void Shot(HWND dialog, const wchar_t* name, bool dark) {
    if (g_shots.empty()) return;
    const std::wstring path = g_shots + L"\\delete-" + name + L"-" + (dark ? L"dark" : L"light") + L"-" +
                              std::to_wstring(static_cast<int>(Scale(dialog) * 100 + 0.5f)) + L".png";
    Check(SaveShot(dialog, nullptr, path), "screenshot saved");
}

bool g_dark = false;

void CALLBACK Step(HWND, UINT, UINT_PTR timer, DWORD) {
    HWND dialog = Dialog();
    if (!dialog || !IsWindowVisible(dialog)) {
        if (++g_wait > 40) { Check(false, "the delete dialog opens"); KillTimer(nullptr, timer); PostQuitMessage(1); }
        return;
    }
    if (g_step++ < 3) return; // let it paint
    KillTimer(nullptr, timer);
    RECT client{};
    GetClientRect(dialog, &client);
    // The total line adds 22 DIP below the list.
    const bool many_rows = g_scenario == 0 || g_scenario == 1 || g_scenario == 5;
    const float tall = many_rows ? 326.0f : 224.0f;
    Check(std::abs(client.bottom - static_cast<LONG>(tall * Scale(dialog))) <= 2,
          many_rows ? "four list rows and total: 326 DIP tall" : "one list row and total: 224 DIP tall");
    const float footer_y = tall - 32.0f;
    switch (g_scenario) {
    case 0: Shot(dialog, L"many", g_dark); SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0); break;
    case 1:
        SendMessageW(dialog, WM_KEYDOWN, VK_TAB, 0);
        UpdateWindow(dialog);
        Shot(dialog, L"many-tab", g_dark);
        SendMessageW(dialog, WM_KEYDOWN, VK_RETURN, 0);
        break;
    case 2: Shot(dialog, L"one-nobin", g_dark); SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0); break;
    case 3: Click(dialog, At(dialog, 440 - 24 - 40, footer_y)); break;
    case 4: // where 移到回收站 would be: nothing there, so Escape still cancels
        Click(dialog, At(dialog, 12 + 40, footer_y));
        if (IsWindow(dialog)) SendMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
        break;
    default: Click(dialog, At(dialog, 12 + 40, footer_y)); break;
    }
}

pulse::ui::PermanentDeleteChoice Run(HWND owner, const pulse::ui::PermanentDeleteInfo& info, bool dark) {
    g_step = 0;
    g_wait = 0;
    g_dark = dark;
    SetTimer(nullptr, 0, 250, Step);
    return pulse::ui::ShowPermanentDeleteDialog(owner, info, dark, D2D1_COLOR_F{ 0.0f, 0.47f, 0.83f, 1.0f });
}

std::wstring MakeFile(const std::wstring& dir, const wchar_t* name, DWORD bytes) {
    const std::wstring path = dir + L"\\" + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        size.QuadPart = bytes;
        SetFilePointerEx(file, size, nullptr, FILE_BEGIN);
        SetEndOfFile(file);
        CloseHandle(file);
    }
    return path;
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
    const std::wstring dir = std::wstring(temp) + L"pulse_delete_dialog_test";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring folder = dir + L"\\\x7D20\x6750\x5907\x4EFD"; // 素材备份
    CreateDirectoryW(folder.c_str(), nullptr);

    pulse::ui::PermanentDeleteInfo many;
    many.paths.push_back(folder);
    many.paths.push_back(MakeFile(dir, L"\x4F1A\x8BAE\x7EAA\x8981.docx", 36 * 1024));        // 会议纪要.docx
    many.paths.push_back(MakeFile(dir, L"\x73B0\x573A\x5F55\x50CF_0817.mp4", 3 * 1024 * 1024)); // 现场录像_0817.mp4
    for (int i = 0; i < 11; ++i)
        many.paths.push_back(MakeFile(dir, (L"photo_" + std::to_wstring(i) + L".jpg").c_str(), 200 * 1024));
    many.offer_recycle = true;

    WNDCLASSW owner_class{};
    owner_class.lpfnWndProc = DefWindowProcW;
    owner_class.hInstance = GetModuleHandleW(nullptr);
    owner_class.lpszClassName = L"PulseDeleteDialogTestOwner";
    RegisterClassW(&owner_class);
    HWND owner = CreateWindowExW(0, owner_class.lpszClassName, L"owner", WS_OVERLAPPEDWINDOW,
                                 80, 80, 900, 700, nullptr, nullptr, owner_class.hInstance, nullptr);

    using pulse::ui::PermanentDeleteChoice;
    g_scenario = 0;
    Check(Run(owner, many, false) == PermanentDeleteChoice::Delete, "Enter deletes (the delete button starts focused)");
    g_scenario = 1;
    Check(Run(owner, many, true) == PermanentDeleteChoice::Recycle, "Tab, Enter moves to the Recycle Bin");
    pulse::ui::PermanentDeleteInfo one;
    one.paths.push_back(many.paths[2]);
    one.no_recycle_bin = true;
    g_scenario = 2;
    Check(Run(owner, one, false) == PermanentDeleteChoice::Cancel, "Escape cancels");
    g_scenario = 3;
    Check(Run(owner, one, true) == PermanentDeleteChoice::Delete, "clicking the delete button deletes");
    g_scenario = 4;
    Check(Run(owner, one, true) == PermanentDeleteChoice::Cancel, "no recycle button where there is no bin");
    g_scenario = 5;
    Check(Run(owner, many, true) == PermanentDeleteChoice::Recycle, "clicking the recycle button recycles");

    for (size_t i = 1; i < many.paths.size(); ++i) DeleteFileW(many.paths[i].c_str());
    RemoveDirectoryW(folder.c_str());
    RemoveDirectoryW(dir.c_str());
    DestroyWindow(owner);
    Gdiplus::GdiplusShutdown(gdiplus);
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
