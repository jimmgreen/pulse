#ifdef PULSE_WITH_SELFTEST
#include "scroll_hover_taskbar_test.h"
#include "app_hover.h"
#include "app_input.h"
#include "app_navigation.h"
#include "app_window_title.h"
#include "../common/localization.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <share.h>
#include <vector>

namespace pulse::app {
namespace {
POINT pointer_screen{};
HWND pointer_window = nullptr;
HWND captured_window = nullptr;
bool pointer_available = true;
int title_messages = 0;

BOOL WINAPI CursorPosition(LPPOINT point) {
    *point = pointer_screen;
    return pointer_available ? TRUE : FALSE;
}
HWND WINAPI WindowAtPoint(POINT) { return pointer_window; }
HWND WINAPI CaptureWindow() { return captured_window; }
LRESULT CALLBACK TestWindow(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETTEXT) ++title_messages;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
int HoverRow(const ui::HitTestResult& hit) {
    using R = ui::HitTestResult;
    return hit.region == R::Row || hit.region == R::RowStar || hit.region == R::RowFolderSize ||
        hit.region == R::RowNewTab || hit.region == R::ChangeBadge || hit.region == R::RowMore
        ? hit.index : -1;
}
std::wstring Caption(HWND hwnd) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<size_t>(copied));
    return text;
}
}

bool RunScrollHoverTaskbarTest() {
    const auto output = std::filesystem::absolute(L"bench_data") /
        (L"scroll-hover-taskbar-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(output);
    FILE* log = _wfsopen((output / L"results.log").c_str(), L"wN", _SH_DENYNO);
    bool ok = log != nullptr;
    int passed = 0, failed = 0;
    const auto check = [&](bool value, const char* label) {
        if (log) { fprintf(log, "[%s] %s\n", value ? "PASS" : "FAIL", label); fflush(log); }
        if (value) ++passed; else ++failed;
        ok &= value;
    };
    check(TaskbarWindowTitle(nullptr) == L"Pulse", "no active folder keeps the Pulse fallback");
    Tab title_tab;
    title_tab.current_path = L"C:\\fixture\\中文 项目\\";
    check(TaskbarWindowTitle(&title_tab) == L"中文 项目 — Pulse",
        "taskbar title uses the Unicode folder leaf, including spaces and trailing slash");
    title_tab.current_path = L"C:\\";
    check(TaskbarWindowTitle(&title_tab) == L"C: — Pulse", "drive roots retain their display name");
    title_tab.current_path = L"\\\\server\\share\\";
    check(TaskbarWindowTitle(&title_tab) == L"share — Pulse", "UNC share naming requires no network access");
    title_tab.current_path.clear();
    check(TaskbarWindowTitle(&title_tab) == l10n::Get(l10n::StringId::ThisPc) + L" — Pulse",
        "This PC uses the existing localized name");
    title_tab.virtual_title = L"搜索：示例";
    check(TaskbarWindowTitle(&title_tab) == L"搜索：示例 — Pulse",
        "virtual folders and search results use their display title");

    WNDCLASSW wc{};
    wc.lpfnWndProc = TestWindow;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseScrollHoverTaskbarTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        wc.lpszClassName, L"Pulse", WS_POPUP, -30000, -30000, 1100, 800,
        nullptr, nullptr, wc.hInstance, nullptr);
    auto state = std::make_unique<AppState>();
    state->hwnd = hwnd;
    state->isolatedTest = true;
    state->places.persist = state->appPrefs.persist = state->searchHistory.persist = false;
    state->ctxMenuPrefs.persist = false;
    state->appPrefs.show_hints = false;
    check(hwnd && state->compositor.Init(hwnd), "isolated hidden Win32 window and real compositor initialize");
    if (!ok || !state->compositor.Dc()) {
        if (hwnd) DestroyWindow(hwnd);
        if (log) fclose(log);
        return false;
    }
    state->renderer.SetCompositor(&state->compositor);
    const auto alpha = output / L"Alpha 目录";
    const auto beta = output / L"Beta 项目";
    std::filesystem::create_directory(alpha);
    std::filesystem::create_directory(beta);
    state->window_tabs.NewTab(alpha.wstring());
    BindCurrentLayout(*state);
    auto* tab = ActiveTab(*state);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 300; ++i) {
        fs::DirEntry entry;
        entry.name = L"entry-" + std::to_wstring(1000 + i) + L".txt";
        entries->push_back(std::move(entry));
    }
    tab->SetSnapshot(entries);
    tab->loading = false;
    tab->SelectOnly(0);
    pointer_window = hwnd;
    captured_window = nullptr;
    pointer_available = true;
    const HoverPointerApi api{CursorPosition, WindowAtPoint, ScreenToClient, CaptureWindow};
    const HWND original_focus = GetFocus();
    const auto original_selection = tab->selected;
    const auto original_revision = tab->selection_revision;

    for (float scale : {1.0f, 1.5f}) {
        state->scale = scale;
        state->renderer.SetScale(scale);
        state->compositor.RecreateTextFormats(scale);
        for (ui::ViewMode mode : {ui::ViewMode::Details, ui::ViewMode::List}) {
            tab->view_mode = mode;
            tab->scroll_x = tab->scroll_y = 0;
            state->scrollAnimating = false;
            const auto vm = BuildVm(*state, false);
            const auto pane = FocusedPaneRect(*state);
            const auto row = state->renderer.ItemRectInPane(vm.pane, pane, 2);
            POINT fixed_point{static_cast<LONG>(row.left + 70 * scale),
                static_cast<LONG>((row.top + row.bottom) * 0.5f)};
            pointer_screen = fixed_point;
            ClientToScreen(hwnd, &pointer_screen);
            RefreshScrolledHover(*state, api);
            const int before = state->hoverRow;
            check(before >= 0, "stationary pointer initially hits a real file in the rendered layout");
            const bool horizontal = mode == ui::ViewMode::List;
            const float delta = horizontal ? 500.0f * scale : 180.0f * scale;
            StartSmoothScroll(*state, delta, horizontal);
            bool every_frame_correct = true;
            bool actual_not_target = false;
            for (int frame = 0; frame < 90 && state->scrollAnimating; ++frame) {
                state->scrollLastUpdateTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(16);
                UpdateSmoothScroll(*state, api);
                auto current = BuildVm(*state, false);
                const auto bounds = D2D1::RectF(0, 0,
                    static_cast<float>(state->compositor.Width()),
                    static_cast<float>(state->compositor.Height()));
                const auto hit = state->renderer.HitTest(current, bounds,
                    static_cast<float>(fixed_point.x), static_cast<float>(fixed_point.y));
                every_frame_correct &= state->hoverRow == HoverRow(hit) &&
                    state->hoverPaneIndex == hit.pane_index &&
                    state->hoverPoint.x == fixed_point.x && state->hoverPoint.y == fixed_point.y;
                if (frame == 0) {
                    if (horizontal) current.pane.scroll_x = state->scrollTargetX;
                    else current.pane.scroll_y = state->scrollTargetY;
                    for (auto& slot : current.pane_slots) {
                        if (horizontal) slot.pane.scroll_x = state->scrollTargetX;
                        else slot.pane.scroll_y = state->scrollTargetY;
                    }
                    const auto final_hit = state->renderer.HitTest(current, bounds,
                        static_cast<float>(fixed_point.x), static_cast<float>(fixed_point.y));
                    actual_not_target = HoverRow(final_hit) != state->hoverRow;
                }
            }
            check(every_frame_correct && !state->scrollAnimating && state->hoverRow != before,
                horizontal ? "horizontal list scrolling refreshes hover every frame and after settling" :
                             "details scrolling refreshes hover every frame and after settling");
            check(actual_not_target, "hover follows the displayed animation position, not its future target");
            StartSmoothScroll(*state, -delta, horizontal);
            for (int frame = 0; frame < 90 && state->scrollAnimating; ++frame) {
                state->scrollLastUpdateTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(16);
                UpdateSmoothScroll(*state, api);
            }
            check(!state->scrollAnimating && state->hoverRow == before,
                "reverse scrolling restores hover without a mouse-move message");
        }
    }
    check(tab->selected == original_selection && tab->selection_revision == original_revision &&
        tab->selected_index == 0 && GetFocus() == original_focus,
        "scroll hover never changes selection, revision or keyboard focus");

    const int hover_before_capture = state->hoverRow;
    captured_window = hwnd;
    RefreshScrolledHover(*state, api);
    check(state->hoverRow == hover_before_capture, "captured drag gestures are not re-entered by scroll hover");
    captured_window = nullptr;
    pointer_window = nullptr;
    RefreshScrolledHover(*state, api);
    check(state->hoverRow == -1 && state->hoverRegion == ui::HitTestResult::None,
        "an occluding window clears file hover rather than highlighting behind it");
    pointer_window = hwnd;
    pointer_available = false;
    RefreshScrolledHover(*state, api);
    check(state->hoverRow == -1, "unavailable pointer cannot create a phantom hovered row");
    pointer_available = true;
    pointer_screen = POINT{-31000, -31000};
    RefreshScrolledHover(*state, api);
    check(state->hoverRow == -1, "pointer outside the client cannot retain stale file hover");

    SyncTaskbarWindowTitle(*state);
    check(Caption(hwnd) == L"Alpha 目录 — Pulse", "native HWND caption contains the active folder name");
    const int stable_title_count = title_messages;
    SyncTaskbarWindowTitle(*state);
    BuildVm(*state, false);
    check(title_messages == stable_title_count, "unchanged frames do not resend WM_SETTEXT");
    const size_t alpha_index = state->window_tabs.active;
    state->window_tabs.NewTab(beta.wstring());
    BindCurrentLayout(*state);
    check(Caption(hwnd) == L"Beta 项目 — Pulse", "binding another active tab updates the native taskbar caption");
    SwitchTab(*state, alpha_index);
    check(Caption(hwnd) == L"Alpha 目录 — Pulse", "switching back restores the previous folder caption");
    auto* layout = state->window_tabs.Active();
    auto second = std::make_unique<Pane>();
    second->NewTab(beta.wstring());
    auto* second_pane = second.get();
    layout->panes.push_back(std::move(second));
    FocusPane(*state, second_pane);
    check(Caption(hwnd) == L"Beta 项目 — Pulse", "changing the focused pane updates caption without selecting a file");
    auto* active = ActiveTab(*state);
    active->virtual_title = L"搜索结果：测试";
    BuildVm(*state, false);
    check(Caption(hwnd) == L"搜索结果：测试 — Pulse", "asynchronous display-title changes are synchronized on model rebuild");
    state->pane = nullptr;
    SyncTaskbarWindowTitle(*state);
    check(Caption(hwnd) == L"Pulse", "no active pane resets a stale folder caption safely");

    state->hwnd = nullptr;
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (log) {
        fprintf(log, "\n%d passed, %d failed\n", passed, failed);
        fclose(log);
    }
    std::wprintf(L"Scroll hover/taskbar results: %s\n", (output / L"results.log").c_str());
    return ok;
}
} // namespace pulse::app
#endif
