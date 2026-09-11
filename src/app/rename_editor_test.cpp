#ifdef PULSE_WITH_SELFTEST
#include "rename_editor_test.h"
#include "app_hosted_edit.h"
#include "app_input.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include <cstdio>
#include <filesystem>
#include <share.h>
#include <dwmapi.h>

namespace pulse::app {
namespace {
LRESULT CALLBACK TestOwner(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
        if (msg == WM_LBUTTONDOWN) return HandleLButtonDown(state, hwnd, msg, wp, lp);
        if (msg == WM_LBUTTONUP) return HandleLButtonUp(state, hwnd, msg, wp, lp);
        if (msg == WM_CTLCOLOREDIT) {
            EnsureEditVisuals(*state);
            const auto dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, state->darkMode ? RGB(255, 255, 255) : RGB(26, 26, 26));
            SetBkColor(dc, state->darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(state->editBrush);
        }
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            auto* dc = state->compositor.Dc();
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0.0f));
            auto vm = BuildVm(*state, false);
            state->renderer.Render(vm, D2D1::RectF(0, 0, 1000, 700), ui::MakeTheme(state->darkMode, state->accentColor));
            dc->EndDraw();
            state->compositor.Present();
            if (state->renameIndex >= 0) LayoutRenameOverlay(*state);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
struct EditorPixels {
    int glyphs = 0;
    int selection = 0;
};
bool CaptureEditor(HWND edit, const wchar_t* path, AppState* rendered_state = nullptr,
                   EditorPixels* sampled = nullptr, bool dark = false) {
    RECT rc{};
    GetWindowRect(edit, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC dc = CreateCompatibleDC(screen);
    if (!dc) { ReleaseDC(nullptr, screen); return false; }
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return false;
    }
    const auto old = SelectObject(dc, bitmap);
    DwmFlush();
    bool ok = rendered_state
        ? rendered_state->compositor.PaintLumaEdit(edit, dc, rendered_state->compositor.TextFormat(),
            HostedEditForeground(*rendered_state), HostedEditBackground(*rendered_state))
        : BitBlt(dc, 0, 0, width, height, screen, rc.left, rc.top, SRCCOPY | CAPTUREBLT) != FALSE;
    GdiFlush();
    if (ok && sampled) {
        *sampled = {};
        const auto* bytes = static_cast<const BYTE*>(pixels);
        const auto selected_pixel = [&](int index) {
            return bytes[index * 4] > bytes[index * 4 + 2] + 15 &&
                bytes[index * 4 + 1] > bytes[index * 4 + 2] + 10;
        };
        for (int i = 0; i < width * height; ++i) {
            const int blue = bytes[i * 4];
            const int green = bytes[i * 4 + 1];
            const int red = bytes[i * 4 + 2];
            if (blue > red + 15 && green > red + 10) ++sampled->selection;
            if (dark ? (red > 160 && green > 160 && blue > 160)
                     : (red < 100 && green < 100 && blue < 100)) ++sampled->glyphs;
            // Native selection uses white glyphs even in the light theme.
            if (!dark && red > 225 && green > 225 && blue > 225) {
                const int x = i % width;
                bool left = false, right = false;
                for (int offset = 1; offset <= 4; ++offset) {
                    if (x >= offset) left |= selected_pixel(i - offset);
                    if (x + offset < width) right |= selected_pixel(i + offset);
                }
                if (left && right) ++sampled->glyphs;
            }
        }
    }
    FILE* file = nullptr;
    if (ok && _wfopen_s(&file, path, L"wb") == 0 && file) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
        header.bfSize = header.bfOffBits + width * height * 4;
        ok = fwrite(&header, sizeof(header), 1, file) == 1 &&
            fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, file) == 1 &&
            fwrite(pixels, static_cast<size_t>(width) * height * 4, 1, file) == 1;
        fclose(file);
    } else ok = false;
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return ok;
}
}

bool RunAddressEditorTest() {
    const auto output = std::filesystem::absolute(L"bench_data/address-editor");
    std::filesystem::create_directories(output);
    FILE* log = _fsopen("bench_data/address-editor/results.log", "wN", _SH_DENYNO);
    bool ok = log != nullptr;
    const auto check = [&](bool value, const char* label) {
        if (log) { fprintf(log, "[%s] %s\n", value ? "PASS" : "FAIL", label); fflush(log); }
        ok &= value;
    };
    auto state = std::make_unique<AppState>();
    state->isolatedTest = true;
    state->places.persist = state->appPrefs.persist = state->searchHistory.persist = false;
    wchar_t option[8]{};
    const bool capture = GetEnvironmentVariableW(L"PULSE_ADDRESS_EDITOR_CAPTURE", option, ARRAYSIZE(option)) > 0;
    if (GetEnvironmentVariableW(L"PULSE_ADDRESS_EDITOR_DARK", option, ARRAYSIZE(option)))
        state->darkMode = option[0] == L'1';
    if (GetEnvironmentVariableW(L"PULSE_ADDRESS_EDITOR_SCALE", option, ARRAYSIZE(option)) &&
        wcscmp(option, L"150") == 0) state->scale = 1.5f;
    WNDCLASSW wc{};
    wc.lpfnWndProc = TestOwner;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseAddressEditorTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName,
        L"", WS_POPUP, capture ? 0 : -30000, capture ? 0 : -30000, 1000, 700,
        nullptr, nullptr, wc.hInstance, nullptr);
    state->hwnd = hwnd;
    if (hwnd && state->compositor.Init(hwnd)) {
        state->compositor.RecreateTextFormats(state->scale);
        state->renderer.SetCompositor(&state->compositor);
        state->renderer.SetScale(state->scale);
        state->window_tabs.NewTab(L"C:\\pulse-address-editor-fixture");
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = ActiveTab(*state);
        tab->loading = false;
        tab->search_input_path = tab->current_path;
        tab->search_input_text = L"setup-dia";
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (capture) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowAddressSearch(*state);
        Pump();
        check(state->addressSearching && GetFocus() == state->hwndAddressEdit,
            "production search editor opens with focus");
        const auto inspect = [&](const wchar_t* name, bool selected) {
            if (!capture) return;
            EditorPixels pixels;
            check(CaptureEditor(state->hwndAddressEdit, (output / name).c_str(), nullptr,
                &pixels, state->darkMode) && pixels.glyphs > 15 && (!selected || pixels.selection > 20),
                "screen shows search characters and any requested selection");
        };
        inspect(L"initial.bmp", true);
        for (wchar_t ch : std::wstring(L"setup-dia")) SendMessageW(state->hwndAddressEdit, WM_CHAR, ch, 0);
        wchar_t text[128]{};
        GetWindowTextW(state->hwndAddressEdit, text, ARRAYSIZE(text));
        check(wcscmp(text, L"setup-dia") == 0, "continuous typing replaces the selected query");
        inspect(L"typed.bmp", false);
        LayoutAddressEditor(*state);
        Pump();
        inspect(L"layout.bmp", false);

        struct HistoryCheck { AppState* state; bool capture; std::filesystem::path output; bool ran = false; bool passed = false; int phase = 0; };
        static HistoryCheck* history = nullptr;
        HistoryCheck history_check{state.get(), capture, output};
        history = &history_check;
        const UINT_PTR timer = SetTimer(nullptr, 0, 80, [](HWND, UINT, UINT_PTR id, DWORD) {
            if (!history || !history->state->menu || !history->state->menu->IsOpen()) return;
            auto& s = *history->state;
            if (history->phase++ == 0) {
                SendMessageW(s.hwndAddressEdit, EM_SETSEL, 0, -1);
                for (wchar_t ch : std::wstring(L"setup-dia")) SendMessageW(s.hwndAddressEdit, WM_CHAR, ch, 0);
                return;
            }
            KillTimer(nullptr, id);
            history->ran = true;
            bool valid = GetFocus() == s.hwndAddressEdit;
            wchar_t text[64]{};
            GetWindowTextW(s.hwndAddressEdit, text, ARRAYSIZE(text));
            valid &= wcscmp(text, L"setup-dia") == 0 && GetFocus() == s.hwndAddressEdit;
            if (history->capture) {
                EditorPixels pixels;
                valid &= CaptureEditor(s.hwndAddressEdit, (history->output / L"history.bmp").c_str(),
                    nullptr, &pixels, s.darkMode) && pixels.glyphs > 15;
                valid &= CaptureEditor(s.hwnd, (history->output / L"history-owner.bmp").c_str());
            }
            history->passed = valid;
            s.menu->Dismiss();
        });
        if (timer) ShowAddressSearchHistory(*state);
        KillTimer(nullptr, timer);
        history = nullptr;
        check(history_check.ran && history_check.passed,
            "history popup keeps continuously typed search text visible and focused");
        state->addressLiveDue = state->addressHistoryDue = 0;
        SendMessageW(state->hwndAddressEdit, EM_SETSEL, 0, -1);
        SendMessageW(state->hwndAddressEdit, WM_IME_STARTCOMPOSITION, 0, 0);
        for (wchar_t ch : std::wstring(L"中文搜索")) SendMessageW(state->hwndAddressEdit, WM_CHAR, ch, 0);
        SendMessageW(state->hwndAddressEdit, WM_IME_ENDCOMPOSITION, 0, 0);
        GetWindowTextW(state->hwndAddressEdit, text, ARRAYSIZE(text));
        check(wcscmp(text, L"中文搜索") == 0 && !state->addressSearchComposing,
            "Unicode query remains editable through composition messages");
        inspect(L"unicode.bmp", false);
        state->addressLiveDue = state->addressHistoryDue = 0;
        HideAddressEditor(*state, false);
        ShowAddressSearch(*state);
        Pump();
        inspect(L"reopened.bmp", true);
        HideAddressEditor(*state, false);
        ShowAddressEditor(*state);
        Pump();
        inspect(L"address.bmp", true);
        HideAddressEditor(*state, false);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    } else check(false, "search owner and graphics initialize");
    if (hwnd) DestroyWindow(hwnd);
    state->hwndAddressEdit = nullptr;
    state->hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (log) fclose(log);
    return ok;
}

bool RunRenameEditorTest() {
    std::filesystem::create_directories(L"bench_data/rename-editor");
    FILE* log = _fsopen("bench_data/rename-editor/results.log", "wN", _SH_DENYNO);
    bool ok = log != nullptr;
    auto check = [&](bool value, const char* label) {
        if (log) { fprintf(log, "[%s] %s\n", value ? "PASS" : "FAIL", label); fflush(log); }
        ok &= value;
        return value;
    };
    const auto fixture = std::filesystem::absolute(std::filesystem::path(L"bench_data/rename-editor") /
        (L"fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    if (!check(std::filesystem::create_directory(fixture), "isolated fixture created")) return false;
    const auto source = fixture / L"before.txt";
    const auto target = fixture / L"after.txt";
    HANDLE file = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!check(file != INVALID_HANDLE_VALUE, "real source created")) return false;
    CloseHandle(file);
    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->isolatedTest = true;
    wchar_t appearance[8]{};
    if (GetEnvironmentVariableW(L"PULSE_RENAME_EDITOR_DARK", appearance, ARRAYSIZE(appearance)))
        state->darkMode = appearance[0] == L'1';
    if (GetEnvironmentVariableW(L"PULSE_RENAME_EDITOR_SCALE", appearance, ARRAYSIZE(appearance)) &&
        wcscmp(appearance, L"150") == 0) state->scale = 1.5f;
    WNDCLASSW wc{};
    wc.lpfnWndProc = TestOwner;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseRenameEditorTest";
    RegisterClassW(&wc);
    wchar_t capture[8]{};
    const bool capture_editor = GetEnvironmentVariableW(L"PULSE_RENAME_EDITOR_CAPTURE", capture, ARRAYSIZE(capture)) > 0;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"", WS_POPUP,
        capture_editor ? 0 : -30000, capture_editor ? 0 : -30000, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    state->hwnd = hwnd;
    if (check(hwnd && state->compositor.Init(hwnd), "offscreen owner and graphics initialized")) {
        state->compositor.RecreateTextFormats(state->scale);
        state->renderer.SetCompositor(&state->compositor);
        state->renderer.SetScale(state->scale);
        state->window_tabs.NewTab(fixture.wstring());
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        const auto original_path = tab->current_path;
        fs::DirEntry entry; entry.name = L"before.txt";
        tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(1, entry));
        tab->loading = false;
        tab->selected_index = 0;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (capture_editor) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        state->ops.Start([] {});
        ShowRenameOverlay(*state);
        check(state->renameIndex == 0 && IsWindowVisible(state->hwndRenameEdit), "production rename editor opens");
        wchar_t initial_name[64]{};
        GetWindowTextW(state->hwndRenameEdit, initial_name, ARRAYSIZE(initial_name));
        check(wcscmp(initial_name, L"before.txt") == 0, "rename opens with the complete original filename");
        DWORD selection_start = 0, selection_end = 0;
        SendMessageW(state->hwndRenameEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start),
            reinterpret_cast<LPARAM>(&selection_end));
        check(selection_start == 0 && selection_end == 6, "rename initially selects the stem and preserves the extension");
        RECT editor_rect{};
        GetWindowRect(state->hwndRenameEdit, &editor_rect);
        if (log) fprintf(log, "editor rectangle: %ld,%ld - %ld,%ld\n", editor_rect.left, editor_rect.top, editor_rect.right, editor_rect.bottom);
        check(editor_rect.right > editor_rect.left && editor_rect.bottom > editor_rect.top,
            "rename editor has a nonempty visible text area");
        if (capture_editor) {
            Pump();
            EditorPixels sampled;
            check(CaptureEditor(state->hwndRenameEdit, L"bench_data/rename-editor/initial.bmp", nullptr,
                &sampled, state->darkMode), "capture initial editor appearance");
            check(sampled.glyphs > 15 && sampled.selection > 20,
                "screen pixels contain both filename glyphs and the initial selection");
            check(CaptureEditor(hwnd, L"bench_data/rename-editor/owner.bmp"), "capture rename editor in the list");
            SendMessageW(state->hwndRenameEdit, WM_TIMER, 71, 0);
            check(CaptureEditor(state->hwndRenameEdit, L"bench_data/rename-editor/repaint.bmp"), "capture editor after caret repaint");
            LayoutRenameOverlay(*state);
            check(CaptureEditor(state->hwndRenameEdit, L"bench_data/rename-editor/relayout.bmp", nullptr,
                &sampled, state->darkMode), "capture editor after repeated layout");
            check(sampled.glyphs > 15 && sampled.selection > 20,
                "filename and selection remain visible after repaint and layout");
            if (state->compositor.LumaTextEnabled())
                check(CaptureEditor(state->hwndRenameEdit, L"bench_data/rename-editor/rendered.bmp", state.get()), "capture rendered editor bitmap");
        }
        GUITHREADINFO gui{sizeof(gui)};
        GetGUIThreadInfo(GetCurrentThreadId(), &gui);
        if (!state->compositor.LumaTextEnabled()) {
            check(gui.hwndFocus == state->hwndRenameEdit && gui.hwndCaret == state->hwndRenameEdit &&
                (gui.flags & GUI_CARETBLINKING), "native fallback keeps its caret visible");
        } else check(gui.hwndFocus == state->hwndRenameEdit, "LumaText editor receives focus");
        const LPARAM mouse_point = MAKELPARAM(4, 10);
        SendMessageW(state->hwndRenameEdit, WM_LBUTTONDBLCLK, MK_LBUTTON, mouse_point);
        SendMessageW(state->hwndRenameEdit, WM_LBUTTONUP, 0, mouse_point);
        SendMessageW(state->hwndRenameEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start),
            reinterpret_cast<LPARAM>(&selection_end));
        check(selection_end > selection_start && GetCapture() != state->hwndRenameEdit,
            "mouse selection remains editable and releases capture");
        if (capture_editor && state->compositor.LumaTextEnabled()) {
            EditorPixels sampled;
            check(CaptureEditor(state->hwndRenameEdit, L"bench_data/rename-editor/mouse.bmp", nullptr,
                &sampled, state->darkMode) && sampled.glyphs > 15 && sampled.selection > 20,
                "mouse selection preserves the rendered filename on screen");
        }
        SendMessageW(state->hwndRenameEdit, EM_SETSEL, 0, 6);
        if (state->compositor.LumaTextEnabled()) {
            const auto draws = state->compositor.GetLumaTextStats()->draw_calls;
            SendMessageW(state->hwndRenameEdit, EM_SETSEL, 0, 6);
            check(state->compositor.GetLumaTextStats()->draw_calls > draws,
                "setting the initial selection immediately repaints the LumaText surface");
        }
        SetWindowTextW(state->hwndRenameEdit, L"after.txt");
        const auto list = ListRect(*state);
        const LPARAM point = MAKELPARAM(static_cast<int>(list.left + 30), static_cast<int>(list.bottom - 30));
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
        check(state->renameIndex == -1 && !IsWindowVisible(state->hwndRenameEdit), "blank click itself closes editor");
        const auto start = GetTickCount64();
        while (!std::filesystem::exists(target) && GetTickCount64() - start < 15000) { Pump(); Sleep(10); }
        check(std::filesystem::exists(target) && !std::filesystem::exists(source), "blank click itself commits real disk rename");
        if (log) fprintf(log, "disk rename observed after %llu ms\n", GetTickCount64() - start);
        check(tab->current_path == original_path, "blank click preserves directory");
        const auto stop_start = GetTickCount64();
        state->ops.Stop();
        check(GetTickCount64() - stop_start < 5000, "immediate shutdown after fast rename does not hang");

        // Simulate a stale edit window after its model has already ended editing.
        ShowWindow(state->hwndRenameEdit, SW_SHOWNOACTIVATE);
        state->renameIndex = -1;
        HideRenameOverlay(*state, false);
        check(!IsWindowVisible(state->hwndRenameEdit), "orphaned rename editor is hidden");
        ShowWindow(state->hwndRenameEdit, SW_HIDE);

        ShowAddressEditor(*state);
        ShowAddressEditor(*state);
        check(state->addressEditing && !state->addressIgnoreKillFocus && IsWindowVisible(state->hwndAddressEdit),
            "focused address editor survives synchronous teardown and reopen");
        HideAddressEditor(*state, false);

        entry.name = L"after.txt";
        tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(1, entry));
        tab->selected_index = 0;
        ShowRenameOverlay(*state);
        ShowRenameOverlay(*state);
        wchar_t name[64]{};
        GetWindowTextW(state->hwndRenameEdit, name, ARRAYSIZE(name));
        check(state->renameIndex == 0 && !state->renameIgnoreKillFocus && wcscmp(name, L"after.txt") == 0 &&
            IsWindowVisible(state->hwndRenameEdit), "focused rename editor survives teardown and reopen");
        HideRenameOverlay(*state, false);

        struct NameCase { const wchar_t* name; bool directory; DWORD selection_end; };
        int capture_index = 0;
        for (const auto& item : { NameCase{L"报告.txt", false, 2},
                                 NameCase{L"README", false, 6},
                                 NameCase{L"folder.name", true, 11},
                                 NameCase{L"Microsoft.Services.Store.winmd", false, 24} }) {
            entry.name = item.name;
            entry.is_dir = item.directory;
            tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(1, entry));
            tab->selected_index = 0;
            ShowRenameOverlay(*state);
            GetWindowTextW(state->hwndRenameEdit, name, ARRAYSIZE(name));
            SendMessageW(state->hwndRenameEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start),
                reinterpret_cast<LPARAM>(&selection_end));
            check(wcscmp(name, item.name) == 0 && selection_start == 0 && selection_end == item.selection_end,
                "filename is preserved with the expected initial selection");
            if (capture_editor) {
                Pump();
                EditorPixels sampled;
                const auto path = L"bench_data/rename-editor/name-" + std::to_wstring(capture_index) + L".bmp";
                check(CaptureEditor(state->hwndRenameEdit, path.c_str(), nullptr, &sampled, state->darkMode) &&
                    sampled.glyphs > 15 && sampled.selection > 20, "filename and selection are visible on screen");
            }
            SendMessageW(state->hwndRenameEdit, WM_CHAR, L'X', 1);
            GetWindowTextW(state->hwndRenameEdit, name, ARRAYSIZE(name));
            const std::wstring expected = L"X" + entry.name.substr(item.selection_end);
            check(name == expected,
                "typing replaces the initial selection and preserves any file extension");
            if (capture_editor) {
                EditorPixels sampled;
                const auto path = L"bench_data/rename-editor/typed-" + std::to_wstring(capture_index) + L".bmp";
                check(CaptureEditor(state->hwndRenameEdit, path.c_str(), nullptr, &sampled, state->darkMode) &&
                    sampled.glyphs > 15, "typed replacement is visible immediately");
            }
            HideRenameOverlay(*state, false);
            ++capture_index;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    if (hwnd) DestroyWindow(hwnd);
    state->hwndRenameEdit = nullptr;
    state->hwndAddressEdit = nullptr;
    state->hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::filesystem::remove(source);
    std::filesystem::remove(target);
    check(std::filesystem::remove(fixture), "isolated fixture cleaned");
    if (log) fclose(log);
    return ok;
}
}
#endif
