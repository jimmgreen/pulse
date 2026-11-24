#include "../app/app_input.h"
#include "../ui/ui_renderer.h"
#include <cstdio>

using namespace pulse;
namespace {
int failures = 0;
void Check(bool passed, const wchar_t* label) {
    std::wprintf(L"[%ls] %ls\n", passed ? L"PASS" : L"FAIL", label);
    failures += !passed;
}
LRESULT CALLBACK BlankPaneTestProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state) {
        switch (msg) {
        case WM_LBUTTONDOWN: return HandleLButtonDown(state, hwnd, msg, wparam, lparam);
        case WM_MOUSEMOVE: return HandleMouseMove(state, hwnd, msg, wparam, lparam);
        case WM_LBUTTONUP: return HandleLButtonUp(state, hwnd, msg, wparam, lparam);
        case WM_LBUTTONDBLCLK: return HandleLButtonDblClk(state, hwnd, msg, wparam, lparam);
        case WM_CAPTURECHANGED: return HandleCaptureChanged(state, hwnd, msg, wparam, lparam);
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void TestSelectedItemDragSurface() {
    auto state = std::make_unique<AppState>();
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->appPrefs.single_click_open = false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = BlankPaneTestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseSelectedDragSelftest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP,
        0, 0, 1000, 700, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, L"selected drag: hidden event window created");
    if (!hwnd) return;
    state->hwnd = hwnd;
    const bool graphics = state->compositor.Init(hwnd);
    Check(graphics, L"selected drag: real hit-test graphics initialized");
    if (graphics) {
        state->compositor.RecreateTextFormats(1.0f);
        state->renderer.SetCompositor(&state->compositor);
        state->renderer.SetScale(1.0f);
        state->window_tabs.NewTab(L"C:\\PulseSelectedDragFixture");
        state->pane = state->window_tabs.Active()->panes.front().get();
        auto* tab = state->pane->ActiveTab();
        auto entries = std::make_shared<std::vector<fs::DirEntry>>(2);
        (*entries)[0].name = L"a.txt";
        (*entries)[1].name = L"folder";
        (*entries)[1].is_dir = true;
        tab->SetSnapshot(entries);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state.get()));
        BYTE saved_keys[256]{};
        GetKeyboardState(saved_keys);
        BYTE keys[256]{};
        keys[VK_LBUTTON] = 0x80;
        SetKeyboardState(keys);
        for (const auto mode : {ui::ViewMode::Details, ui::ViewMode::MediumIcons}) {
            tab->view_mode = mode;
            for (const int source : {0, 1}) {
                tab->ClearSelection();
                auto vm = BuildVm(*state, false);
                const auto pane = FocusedPaneRect(*state);
                const auto cell = state->renderer.ItemRectInPane(vm.pane, pane, vm.pane.ViewIndex(source));
                POINT blank{-1, -1};
                for (int y = static_cast<int>(cell.top + 1); y < cell.bottom && blank.x < 0; ++y) {
                    for (int x = static_cast<int>(cell.right - 2); x > cell.left; --x) {
                        const auto hit = state->renderer.HitTest(vm, D2D1::RectF(0, 0, 1000, 700),
                            static_cast<float>(x), static_cast<float>(y));
                        if (hit.region == ui::HitTestResult::Row && hit.index == source &&
                            !state->renderer.PointInItemGrip(vm.pane, pane, source, static_cast<float>(x), static_cast<float>(y))) {
                            blank = {x, y};
                            break;
                        }
                    }
                }
                Check(blank.x >= 0, mode == ui::ViewMode::Details
                    ? L"selected drag: details blank column found" : L"selected drag: icon card whitespace found");
                if (blank.x < 0) continue;
                const auto point = MAKELPARAM(blank.x, blank.y);
                SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                Check(state->listSelection.PendingBand() && !state->listSelection.PendingFile(),
                    L"selected drag: unselected whitespace still arms marquee");
                SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
                for (const bool multi : {false, true}) {
                    tab->SelectOnly(source);
                    if (multi) tab->SelectAll();
                    SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    Check(state->listSelection.PendingFile() && !state->listSelection.PendingBand() &&
                        tab->SelectedCount() == (multi ? 2 : 1),
                        L"selected drag: selected whitespace arms file press without collapsing selection");
                    SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, point);
                    Check(state->listSelection.PendingFile() && !state->listSelection.BandActive(),
                        L"selected drag: no movement stays pending instead of starting drag");
                    SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(blank.x - 1, blank.y));
                    Check(state->listSelection.PendingFile() && tab->SelectedCount() == (multi ? 2 : 1),
                        L"selected drag: sub-threshold movement retains the entire drag selection");
                    SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
                    Check(!state->listSelection.PendingFile() && tab->SelectedCount() == 1 &&
                        !state->fileClick.RenamePending(),
                        L"selected drag: ordinary release collapses selection without whitespace rename");
                }
                tab->SelectAll();
                SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                ReleaseCapture();
                Check(!state->listSelection.PendingFile() && tab->SelectedCount() == 2,
                    L"selected drag: lost capture cancels pending drag without collapsing multi-selection");
                const auto list = ListRect(*state);
                const auto empty = MAKELPARAM(static_cast<int>(list.left + 30), static_cast<int>(list.bottom - 20));
                SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, empty);
                Check(state->listSelection.PendingBand() && !state->listSelection.PendingFile(),
                    L"selected drag: empty pane space remains marquee even with a selection");
                SendMessageW(hwnd, WM_LBUTTONUP, 0, empty);
            }
        }
        SetKeyboardState(saved_keys);
        state->renderer.SetCompositor(nullptr);
        state->compositor.Shutdown();
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    state->hwnd = nullptr;
    state->pane = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

}
int wmain() {
    const HRESULT ole = OleInitialize(nullptr);
    TestSelectedItemDragSurface();
    if (SUCCEEDED(ole)) OleUninitialize();
    return failures ? 1 : 0;
}
