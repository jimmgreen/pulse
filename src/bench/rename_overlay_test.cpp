#include "../app/app_internal.h"
#include "../app/search_query.h"
#include <iostream>
#include <cmath>
#include <utility>
#include <wrl/client.h>

int main() {
    using namespace pulse;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!ok) ++failures;
    };
    auto owned = std::make_unique<AppState>(); auto& s = *owned;
    s.appPrefs.persist = false;
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseRenameFixture"; RegisterClassW(&wc);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Pulse rename verification", WS_OVERLAPPEDWINDOW,
        100, 100, 1200, 760, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) return 2;
    s.compositor.Resize(1180, 720); s.renderer.SetCompositor(&s.compositor);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (const auto* name : {L"中文合同.docx", L"example.cpp", L"新建 Microsoft Excel 工作表.xlsx", L"notes.txt"}) {
        fs::DirEntry row; row.name = name; row.full_path = L"C:\\fixture\\" + row.name;
        entries->push_back(std::move(row));
    }
    tab.SetSnapshot(entries);
    tab.search_snippets = std::make_shared<std::vector<std::wstring>>(4, L"L1  合同内容匹配");
    auto pump = [] { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } };
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    for (const float scale : {1.0f, 1.5f}) for (bool search : {false, true}) {
        s.scale = scale; s.darkMode = search;
        s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
        if (s.editFont) { DeleteObject(s.editFont); s.editFont = nullptr; }
        if (s.editBrush) { DeleteObject(s.editBrush); s.editBrush = nullptr; }
        tab.current_path = search ? app::MakeSearchPath(L"content:合同") : L"C:\\fixture";
        tab.banner_message = search ? L"Content indexing is up to date" : L"";
        tab.SelectOnly(2); ShowRenameOverlay(s);
        auto vm = BuildVm(s, false);
        for (const auto& slot : vm.pane_slots) if (slot.focused) {
            const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
            const auto frame = s.renderer.RenameFieldRect(slot.pane, list, s.renameIndex);
            RECT edit{}; GetWindowRect(s.hwndRenameEdit, &edit);
            MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&edit), 2);
            check(edit.left >= frame.left && edit.right <= frame.right + 1 && edit.top >= frame.top && edit.bottom <= frame.bottom + 1,
                search ? "search rename text is inside the shared frame" : "folder rename text is inside the shared frame");
            const auto row = s.renderer.ItemRectInPane(slot.pane, slot.rect, slot.pane.ViewIndex(s.renameIndex));
            check(frame.top >= row.top && frame.bottom <= row.bottom + 1, "rename frame stays in the selected row");
            const auto hit = s.renderer.HitTest(vm, D2D1::RectF(0, 0, 1180, 720), frame.left + 4, (frame.top + frame.bottom) / 2);
            check(hit.index == 2, "rendered edit position hits the same file row");
        }
        DWORD start = 0, end = 0; SendMessageW(s.hwndRenameEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        check(start == 0 && end == entries->at(2).name.find_last_of(L'.'), "shared rename selects the stem and preserves the extension");
        const auto theme = ui::MakeTheme(s.darkMode, ui::HexColor(0x0078D4));
        s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm, D2D1::RectF(0, 0, 1180, 720), theme);
        s.compositor.Dc()->EndDraw(); s.compositor.Present();
        InvalidateRect(s.hwndRenameEdit, nullptr, TRUE); UpdateWindow(s.hwndRenameEdit); pump();
        if (scale == 1.5f && search && GetEnvironmentVariableW(L"PULSE_RENAME_SHOW", nullptr, 0)) {
            const auto until = GetTickCount64() + 120000;
            while (GetTickCount64() < until) { pump(); Sleep(20); }
        }
        SendMessageW(s.hwndRenameEdit, WM_KEYDOWN, VK_ESCAPE, 0);
        check(s.renameIndex == -1 && !IsWindowVisible(s.hwndRenameEdit), "Escape uses the existing cancel path");
    }
    // Mouse selection in the LumaText rename editor (pointer drag flicker and
    // imprecise double-click words).
    {
        s.scale = 1.0f; s.darkMode = false;
        s.compositor.RecreateTextFormats(1.0f); s.renderer.SetScale(1.0f);
        tab.current_path = L"C:\\fixture"; tab.banner_message.clear();
        auto mouse_entries = std::make_shared<std::vector<fs::DirEntry>>();
        for (const auto* name : {L"IMG_2024-final report.docx", L"中文合同.docx",
                                 L"一份很长的文件名_用来验证拖动选择时文字不会在指针下来回滑动_report-2026-final-version-"
                                 L"with-a-deliberately-long-suffix-so-the-editor-must-scroll-horizontally.txt"}) {
            fs::DirEntry row; row.name = name; row.full_path = L"C:\\fixture\\" + row.name;
            mouse_entries->push_back(std::move(row));
        }
        tab.SetSnapshot(mouse_entries);
        auto selection = [&] {
            DWORD a = 0, b = 0;
            SendMessageW(s.hwndRenameEdit, EM_GETSEL, reinterpret_cast<WPARAM>(&a), reinterpret_cast<LPARAM>(&b));
            return std::pair<int, int>{static_cast<int>(a), static_cast<int>(b)};
        };
        // Client x of a point 3/4 into character `index` (its right half), with
        // the editor unscrolled and LumaText's 2px text padding.
        auto char_x = [&](const std::wstring& text, int index, float fraction = 0.75f) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            RECT rc{}; GetClientRect(s.hwndRenameEdit, &rc);
            s.compositor.DwriteFactory()->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                HostedEditFormat(s, s.hwndRenameEdit), 1.0e6f, static_cast<float>(rc.bottom), &layout);
            float x = 0, y = 0; DWRITE_HIT_TEST_METRICS hit{};
            if (layout) layout->HitTestTextPosition(static_cast<UINT32>(index), FALSE, &x, &y, &hit);
            return static_cast<int>(std::lround(2.0f + hit.left + hit.width * fraction));
        };
        auto double_click = [&](int row, int index) {
            tab.SelectOnly(row); ShowRenameOverlay(s);
            RECT rc{}; GetClientRect(s.hwndRenameEdit, &rc);
            const LPARAM point = MAKELPARAM(char_x(mouse_entries->at(row).name, index), rc.bottom / 2);
            SendMessageW(s.hwndRenameEdit, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
            SendMessageW(s.hwndRenameEdit, WM_LBUTTONUP, 0, point);
            const auto range = selection();
            SendMessageW(s.hwndRenameEdit, WM_KEYDOWN, VK_ESCAPE, 0);
            return range;
        };
        check(double_click(0, 13) == std::pair<int, int>{9, 14},
            "double-click on the right half of a letter selects that punctuation-delimited word");
        check(double_click(0, 1) == std::pair<int, int>{0, 8}, "double-click keeps underscore names together");
        check(double_click(0, 23) == std::pair<int, int>{22, 26}, "double-click on the extension selects only the extension");
        check(double_click(1, 1) == std::pair<int, int>{0, 4}, "double-click on CJK text stops at the extension dot");
        // The button is still held after WM_LBUTTONDBLCLK; any move before the
        // release used to extend from the word start to the pointer ("wit").
        auto double_click_drag = [&](int row, int index, int move_index, float fraction) {
            tab.SelectOnly(row); ShowRenameOverlay(s);
            RECT rc{}; GetClientRect(s.hwndRenameEdit, &rc);
            const auto& name = mouse_entries->at(row).name;
            const LPARAM point = MAKELPARAM(char_x(name, index), rc.bottom / 2);
            const LPARAM moved = MAKELPARAM(char_x(name, move_index, fraction), rc.bottom / 2);
            SendMessageW(s.hwndRenameEdit, WM_LBUTTONDBLCLK, MK_LBUTTON, point);
            SendMessageW(s.hwndRenameEdit, WM_MOUSEMOVE, MK_LBUTTON, moved);
            SendMessageW(s.hwndRenameEdit, WM_LBUTTONUP, 0, moved);
            const auto range = selection();
            SendMessageW(s.hwndRenameEdit, WM_KEYDOWN, VK_ESCAPE, 0);
            return range;
        };
        check(double_click_drag(0, 13, 12, 0.3f) == std::pair<int, int>{9, 14},
            "a pointer that shifts after a double-click keeps the whole word selected");
        check(double_click_drag(0, 13, 23, 0.5f) == std::pair<int, int>{9, 26},
            "dragging right after a double-click extends by whole words");
        check(double_click_drag(0, 13, 1, 0.5f) == std::pair<int, int>{0, 14},
            "dragging left after a double-click extends by whole words");

        struct Redraws { int hidden = 0; };
        const auto observe = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT {
            if (msg == WM_SETREDRAW && !wp) ++reinterpret_cast<Redraws*>(data)->hidden;
            return DefSubclassProc(hwnd, msg, wp, lp);
        };
        tab.SelectOnly(2); ShowRenameOverlay(s);
        RECT rc{}; GetClientRect(s.hwndRenameEdit, &rc);
        const int y = rc.bottom / 2;
        Redraws redraws;
        SetWindowSubclass(s.hwndRenameEdit, observe, 97, reinterpret_cast<DWORD_PTR>(&redraws));
        int moves_hidden = 0;
        // Drag past the right edge so the editor scrolls, then hold the pointer
        // inside the field. Repeating the same move must not slide the text.
        auto drag = [&](int inside_moves) {
            SendMessageW(s.hwndRenameEdit, EM_SETSEL, 0, 0);
            SendMessageW(s.hwndRenameEdit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, y));
            redraws.hidden = 0;
            for (int i = 0; i < 5; ++i)
                SendMessageW(s.hwndRenameEdit, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(rc.right + 40, y));
            for (int i = 0; i < inside_moves; ++i)
                SendMessageW(s.hwndRenameEdit, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(rc.right - 40, y));
            moves_hidden += redraws.hidden;
            SendMessageW(s.hwndRenameEdit, WM_LBUTTONUP, 0, MAKELPARAM(rc.right - 40, y));
            return selection();
        };
        const auto once = drag(1);
        const auto held = drag(6);
        std::cout << "drag once=" << once.first << "," << once.second << " held=" << held.first << "," << held.second
                  << " hidden_during_moves=" << moves_hidden << std::endl;
        check(moves_hidden == 0, "dragging a selection never hides the LumaText edit surface");
        check(once == held && once.second > once.first, "a held pointer keeps the same dragged selection end");
        check(once.second < static_cast<int>(mouse_entries->at(2).name.size()) && once.second > 20,
            "releasing inside the field keeps the scrolled characters under the pointer");
        check(GetCapture() != s.hwndRenameEdit, "drag release frees mouse capture");
        SendMessageW(s.hwndRenameEdit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, y));
        SendMessageW(s.hwndRenameEdit, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(60, y));
        SendMessageW(s.hwndRenameEdit, WM_CHAR, L'Z', 1);
        SendMessageW(s.hwndRenameEdit, WM_LBUTTONUP, 0, MAKELPARAM(60, y));
        wchar_t typed[512]{}; GetWindowTextW(s.hwndRenameEdit, typed, ARRAYSIZE(typed));
        const auto typed_selection = selection();
        check(std::wstring(typed).find(L'Z') != std::wstring::npos && typed_selection.first == typed_selection.second &&
            wcslen(typed) < mouse_entries->at(2).name.size(),
            "typing during a drag replaces the selection shown on screen");
        RemoveWindowSubclass(s.hwndRenameEdit, observe, 97);
        SendMessageW(s.hwndRenameEdit, WM_KEYDOWN, VK_ESCAPE, 0);
    }
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown(); DestroyWindow(s.hwnd); s.hwnd = nullptr;
    CoUninitialize(); return failures ? 1 : 0;
}
