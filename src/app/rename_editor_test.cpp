#ifdef PULSE_WITH_SELFTEST
#include "rename_editor_test.h"
#include "app_hosted_edit.h"
#include "app_input.h"
#include <cstdio>
#include <filesystem>
#include <share.h>

namespace pulse::app {
namespace {
LRESULT CALLBACK TestOwner(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
        if (msg == WM_LBUTTONDOWN) return HandleLButtonDown(state, hwnd, msg, wp, lp);
        if (msg == WM_LBUTTONUP) return HandleLButtonUp(state, hwnd, msg, wp, lp);
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
    WNDCLASSW wc{};
    wc.lpfnWndProc = TestOwner;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseRenameEditorTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP,
        -30000, -30000, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    state->hwnd = hwnd;
    if (check(hwnd && state->compositor.Init(hwnd), "offscreen owner and graphics initialized")) {
        state->compositor.RecreateTextFormats(1.0f);
        state->renderer.SetCompositor(&state->compositor);
        state->window_tabs.NewTab(fixture.wstring());
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        const auto original_path = tab->current_path;
        fs::DirEntry entry; entry.name = L"before.txt";
        tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(1, entry));
        tab->selected_index = 0;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        state->ops.Start([] {});
        ShowRenameOverlay(*state);
        check(state->renameIndex == 0 && IsWindowVisible(state->hwndRenameEdit), "production rename editor opens");
        GUITHREADINFO gui{sizeof(gui)};
        GetGUIThreadInfo(GetCurrentThreadId(), &gui);
        if (!state->compositor.LumaTextEnabled()) {
            check(gui.hwndFocus == state->hwndRenameEdit && gui.hwndCaret == state->hwndRenameEdit &&
                (gui.flags & GUI_CARETBLINKING), "native fallback keeps its caret visible");
        } else check(gui.hwndFocus == state->hwndRenameEdit, "LumaText editor receives focus");
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
