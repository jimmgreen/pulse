#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../common/localization.h"
#include "../ui/ui_renderer.h"
#include <filesystem>
#include <iostream>

void Render(pulse::AppState&);

namespace {
LRESULT CALLBACK Owner(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<pulse::AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
        if (msg == WM_LBUTTONDOWN) return pulse::HandleLButtonDown(state, hwnd, msg, wp, lp);
        if (msg == WM_LBUTTONUP) return pulse::HandleLButtonUp(state, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::wstring EditText(HWND edit) {
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
    value.resize(GetWindowTextW(edit, value.data(), static_cast<int>(value.size())));
    return value;
}

bool SaveEditor(pulse::AppState& s, const std::filesystem::path& path) {
    RECT rc{};
    GetClientRect(s.hwndAddressEdit, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rc.right;
    info.bmiHeader.biHeight = -rc.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    const HDC dc = CreateCompatibleDC(nullptr);
    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return false;
    }
    const auto old = SelectObject(dc, bitmap);
    bool ok = s.compositor.PaintLumaEdit(s.hwndAddressEdit, dc, s.compositor.TextFormat(),
        pulse::HostedEditForeground(s), pulse::HostedEditBackground(s));
    GdiFlush();
    FILE* file = nullptr;
    if (ok && _wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
        header.bfSize = header.bfOffBits + rc.right * rc.bottom * 4;
        ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
            fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file) == 1 &&
            fwrite(pixels, static_cast<size_t>(rc.right) * rc.bottom * 4, 1, file) == 1;
        fclose(file);
    } else ok = false;
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return ok;
}
}

int main(int argc, char** argv) {
    using namespace pulse;
    wchar_t guarded[8]{};
    if (!GetEnvironmentVariableW(L"PULSE_BREADCRUMB_CLIPBOARD_GUARDED", guarded, ARRAYSIZE(guarded))) {
        std::cout << "[FAIL] launch with run-native.ps1 to preserve the clipboard" << std::endl;
        return 2;
    }
    const auto desktop_name = L"PulseBreadcrumbProbe-" + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) {
        std::cout << "[FAIL] isolated Windows desktop error=" << GetLastError() << std::endl;
        return 2;
    }
    std::cout << "[INFO] real Win32 windows on an isolated desktop; no desktop switch or physical input; clipboard guarded by runner" << std::endl;
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(argc > 1 ? argv[1] : "bench_data/address-long-name-20261124/live");
    std::filesystem::create_directories(output);
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !ok;
    };
    WNDCLASSW wc{};
    wc.lpfnWndProc = Owner;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseBreadcrumbAddressTest";
    RegisterClassW(&wc);
    const std::wstring name = std::wstring(120, L'长') + std::wstring(135, L'n');
    const auto parent = output / L"fixtures";
    std::filesystem::create_directories(parent);
    const auto short_path = fs::NormalizePath((parent / L"short").wstring());
    const auto long_path = fs::NormalizePath((parent / name).wstring());
    check(CreateDirectoryW(short_path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS,
        "real short directory exists");
    check(CreateDirectoryW(long_path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS,
        "real 255-unit directory exists beyond MAX_PATH");
    std::cout << "[INPUT] name_units=" << name.size() << " path_units=" << ClipboardPath(long_path).size() << std::endl;
    for (float scale : {1.0f, 1.5f}) for (bool dark : {false, true}) for (int width : {1100, 650}) {
        std::cout << "[CASE] scale=" << scale << " dark=" << dark << " width=" << width << std::endl;
        const auto image_name = std::to_wstring(width) + L"-" + std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark" : L"-light");
        auto owned = std::make_unique<AppState>();
        auto& s = *owned;
        s.isolatedTest = true;
        s.appPrefs.persist = s.searchHistory.persist = s.places.persist = false;
        s.appPrefs.show_hints = false;
        s.scale = scale;
        s.darkMode = dark;
        // Real HWND, compositor, production hit testing and input handlers;
        // off-screen so this probe does not cover the user's working windows.
        s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName,
            L"Pulse breadcrumb address regression", WS_POPUP, -20000, -20000, width, 720,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!s.hwnd || !s.compositor.Init(s.hwnd)) {
            check(false, "native window and compositor initialize");
            if (s.hwnd) DestroyWindow(s.hwnd);
            continue;
        }
        s.renderer.SetCompositor(&s.compositor);
        s.compositor.RecreateTextFormats(scale);
        s.renderer.SetScale(scale);
        s.window_tabs.EnsureDefault();
        s.pane = s.window_tabs.Active()->FocusedPane();
        auto* tab = ActiveTab(s);
        SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
        ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
        auto point_for = [&](ui::HitTestResult::Region region, const std::wstring& path, POINT& point) {
            const auto vm = BuildVm(s, false);
            const auto rect = D2D1::RectF(0, 0, static_cast<float>(width), 720);
            const auto address = s.renderer.AddressBarRect(static_cast<float>(width));
            const int y = static_cast<int>((address.top + address.bottom) / 2);
            for (int x = static_cast<int>(address.left) + 1; x < static_cast<int>(address.right); ++x) {
                const auto hit = s.renderer.HitTest(vm, rect, static_cast<float>(x), static_cast<float>(y));
                if (hit.region == region && (path.empty() || hit.path == path)) {
                    point = {x, y};
                    return true;
                }
            }
            return false;
        };
        auto click = [&](POINT point) {
            SendMessageW(s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
            SendMessageW(s.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
            Render(s);
        };
        for (const auto& path : {short_path, long_path}) {
            tab->NavigateTo(path);
            tab->loading = false;
            Render(s);
            POINT point{};
            const bool located = point_for(ui::HitTestResult::BreadcrumbSegment, ClipboardPath(path), point);
            check(located, "current directory has a visible text hit target");
            if (!located) continue;
            const auto generation = tab->view_generation;
            click(point);
            const bool editing = s.addressEditing && !s.addressSearching && s.hwndAddressEdit && IsWindowVisible(s.hwndAddressEdit);
            check(editing, "single current-directory click opens actual editor");
            if (!editing) continue;
            check(tab->current_path == path && tab->view_generation == generation,
                "editing does not renavigate or reload the directory");
            check(EditText(s.hwndAddressEdit) == ClipboardPath(path), "editor contains full untruncated path");
            check(GetFocus() == s.hwndAddressEdit, "native editor owns focus");
            DWORD first = 0, last = 0;
            SendMessageW(s.hwndAddressEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
            check(first == 0 && last == ClipboardPath(path).size(), "opening selects the full path");
            const auto shown = ClipboardPath(path);
            const auto start = shown.find_last_of(L'\\') + 1;
            SendMessageW(s.hwndAddressEdit, EM_SETSEL, start, shown.size());
            {
                SendMessageW(s.hwndAddressEdit, WM_COPY, 0, 0);
                std::wstring copied;
                if (OpenClipboard(s.hwnd)) {
                    const auto data = GetClipboardData(CF_UNICODETEXT);
                    const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
                    if (text) { copied = text; GlobalUnlock(data); }
                    CloseClipboard();
                }
                check(copied == shown.substr(start), "WM_COPY copies only the complete folder name");
            }
            if (path == long_path) check(SaveEditor(s, output / (image_name + L"-editor.bmp")), "actual editor state rendered for visual inspection");
            SendMessageW(s.hwndAddressEdit, EM_SETSEL, shown.size(), shown.size());
            SendMessageW(s.hwndAddressEdit, WM_CHAR, L'x', 0);
            check(EditText(s.hwndAddressEdit) == shown + L"x", "long path accepts keyboard editing at the end");
            SendMessageW(s.hwndAddressEdit, WM_CHAR, VK_BACK, 0);
            check(EditText(s.hwndAddressEdit) == shown, "Backspace restores the complete path");
            SendMessageW(s.hwndAddressEdit, WM_KEYDOWN, VK_ESCAPE, 0);
            check(!s.addressEditing && tab->current_path == path, "Escape ends editing without navigation");
            Render(s);
            click(point);
            check(s.addressEditing, "current-directory click reopens editor");
            HideAddressEditor(s, false);
        }
        tab->NavigateTo(short_path);
        tab->loading = false;
        Render(s);
        POINT blank{};
        if (point_for(ui::HitTestResult::AddressBar, L"", blank)) {
            click(blank);
            check(s.addressEditing, "blank address area still opens editor");
            HideAddressEditor(s, false);
        }
        POINT ancestor{};
        if (point_for(ui::HitTestResult::BreadcrumbSegment, ClipboardPath(fs::NormalizePath(parent.wstring())), ancestor)) {
            click(ancestor);
            check(!s.addressEditing && tab->current_path == fs::NormalizePath(parent.wstring()),
                "ancestor text still navigates to the real parent directory");
        } else std::cout << "[INFO] ancestor hidden at this width; covered in wider layouts" << std::endl;
        tab->NavigateTo(long_path);
        tab->loading = false;
        // An occluded swap chain rotates to the previous buffer after Present.
        // Capture the freshly rendered frame instead of that stale buffer.
        s.shot.active = true;
        s.shot.start = std::chrono::steady_clock::now();
        SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", L"1");
        Render(s);
        Sleep(1500); // Let newly navigated breadcrumb labels finish their entrance animation.
        Render(s);
        const auto image = output / (image_name + L".png");
        check(s.compositor.SaveSnapshot(image.c_str()), "actual compositor snapshot saved");
        s.shot.active = false;
        SetEnvironmentVariableW(L"PULSE_TEST_HIDDEN_SHOT", nullptr);
        SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0);
        s.addressIgnoreKillFocus = true;
        if (s.hwndAddressEdit) DestroyWindow(s.hwndAddressEdit);
        s.hwndAddressEdit = nullptr;
        s.renderer.SetCompositor(nullptr);
        s.compositor.Shutdown();
        DestroyWindow(s.hwnd);
        s.hwnd = nullptr;
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::cout << "failures=" << failures << std::endl;
    OleUninitialize();
    return failures ? 1 : 0;
}
