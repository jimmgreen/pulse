#include "app_input.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_sidebar_refresh.h"
#include "app_runtime.h"
#include "app_ops_ui.h"
#include "marquee_selection.h"
#include "../ui/view_layout.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <thread>

using namespace pulse;
namespace {
int CheckMarqueeClick(AppState& s, FILE* log) {
    const auto pump = [&](ULONGLONG duration) {
        const auto deadline = GetTickCount64() + duration;
        while (GetTickCount64() < deadline) {
            MSG message{};
            while (GetTickCount64() < deadline && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(1);
        }
    };
    auto* tab = ActiveTab(s);
    if (!s.isolatedTest || !tab || tab->EntryCount() < 8) return 2;
    const bool grouped_probe = GetEnvironmentVariableW(L"PULSE_TEST_MARQUEE_GROUPED_ONLY", nullptr, 0) != 0;
    if (grouped_probe) {
        if (tab->EntryCount() < 2500) {
            fprintf(log, "[ERROR] grouped probe requires 2500 fixture items\n");
            fflush(log);
            return 2;
        }
        tab->group_by = 2; // Date grouping, using the fixture's timestamps.
        if (GetEnvironmentVariableW(L"PULSE_TEST_MARQUEE_COLLAPSED", nullptr, 0)) {
            const auto view = BuildVm(s);
            const auto* groups = view.pane.Groups();
            if (!groups || groups->size() < 2) {
                fprintf(log, "[ERROR] collapsed-group fixture requires two date groups\n");
                return 2;
            }
            tab->collapsed_groups[tab->current_path].insert(groups->front().key);
            ++tab->group_collapse_rev;
            fprintf(log, "[PASS] first date group collapsed before cross-screen selection\n");
        }
    }
    const auto trace = [&](const char* stage, int scenario) {
        fprintf(log, "[STATE] scenario=%d stage=%s count=%zu snapshot=%p revision=%llu generation=%llu pending=%d active=%d dirty=%d drag=%d collapse=%d capture=%d\n",
            scenario, stage, static_cast<size_t>(tab->SelectedCount()), static_cast<const void*>(tab->snapshot.get()),
            static_cast<unsigned long long>(tab->selection_revision), static_cast<unsigned long long>(tab->view_generation),
            s.listSelection.PendingBand(), s.listSelection.BandActive(), s.listSelection.dirty, s.listSelection.PendingFile(), s.listSelection.DeferredIndex(), GetCapture() == s.hwnd);
        fflush(log);
    };
    tab->view_mode = ui::ViewMode::Details;
    tab->scroll_y = 0;
    s.appPrefs.single_click_open = false;
    s.shot.active = false;
    ShowWindow(s.hwnd, SW_SHOWNORMAL);
    InvalidateRect(s.hwnd, nullptr, FALSE);
    UpdateWindow(s.hwnd);
    pump(1200);
    int failures = 0;
    if (grouped_probe || GetEnvironmentVariableW(L"PULSE_TEST_MARQUEE_SCROLL_ONLY", nullptr, 0) ||
        GetEnvironmentVariableW(L"PULSE_TEST_MARQUEE_PHYSICAL_ONLY", nullptr, 0)) {
        if (tab->EntryCount() < 100) {
            fprintf(log, "[ERROR] cross-screen/physical fixture requires 100 items; actual=%zu\n", tab->EntryCount());
            fflush(log);
            return 2;
        }
        const auto points = [&](POINT& top, POINT& bottom, POINT& middle, int& middle_source, float& row_height) {
            const auto current = BuildVm(s);
            for (const auto& slot : current.pane_slots) {
                if (!slot.focused) continue;
                if (grouped_probe && (!slot.pane.Groups() || slot.pane.Groups()->empty())) {
                    fprintf(log, "[ERROR] date-grouped layout was not produced\n");
                    fflush(log);
                    return false;
                }
                const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
                ui::ViewLayout layout(slot.pane.view_mode, list, slot.pane.EntryCount(), slot.pane.scroll_x,
                    slot.pane.scroll_y, s.scale, s.renderer.ListRowHeightDip(slot.pane, list), slot.pane.Groups());
                const auto range = layout.VisibleRange();
                std::vector<std::pair<POINT, int>> rows;
                for (int row = range.first; row <= range.second && row < static_cast<int>(slot.pane.EntryCount()); ++row) {
                    const auto rect = layout.ItemRect(row);
                    if (rect.top < list.top || rect.bottom > list.bottom) continue;
                    const int y = static_cast<int>((rect.top + rect.bottom) * 0.5f);
                    row_height = rect.bottom - rect.top;
                    const int source = slot.pane.SourceIndex(row);
                    for (int x = static_cast<int>(list.right - 30 * s.scale); x > list.left; --x) {
                        const auto hit = s.renderer.HitTest(current,
                            {0, 0, float(s.compositor.Width()), float(s.compositor.Height())}, float(x), float(y));
                        if (hit.region == ui::HitTestResult::Row && hit.index == source &&
                            !s.renderer.PointInItemGrip(slot.pane, slot.rect, source, float(x), float(y))) {
                            rows.push_back({{x, y}, source});
                            break;
                        }
                    }
                }
                if (rows.size() < 8) return false;
                top = rows.front().first;
                bottom = rows.back().first;
                middle = rows[rows.size() / 2].first;
                middle_source = rows[rows.size() / 2].second;
                return true;
            }
            return false;
        };
        const auto mouse = [&](UINT message, WPARAM keys, POINT point) {
            SendMessageW(s.hwnd, message, keys, MAKELPARAM(point.x, point.y));
        };
        if (GetEnvironmentVariableW(L"PULSE_TEST_MARQUEE_PHYSICAL_ONLY", nullptr, 0)) {
            const auto error = [&](const char* stage) {
                fprintf(log, "[ERROR] physical environment/input stage=%s win32_error=%lu foreground=%p expected=%p\n",
                    stage, GetLastError(), static_cast<void*>(GetForegroundWindow()), static_cast<void*>(s.hwnd));
                fflush(log);
                return 2;
            };
            const auto screen_capture = [&](const std::wstring& path) {
                if (GetForegroundWindow() != s.hwnd) {
                    error("screen capture requires foreground fixture window");
                    return false;
                }
                const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
                if (dwm) {
                    const auto flush = reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(dwm, "DwmFlush"));
                    if (flush) flush();
                    FreeLibrary(dwm);
                }
                RECT client{};
                GetClientRect(s.hwnd, &client);
                POINT origin{};
                ClientToScreen(s.hwnd, &origin);
                const int width = client.right, height = client.bottom;
                if (width <= 0 || height <= 0) return false;
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = width;
                info.bmiHeader.biHeight = -height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                const DWORD bytes = static_cast<DWORD>(width) * static_cast<DWORD>(height) * 4;
                const HDC screen = GetDC(nullptr);
                const HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
                void* pixels = nullptr;
                const HBITMAP bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
                const HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
                bool okay = bitmap && BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
                if (okay) {
                    GdiFlush();
                    BITMAPFILEHEADER header{};
                    header.bfType = 0x4d42;
                    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
                    header.bfSize = header.bfOffBits + bytes;
                    FILE* output = nullptr;
                    okay = _wfopen_s(&output, path.c_str(), L"wb") == 0 && output;
                    if (output) {
                        okay = fwrite(&header, sizeof(header), 1, output) == 1 &&
                            fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, output) == 1 &&
                            fwrite(pixels, bytes, 1, output) == 1;
                        okay = fclose(output) == 0 && okay;
                    }
                }
                if (old) SelectObject(memory, old);
                if (bitmap) DeleteObject(bitmap);
                if (memory) DeleteDC(memory);
                if (screen) ReleaseDC(nullptr, screen);
                fprintf(log, "[%s] actual client screen capture %ls\n", okay ? "PASS" : "FAIL", path.c_str());
                fflush(log);
                return okay;
            };
            struct RestoreMouse {
                POINT cursor{};
                RestoreMouse() { GetCursorPos(&cursor); }
                ~RestoreMouse() {
                    INPUT release{};
                    release.type = INPUT_MOUSE;
                    release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                    SendInput(1, &release, sizeof(INPUT));
                    SetCursorPos(cursor.x, cursor.y);
                }
            } restore_mouse;
            ShowWindow(s.hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(s.hwnd);
            SetFocus(s.hwnd);
            pump(300);
            if (GetForegroundWindow() != s.hwnd) {
                BringWindowToTop(s.hwnd);
                SetForegroundWindow(s.hwnd);
                pump(300);
                if (GetForegroundWindow() != s.hwnd) {
                    const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
                    const DWORD current_thread = GetCurrentThreadId();
                    const bool attached = foreground_thread && foreground_thread != current_thread &&
                        AttachThreadInput(current_thread, foreground_thread, TRUE);
                    BringWindowToTop(s.hwnd);
                    SetForegroundWindow(s.hwnd);
                    SetFocus(s.hwnd);
                    if (attached) AttachThreadInput(current_thread, foreground_thread, FALSE);
                    pump(300);
                    if (GetForegroundWindow() != s.hwnd) return error("foreground activation after temporary input attachment");
                }
            }
            const auto physical = [&](POINT client, DWORD flags) {
                ClientToScreen(s.hwnd, &client);
                INPUT input{};
                input.type = INPUT_MOUSE;
                input.mi.dx = MulDiv(client.x - GetSystemMetrics(SM_XVIRTUALSCREEN), 65535,
                    std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
                input.mi.dy = MulDiv(client.y - GetSystemMetrics(SM_YVIRTUALSCREEN), 65535,
                    std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
                input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | flags;
                SetLastError(ERROR_SUCCESS);
                const UINT sent = SendInput(1, &input, sizeof(INPUT));
                if (sent != 1) {
                    fprintf(log, "[ERROR] SendInput flags=%lu sent=%u error=%lu screen=(%ld,%ld)\n",
                        flags, sent, GetLastError(), client.x, client.y);
                    fflush(log);
                }
                return sent == 1;
            };
            for (int repeat = 0; repeat < 3; ++repeat) {
                POINT top{}, bottom{}, middle{};
                int target_source = -1;
                float row_height = 0;
                tab->scroll_y = 0;
                tab->ClearSelection();
                InvalidateRect(s.hwnd, nullptr, FALSE);
                UpdateWindow(s.hwnd);
                pump(200);
                if (!points(top, bottom, middle, target_source, row_height)) return error("locate visible rows");
                const auto current = BuildVm(s);
                const auto list = s.renderer.PaneListRect(current.pane, FocusedPaneRect(s));
                POINT edge{bottom.x, static_cast<LONG>(list.bottom - 1)};
                top.x -= static_cast<LONG>(80 * s.scale);
                const auto hit = s.renderer.HitTest(current,
                    {0, 0, float(s.compositor.Width()), float(s.compositor.Height())}, float(top.x), float(top.y));
                if (hit.region != ui::HitTestResult::Row ||
                    s.renderer.PointInItemGrip(current.pane, FocusedPaneRect(s), hit.index, float(top.x), float(top.y))) return error("start row whitespace hit test");
                if (!physical(top, MOUSEEVENTF_LEFTDOWN)) return error("drag press");
                pump(30);
                if (!physical(edge, 0)) return error("drag move to edge");
                const auto deadline = GetTickCount64() + 4000;
                do { pump(25); } while (tab->scroll_y < 8 * row_height && GetTickCount64() < deadline);
                const float scrolled = tab->scroll_y;
                if (!physical(edge, MOUSEEVENTF_LEFTUP)) return error("drag release");
                pump(10);
                const auto multiple = tab->SelectedCount();
                if (!points(top, bottom, middle, target_source, row_height)) return error("locate visible rows");
                if (!physical(middle, MOUSEEVENTF_LEFTDOWN) || !physical(middle, MOUSEEVENTF_LEFTUP)) return error("single click");
                pump(10);
                const bool immediate = tab->SelectedCount() == 1 && tab->IsSelected(target_source);
                fprintf(log, "[%s] physical immediate single selection count=%zu target=%d\n",
                    immediate ? "PASS" : "FAIL", static_cast<size_t>(tab->SelectedCount()), target_source);
                failures += !immediate;
                wchar_t immediate_data[32768]{};
                if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", immediate_data, ARRAYSIZE(immediate_data))) {
                    UpdateWindow(s.hwnd);
                    failures += !screen_capture(std::wstring(immediate_data) + L"\\physical-immediate-" +
                        std::to_wstring(repeat) + L".bmp");
                }
                if (!physical({middle.x - 5, middle.y - 1}, 0)) return error("post-click hover move");
                pump(500);
                const bool okay = scrolled >= 8 * row_height && multiple > 8 && tab->SelectedCount() == 1 &&
                    tab->IsSelected(target_source) && !s.listSelection.BandActive() && !s.listSelection.PendingBand();
                fprintf(log, "[%s] physical repeat=%d scroll=%.1f before=%zu selected_count=%zu index=%d target=%d active=%d pending=%d\n",
                    okay ? "PASS" : "FAIL", repeat, scrolled, static_cast<size_t>(multiple),
                    static_cast<size_t>(tab->SelectedCount()), tab->selected_index, target_source, s.listSelection.BandActive(), s.listSelection.PendingBand());
                failures += !okay;
                wchar_t data[32768]{};
                if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data))) {
                    const auto base = std::wstring(data) + L"\\physical-" + std::to_wstring(repeat);
                    const bool screen_fast = screen_capture(base + L"-screen-fast.bmp");
                    const bool fast = s.compositor.SaveSnapshot((base + L"-fast.png").c_str());
                    InvalidateRect(s.hwnd, nullptr, FALSE);
                    UpdateWindow(s.hwnd);
                    const bool screen_full = screen_capture(base + L"-screen-full.bmp");
                    const bool full = s.compositor.SaveSnapshot((base + L"-full.png").c_str());
                    failures += !(fast && full && screen_fast && screen_full);
                    fprintf(log, "[%s] physical snapshots saved\n", fast && full ? "PASS" : "FAIL");
                }
                fflush(log);
            }
            return failures ? 1 : 0;
        }
        for (int repeat = 0; repeat < (grouped_probe ? 4 : 3); ++repeat) for (bool reverse : {false, true}) {
            POINT top{}, bottom{}, middle{};
            int target_source = -1;
            float row_height = 0;
            tab->scroll_y = 0;
            tab->ClearSelection();
            if (!points(top, bottom, middle, target_source, row_height)) return 2;
            tab->scroll_y = reverse ? 16 * row_height : 0;
            InvalidateRect(s.hwnd, nullptr, FALSE);
            UpdateWindow(s.hwnd);
            if (!points(top, bottom, middle, target_source, row_height)) return 2;
            POINT start = reverse ? bottom : top;
            start.x -= static_cast<LONG>(80 * s.scale);
            const auto start_vm = BuildVm(s);
            const auto start_hit = s.renderer.HitTest(start_vm,
                {0, 0, float(s.compositor.Width()), float(s.compositor.Height())}, float(start.x), float(start.y));
            if (start_hit.region != ui::HitTestResult::Row ||
                s.renderer.PointInItemGrip(start_vm.pane, FocusedPaneRect(s), start_hit.index, float(start.x), float(start.y))) {
                fprintf(log, "[ERROR] cross-screen start must be row whitespace\n");
                return 2;
            }
            const POINT first_end = reverse ? top : bottom;
            mouse(WM_LBUTTONDOWN, MK_LBUTTON, start);
            mouse(WM_MOUSEMOVE, MK_LBUTTON, first_end);
            UpdateMarqueeFrame(s);
            UpdateWindow(s.hwnd);
            tab->scroll_y += (reverse ? -8 : 8) * row_height;
            UpdateMarqueeFrame(s);
            InvalidateRect(s.hwnd, nullptr, FALSE);
            UpdateWindow(s.hwnd);
            if (!points(top, bottom, middle, target_source, row_height)) return 2;
            const POINT end = reverse ? top : bottom;
            mouse(WM_MOUSEMOVE, MK_LBUTTON, end);
            UpdateMarqueeFrame(s);
            UpdateWindow(s.hwnd);
            mouse(WM_LBUTTONUP, 0, end);
            const auto multiple = tab->SelectedCount();
            bool stages_single = true;
            const auto check_single = [&] {
                stages_single &= tab->SelectedCount() == 1 && tab->IsSelected(target_source);
            };
            mouse(WM_LBUTTONDOWN, MK_LBUTTON, middle);
            if (grouped_probe) check_single();
            if (grouped_probe) {
                trace("down", repeat);
                if (repeat == 1) mouse(WM_MOUSEMOVE, 0, middle);
                if (repeat == 2 && GetCapture() == s.hwnd) ReleaseCapture();
                if (repeat == 3) {
                    auto replacement = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
                    tab->SetSnapshot(replacement);
                }
                trace("after-injection", repeat);
                check_single();
            }
            mouse(WM_LBUTTONUP, 0, middle);
            if (grouped_probe) { trace("up", repeat); check_single(); }
            UpdateWindow(s.hwnd);
            if (grouped_probe) { trace("first-frame", repeat); check_single(); }
            mouse(WM_MOUSEMOVE, 0, top);
            pump(100);
            if (grouped_probe) { trace("100ms", repeat); check_single(); }
            pump(400);
            if (grouped_probe) { trace("500ms", repeat); check_single(); }
            const bool normal = multiple > 8 && tab->SelectedCount() == 1 && tab->IsSelected(target_source);
            const bool okay = normal && (!grouped_probe || (stages_single &&
                !s.listSelection.BandActive() && !s.listSelection.PendingBand() && s.listSelection.DeferredIndex() < 0));
            if (grouped_probe) fprintf(log, "[EXPECT] scenario=%d committed plain click remains target-only through cancellation and frames\n", repeat);
            fprintf(log, "[%s] cross-screen repeat=%d reverse=%d before=%zu selected_count=%zu index=%d target=%d\n",
                okay ? "PASS" : "FAIL", repeat, reverse, static_cast<size_t>(multiple),
                static_cast<size_t>(tab->SelectedCount()), tab->selected_index, target_source);
            failures += !okay;
            wchar_t data[32768]{};
            if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data))) {
                const auto base = std::wstring(data) + L"\\cross-screen-" + std::to_wstring(repeat * 2 + int(reverse));
                const bool fast = s.compositor.SaveSnapshot((base + L"-fast.png").c_str());
                InvalidateRect(s.hwnd, nullptr, FALSE);
                UpdateWindow(s.hwnd);
                const bool full = s.compositor.SaveSnapshot((base + L"-full.png").c_str());
                fprintf(log, "[%s] cross-screen snapshots saved\n", fast && full ? "PASS" : "FAIL");
                failures += !(fast && full);
            }
            fflush(log);
        }
        if (grouped_probe) {
            POINT top{}, bottom{}, middle{};
            int target_source = -1;
            float row_height = 0;
            if (!points(top, bottom, middle, target_source, row_height)) return 2;
            const auto check = [&](bool okay, const char* label) {
                fprintf(log, "[%s] %s\n", okay ? "PASS" : "FAIL", label);
                failures += !okay;
            };
            const std::vector<int> original{target_source, target_source + 1, target_source + 2};
            tab->SelectIndices(original);
            mouse(WM_LBUTTONDOWN, MK_LBUTTON, middle);
            mouse(WM_MOUSEMOVE, MK_LBUTTON, bottom);
            UpdateMarqueeFrame(s);
            check(s.listSelection.BandActive(), "Esc fixture activates marquee");
            SendMessageW(s.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
            check(tab->SelectedIndices() == original && !s.listSelection.BandActive() && !s.listSelection.PendingBand(),
                "Esc after active plain marquee restores pre-press multiple selection");

            const auto current = BuildVm(s);
            POINT name = middle;
            bool found_name = false;
            const auto bounds = FocusedPaneRect(s);
            for (int x = static_cast<int>(bounds.left); x < static_cast<int>(bounds.right); ++x) {
                const auto hit = s.renderer.HitTest(current,
                    {0, 0, float(s.compositor.Width()), float(s.compositor.Height())}, float(x), float(middle.y));
                if (hit.region == ui::HitTestResult::Row && hit.index == target_source &&
                    s.renderer.PointInItemName(current.pane, bounds, target_source, float(x), float(middle.y))) {
                    name.x = x; found_name = true; break;
                }
            }
            check(found_name, "locate filename drag grip");
            if (found_name) {
                tab->SelectIndices(original);
                mouse(WM_LBUTTONDOWN, MK_LBUTTON, name);
                check(tab->SelectedIndices() == original && s.listSelection.PendingFile(),
                    "filename drag press preserves full payload selection");
                // Cancel before movement so no OLE file drag or file open occurs.
                if (GetCapture() == s.hwnd) ReleaseCapture();
            }
            tab->SelectIndices(original);
            HandleListRowClick(s, target_source, true, false);
            check(tab->SelectedIndices() == original, "Ctrl selected-row press defers toggle");
            FinishListRowClick(s);
            check(!tab->IsSelected(target_source) && tab->SelectedCount() == 2, "Ctrl release toggles target only");
            tab->SelectOnly(target_source);
            HandleListRowClick(s, target_source + 2, false, true);
            FinishListRowClick(s);
            check(tab->SelectedCount() == 3 && tab->IsSelected(target_source + 1), "Shift click retains range semantics");
            // Pure marquee delta checks avoid changing real keyboard state.
            for (bool ctrl : {false, true}) {
                app::MarqueeSelection gesture;
                std::unordered_set<int> selected{1, 2};
                const std::unordered_set<int> base = selected;
                bool all = false;
                std::vector<int> hits{2, 3};
                gesture.Apply(selected, all, base, true, ctrl, 10, hits);
                const std::unordered_set<int> expected = ctrl ? std::unordered_set<int>{1, 3} : std::unordered_set<int>{1, 2, 3};
                check(!all && selected == expected, ctrl ? "Ctrl marquee toggles against original base" : "Shift marquee adds to original base");
            }
            fflush(log);
        }
        return failures ? 1 : 0;
    }
    const auto vm = BuildVm(s);
    for (const auto& slot : vm.pane_slots) {
        if (!slot.focused) continue;
        const auto list = s.renderer.PaneListRect(slot.pane, slot.rect);
        ui::ViewLayout layout(slot.pane.view_mode, list, slot.pane.EntryCount(), slot.pane.scroll_x,
            slot.pane.scroll_y, s.scale, s.renderer.ListRowHeightDip(slot.pane, list), slot.pane.Groups());
        const auto visible = layout.VisibleRange();
        const int row_count = std::min(18, std::min(static_cast<int>(slot.pane.EntryCount()), visible.second));
        if (row_count < 8) return 2;
        const int middle = row_count / 2;
        POINT names[18]{}, blanks[18]{}, right_blanks[18]{};
        int sources[18]{};
        for (int row = 0; row < row_count; ++row) {
            const auto rect = layout.ItemRect(row);
            if (rect.top < list.top || rect.bottom > list.bottom) {
                fprintf(log, "[ERROR] eight fully visible rows required\n");
                return 2;
            }
            const int y = static_cast<int>((rect.top + rect.bottom) / 2);
            bool name_found = false, blank_found = false;
            int name_right = 0;
            sources[row] = slot.pane.SourceIndex(row);
            for (int x = static_cast<int>(list.left); x < static_cast<int>(list.right); ++x) {
                const auto hit = s.renderer.HitTest(vm,
                    {0, 0, float(s.compositor.Width()), float(s.compositor.Height())}, float(x), float(y));
                if (hit.region != ui::HitTestResult::Row || hit.index != sources[row]) continue;
                if (s.renderer.PointInItemName(slot.pane, slot.rect, hit.index, float(x), float(y))) {
                    if (!name_found) names[row] = {x, y};
                    name_found = true;
                    name_right = x;
                }
                if (name_found && x > name_right + 24 * s.scale && !s.renderer.PointInItemGrip(slot.pane, slot.rect, hit.index, float(x), float(y))) {
                    if (!blank_found) blanks[row] = {x, y};
                    right_blanks[row] = {x, y}; blank_found = true;
                }
            }
            if (!name_found || !blank_found) {
                fprintf(log, "[ERROR] cannot locate visible row %d name/blank points\n", row);
                return 2;
            }
        }
        const auto mouse = [&](UINT message, WPARAM keys, POINT p) {
            SendMessageW(s.hwnd, message, keys, MAKELPARAM(p.x, p.y));
        };
        for (bool reverse : {false, true}) for (bool name : {false, true}) {
            tab->ClearSelection();
            const POINT start = reverse ? right_blanks[row_count - 1] : blanks[0];
            const POINT end = reverse ? blanks[0] : right_blanks[row_count - 1];
            mouse(WM_LBUTTONDOWN, MK_LBUTTON, start);
            mouse(WM_MOUSEMOVE, MK_LBUTTON, end);
            UpdateMarqueeFrame(s);
            UpdateWindow(s.hwnd);
            mouse(WM_LBUTTONUP, 0, end);
            const auto multiple = tab->SelectedCount();
            const POINT target = name ? names[middle] : blanks[middle];
            mouse(WM_LBUTTONDOWN, MK_LBUTTON, target);
            mouse(WM_LBUTTONUP, 0, target);
            UpdateWindow(s.hwnd);
            mouse(WM_MOUSEMOVE, 0, blanks[row_count - 2]);
            UpdateWindow(s.hwnd);
            pump(500);
            wchar_t data[32768]{};
            if (GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data))) {
                const auto suffix = std::to_wstring(int(reverse) * 2 + int(name)) + L".png";
                const auto base = std::wstring(data) + L"\\";
                const bool fast = s.compositor.SaveSnapshot((base + L"marquee-fast-click-" + suffix).c_str());
                InvalidateRect(s.hwnd, nullptr, FALSE);
                UpdateWindow(s.hwnd);
                const bool full = s.compositor.SaveSnapshot((base + L"forced-full-" + suffix).c_str());
                fprintf(log, "[%s] fast/full snapshots saved\n", fast && full ? "PASS" : "FAIL");
                failures += !(fast && full);
            }
            const bool okay = multiple >= static_cast<size_t>(row_count) && tab->SelectedCount() == 1 && tab->IsSelected(sources[middle]);
            fprintf(log, "[%s] marquee reverse=%d click_name=%d before=%zu selected_count=%zu index=%d\n",
                okay ? "PASS" : "FAIL", reverse, name, static_cast<size_t>(multiple),
                static_cast<size_t>(tab->SelectedCount()), tab->selected_index);
            fflush(log);
            failures += !okay;
        }
        // Keep events synchronous to exercise both a queued burst and frames
        // interleaved between release, click, and hover without opening files.
        for (int repeat = 0; repeat < 3; ++repeat) {
            for (bool reverse : {false, true}) for (bool paints : {false, true}) {
                for (bool slight_move : {false, true}) for (int location = 0; location < 3; ++location) {
                    tab->ClearSelection();
                    const POINT start = reverse ? right_blanks[row_count - 1] : blanks[0];
                    const POINT end = reverse ? blanks[0] : right_blanks[row_count - 1];
                    mouse(WM_LBUTTONDOWN, MK_LBUTTON, start);
                    mouse(WM_MOUSEMOVE, MK_LBUTTON, end);
                    if (paints) UpdateWindow(s.hwnd);
                    mouse(WM_LBUTTONUP, 0, end);
                    const auto multiple = tab->SelectedCount();
                    if (paints) UpdateWindow(s.hwnd);
                    const POINT target = location == 0 ? names[middle] : location == 1 ? blanks[middle] : right_blanks[middle];
                    mouse(WM_LBUTTONDOWN, MK_LBUTTON, target);
                    if (slight_move) mouse(WM_MOUSEMOVE, MK_LBUTTON, {target.x, target.y + 1});
                    if (paints) UpdateWindow(s.hwnd);
                    mouse(WM_LBUTTONUP, 0, slight_move ? POINT{target.x, target.y + 1} : target);
                    if (paints) UpdateWindow(s.hwnd);
                    PostMessageW(s.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(blanks[row_count - 2].x, blanks[row_count - 2].y));
                    pump(500);
                    const bool okay = multiple >= static_cast<size_t>(row_count) && tab->SelectedCount() == 1 && tab->IsSelected(sources[middle]);
                    fprintf(log, "[%s] burst repeat=%d reverse=%d paints=%d slight_move=%d location=%d before=%zu selected_count=%zu index=%d\n",
                        okay ? "PASS" : "FAIL", repeat, reverse, paints, slight_move, location,
                        static_cast<size_t>(multiple), static_cast<size_t>(tab->SelectedCount()), tab->selected_index);
                    failures += !okay;
                    // Save every timing/location combination once, plus failures.
                    wchar_t data[32768]{};
                    if ((repeat == 0 || !okay) && GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data))) {
                        const int id = (((repeat * 2 + int(reverse)) * 2 + int(paints)) * 2 + int(slight_move)) * 3 + location;
                        const auto base = std::wstring(data) + L"\\burst-" + std::to_wstring(id);
                        const bool fast = s.compositor.SaveSnapshot((base + L"-fast.png").c_str());
                        InvalidateRect(s.hwnd, nullptr, FALSE);
                        UpdateWindow(s.hwnd);
                        const bool full = s.compositor.SaveSnapshot((base + L"-full.png").c_str());
                        fprintf(log, "[%s] burst snapshots id=%d\n", fast && full ? "PASS" : "FAIL", id);
                        failures += !(fast && full);
                    }
                    fflush(log);
                }
            }
        }
        return failures ? 1 : 0;
    }
    return 2;
}

int CheckDeleteSelection(AppState& s, FILE* log) {
    wchar_t data[32768]{};
    if (!s.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data))) return 2;
    auto* tab = ActiveTab(s);
    const std::wstring fixture = fs::NormalizePath(std::wstring(data) + L"\\files");
    if (!tab || fs::NormalizePath(tab->current_path) != fs::NormalizePath(fixture) || tab->EntryCount() != 5) {
        fprintf(log, "[ERROR] fixture guard path=%ls expected=%ls count=%zu\n",
                tab ? tab->current_path.c_str() : L"", fixture.c_str(), tab ? tab->EntryCount() : 0);
        return 2;
    }
    for (int i = 0; i < 5; ++i) {
        const std::wstring expected(1, static_cast<wchar_t>(L'a' + i));
        if (tab->EntryAt(i).name != expected + L".txt") {
            fprintf(log, "[ERROR] fixture row %d name=%ls\n", i, tab->EntryAt(i).name.c_str());
            return 2;
        }
    }
    s.appPrefs.confirm_recycle_delete = false;
    if (GetEnvironmentVariableW(L"PULSE_TEST_DELETE_LATENCY_ICONS", nullptr, 0)) {
        tab->view_mode = ui::ViewMode::LargeIcons;
        ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
    int failures = 0;
    std::atomic<ULONGLONG> posted_at{0};
    ULONGLONG handler_at = 0, handler_done_at = 0;
    const auto pump = [&] {
        MSG msg{};
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            const bool deletion = msg.hwnd == s.hwnd && msg.message == WM_KEYDOWN && msg.wParam == VK_DELETE;
            if (deletion) handler_at = GetTickCount64();
            const auto dispatch_start = GetTickCount64();
            TranslateMessage(&msg); DispatchMessageW(&msg);
            const auto dispatch_ms = GetTickCount64() - dispatch_start;
            if (dispatch_ms >= 100) {
                fprintf(log, "[SLOW_MESSAGE] message=0x%x wparam=%llu elapsed=%llu ms\n", msg.message,
                    static_cast<unsigned long long>(msg.wParam), dispatch_ms);
                fflush(log);
            }
            if (deletion) handler_done_at = GetTickCount64();
        } else Sleep(1);
    };
    const auto remove = [&](std::vector<int> indices, int remaining, const wchar_t* expected, const char* label) {
        if (!indices.empty()) tab->SelectIndices(indices);
        const auto paths = SelectedFullPaths(*tab);
        fprintf(log, "[STATE] window_visible=%d undo_capsule_visible=%d rows=%zu\n",
            IsWindowVisible(s.hwnd) != FALSE, s.notification_toast.IsVisible(), tab->EntryCount());
        const auto started = GetTickCount64();
        posted_at.store(0);
        handler_at = handler_done_at = 0;
        std::jthread input([&] {
            Sleep(20);
            posted_at.store(GetTickCount64());
            PostMessageW(s.hwnd, WM_KEYDOWN, VK_DELETE, 0);
        });
        ULONGLONG preflight_ms = 0, removed_ms = 0;
        const auto deadline = GetTickCount64() + 15000;
        bool completed = false;
        while (GetTickCount64() < deadline) {
            pump();
            if (handler_done_at && !preflight_ms && !s.delete_preflight) preflight_ms = GetTickCount64() - started;
            if (!removed_ms && std::all_of(paths.begin(), paths.end(), [](const auto& path) {
                return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
            })) removed_ms = GetTickCount64() - started;
            completed = !tab->loading && tab->EntryCount() == static_cast<size_t>(remaining);
            for (const auto& path : paths) completed &= GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
            if (completed) break;
        }
        fprintf(log, "[TIMING] %s queued=%llu handler=%llu preflight=%llu absent=%llu list=%llu ms\n", label,
            handler_at ? handler_at - posted_at.load() : 0, handler_done_at ? handler_done_at - handler_at : 0,
            preflight_ms ? preflight_ms - (posted_at.load() - started) : 0,
            removed_ms ? removed_ms - (posted_at.load() - started) : 0, GetTickCount64() - posted_at.load());
        const std::wstring actual = tab->selected_index >= 0 ? tab->EntryAt(tab->selected_index).name : L"";
        const bool okay = completed && actual == expected && (remaining || tab->SelectedCount() == 0);
        fprintf(log, "[%s] %s actual=%ls index=%d remaining=%zu\n", okay ? "PASS" : "FAIL", label,
                actual.c_str(), tab->selected_index, tab->EntryCount());
        fflush(log);
        failures += !okay;
        return okay;
    };
    if (GetEnvironmentVariableW(L"PULSE_TEST_DELETE_LATENCY_ONLY", nullptr, 0)) {
        if (!remove({0}, 4, L"b.txt", "Del selects b")) return 1;
        if (!remove({}, 3, L"c.txt", "immediate Del selects c")) return 1;
        if (!remove({}, 2, L"d.txt", "immediate Del selects d")) return 1;
        if (!remove({}, 1, L"e.txt", "immediate Del selects e")) return 1;
        if (!remove({}, 0, L"", "immediate Del clears final selection")) return 1;
        return failures ? 1 : 0;
    }
    if (!remove({2}, 4, L"d.txt", "real recycle command: middle selection follows successor")) return 1;
    if (!remove({3}, 3, L"d.txt", "real recycle command: last selection follows predecessor")) return 1;
    if (!remove({1, 2}, 1, L"a.txt", "real recycle command: multiple deletion follows surviving neighbor")) return 1;
    if (!remove({0}, 0, L"", "real recycle command: deleting final item clears selection")) return 1;
    for (wchar_t ch = L'a'; ch <= L'e'; ++ch) {
        const auto path = fixture + L"\\" + std::wstring(1, ch) + L".txt";
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 2;
        CloseHandle(file);
    }
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    fs::EnumerateDirectory(fixture, *entries);
    std::sort(entries->begin(), entries->end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    tab->SetSnapshot(entries);
    for (const auto& name : {L"c.txt", L"e.txt"}) {
        int index = -1;
        for (int i = 0; i < static_cast<int>(tab->EntryCount()); ++i) if (tab->EntryAt(i).name == name) index = i;
        if (index < 0) return 2;
        tab->SelectOnly(index);
        if (!DeleteFileW((fixture + L"\\" + name).c_str())) return 2;
        const bool applied = ApplyNotifyToVisible(s, fixture, {FILE_ACTION_REMOVED, name, {}});
        const auto actual = tab->selected_index >= 0 ? tab->EntryAt(tab->selected_index).name : L"";
        const bool okay = applied && actual == L"d.txt";
        fprintf(log, "[%s] directory-notify handler: applied=%d removed=%ls selected=%ls\n",
                okay ? "PASS" : "FAIL", applied ? 1 : 0, name, actual.c_str());
        fflush(log);
        failures += !okay;
    }
    return failures ? 1 : 0;
}

template<class Check>
void CheckFilteredRanges(Check check) {
    // In-memory rows only: validate the same path list consumed by operations,
    // without dispatching delete/move or touching files/preferences.
    for (bool descending : {false, true}) {
        for (int mode = 0; mode < 5; ++mode) {
            app::Pane pane;
            auto& tab = pane.view;
            tab.current_path = L"C:\\pulse-selection-fixture";
            auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
            const wchar_t* names[] = {L"a.keep", L"b.secret", L"c.keep"};
            for (int i = 0; i < 3; ++i) {
                auto& entry = (*entries)[i];
                entry.name = names[i];
                entry.full_path = tab.current_path + L"\\" + names[i];
                entry.size = i == 1 ? 1000 : 10;
                entry.attrs = mode == 4 && i == 1 ? FILE_ATTRIBUTE_HIDDEN : FILE_ATTRIBUTE_NORMAL;
            }
            if (descending) std::reverse(entries->begin(), entries->end());
            tab.SetSnapshot(entries);
            app::PlacesCatalog places;
            places.persist = false;
            app::ColorTag tag;
            tag.name = L"keep";
            tag.paths = {(*entries)[0].full_path, (*entries)[2].full_path};
            places.tags.push_back(std::move(tag));
            places.TagsReordered();
            if (mode == 0) tab.filter_text = L"*.keep";
            if (mode == 1) tab.filter_text = L"#keep";
            if (mode == 2 || mode == 3) {
                tab.compare_marks = std::make_shared<const std::vector<uint8_t>>(std::initializer_list<uint8_t>{1, 0, 1});
                tab.compare_diff_only = true;
            }
            if (mode == 3) tab.filter_text = L"*.keep #keep";
            ui::PaneViewModel view;
            app::FillPaneViewModel(view, pane, &places);
            check(view.EntryCount() == 2 && view.SourceIndex(0) == 0 && view.SourceIndex(1) == 2,
                  "filter pipeline exposes only expected rows (name/tag/diff/combined/hidden, ascending/descending)");
            for (bool reverse : {false, true}) {
                const int from = reverse ? 2 : 0;
                const int to = reverse ? 0 : 2;
                tab.SelectOnly(from);
                tab.SelectRange(from, to, &view);
                auto paths = SelectedFullPaths(tab);
                auto expected = std::vector<std::wstring>{(*entries)[0].full_path, (*entries)[2].full_path};
                std::sort(paths.begin(), paths.end());
                std::sort(expected.begin(), expected.end());
                uint64_t bytes = 0;
                int files = 0, folders = 0;
                tab.SelectionSizeSummary(&bytes, &files, &folders);
                check(paths == expected && tab.SelectedCount() == 2 && bytes == 20 && files == 2 && folders == 0,
                      "forward/reverse Shift range exports only visible operation paths and matching status totals");
                tab.SelectOnly(from);
                tab.MoveFocus(to, true, &view);
                check(tab.SelectedIndices() == std::vector<int>({0, 2}) && tab.selected_index == to,
                      "keyboard focus extension uses the displayed range");
            }
            tab.SelectOnly(1);
            tab.MoveFocus(2, true, &view);
            check(tab.SelectedIndices() == std::vector<int>({2}) && tab.selection_anchor == 2,
                  "removed anchor restarts range at visible target");
        }
    }
    app::Pane pane;
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(3);
    for (auto& entry : *entries) entry.attrs = FILE_ATTRIBUTE_NORMAL;
    pane.view.SetSnapshot(entries);
    ui::PaneViewModel view;
    app::FillPaneViewModel(view, pane);
    auto groups = std::make_shared<ui::ListGroups>(3);
    for (int i = 0; i < 3; ++i) { (*groups)[i].first = i; (*groups)[i].count = 1; }
    (*groups)[1].collapsed = true;
    view.groups = groups;
    view.view_mode = ui::ViewMode::Details;
    pane.view.SelectRange(0, 2, &view);
    check(pane.view.SelectedIndices() == std::vector<int>({0, 2}), "range excludes collapsed group members");
    pane.view.SelectOnly(1);
    pane.view.MoveFocus(2, true, &view);
    check(pane.view.SelectedIndices() == std::vector<int>({2}), "collapsed anchor restarts range");
    view.filter_map = std::make_shared<const ui::PaneViewModel::FilterMap>();
    pane.view.SelectRange(0, 2, &view);
    check(pane.view.SelectedCount() == 0, "empty filtered view cannot select source rows");
}

// Explorer-style keyboard selection through the real key handler. GetKeyState
// reads this thread's key table, so SetKeyboardState stands in for held keys.
int CheckKeyboardSelection(AppState& s, FILE* log) {
    auto* tab = ActiveTab(s);
    if (!tab || tab->EntryCount() < 4) {
        fprintf(log, "[ERROR] keyboard fixture needs 4 rows, got %zu\n", tab ? tab->EntryCount() : 0);
        return 2;
    }
    ui::PaneViewModel view;
    app::FillPaneViewModel(view, *s.pane, &s.places);
    const auto row = [&](int v) { return view.SourceIndex(v); };
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        fflush(log);
        failures += !ok;
    };
    const auto key = [&](UINT vk, bool ctrl, bool shift) {
        BYTE keys[256]{};
        GetKeyboardState(keys);
        keys[VK_CONTROL] = keys[VK_LCONTROL] = ctrl ? 0x80 : 0;
        keys[VK_SHIFT] = keys[VK_LSHIFT] = shift ? 0x80 : 0;
        SetKeyboardState(keys);
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, vk, 0);
        keys[VK_CONTROL] = keys[VK_LCONTROL] = keys[VK_SHIFT] = keys[VK_LSHIFT] = 0;
        SetKeyboardState(keys);
    };
    const auto rows = [&](std::initializer_list<int> views) {
        std::vector<int> out;
        for (int v : views) out.push_back(row(v));
        std::sort(out.begin(), out.end());
        return out;
    };
    tab->SelectOnly(row(0));
    key(VK_DOWN, true, false);
    key(VK_DOWN, true, false);
    check(tab->SelectedIndices() == rows({0}) && tab->FocusIndex() == row(2),
          "Ctrl+Down moves focus without changing the selection");
    ui::PaneViewModel ring;
    app::FillPaneViewModel(ring, *s.pane, &s.places);
    check(ring.focus_ring_source == row(2), "the unselected focused row gets the focus ring");
    key(VK_SPACE, true, false);
    check(tab->SelectedIndices() == rows({0, 2}), "Ctrl+Space toggles the focused row on");
    key(VK_DOWN, true, true);
    check(tab->SelectedIndices() == rows({0, 2, 3}), "Ctrl+Shift+Down adds the range from the anchor");
    HandleListRowClick(s, row(3), true, false);
    FinishListRowClick(s);
    check(tab->SelectedIndices() == rows({0, 2}) && tab->FocusIndex() == row(3) &&
          tab->selection_anchor == row(3), "Ctrl-click deselects, keeps focus and becomes the anchor");
    HandleListRowClick(s, row(1), false, true);
    check(tab->SelectedIndices() == rows({1, 2, 3}), "Shift-click replaces with anchor..row");
    key(VK_HOME, true, false);
    check(tab->SelectedIndices() == rows({1, 2, 3}) && tab->FocusIndex() == row(0),
          "Ctrl+Home moves focus only");
    key(VK_SPACE, false, true);
    check(tab->SelectedIndices() == rows({0, 1, 2, 3}), "Shift+Space selects anchor..focus");
    key(VK_DOWN, false, false);
    check(tab->SelectedIndices() == rows({1}), "plain Down continues from the moved focus");
    tab->SelectOnly(row(0));
    HandleListRowClick(s, row(2), true, false);
    HandleListRowClick(s, row(3), true, true);
    check(tab->SelectedIndices() == rows({0, 2, 3}), "Ctrl+Shift-click adds the range");
    key(VK_UP, false, true);
    check(tab->SelectedIndices() == rows({2}), "Shift+Up replaces with anchor..focus");
    return failures ? 1 : 0;
}
}
// Runs only in an explicitly isolated --shot test instance with fixture files.
int RunSelectionRefreshProbe(AppState& s, FILE* log);
int RunBackgroundDragProbe(AppState& s, FILE* log);
int RunSelectionInputProbe(AppState& s, const wchar_t* output) {
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"w") || !log) return 2;
    if (GetEnvironmentVariableW(L"PULSE_TEST_BACKGROUND_DRAG_ONLY", nullptr, 0)) {
        const int result = RunBackgroundDragProbe(s, log);
        std::fclose(log);
        return result;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_SELECTION_REFRESH_ONLY", nullptr, 0)) {
        const int result = RunSelectionRefreshProbe(s, log);
        std::fclose(log);
        return result;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_MARQUEE_CLICK_ONLY", nullptr, 0)) {
        const int result = CheckMarqueeClick(s, log);
        std::fclose(log);
        return result;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_DELETE_SELECTION_ONLY", nullptr, 0)) {
        const int result = CheckDeleteSelection(s, log);
        std::fclose(log);
        return result;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_KEYBOARD_SELECTION_ONLY", nullptr, 0)) {
        const int result = CheckKeyboardSelection(s, log);
        std::fclose(log);
        return result;
    }
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(log);
        failures += !ok;
    };
    CheckFilteredRanges(check);
    if (GetEnvironmentVariableW(L"PULSE_TEST_FILTERED_SELECTION_ONLY", nullptr, 0)) {
        extern int RunNavigationAuditProbe(AppState&, FILE*);
        failures += RunNavigationAuditProbe(s, log);
        std::fclose(log);
        return failures ? 1 : 0;
    }
    const auto pump_sidebar = [&](auto ready) {
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            TickSidebarRefresh(s, GetTickCount64());
            if (ready()) return true;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(10);
        }
        return false;
    };
    check(pump_sidebar([&] { return !s.sidebar.drives.empty(); }), "sidebar loads asynchronously after worker startup");
    if (!s.sidebar.drives.empty()) {
        s.sidebar.drives.front().detail = L"stale-capacity";
        RequestSidebarRefresh(s);
        check(pump_sidebar([&] { return s.sidebar.drives.front().detail != L"stale-capacity"; }),
              "background capacity request replaces stale display without rebuilding sidebar");
    }
    auto* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || tab->EntryCount() < 3) { std::fclose(log); return 3; }
    tab->ClearSelection();
    SetFocus(s.hwnd);
    SendMessageW(s.hwnd, WM_CHAR, L'a', 0);
    check(tab->selected_index >= 0 && tab->EntryAt(tab->selected_index).name == L"apple.txt",
          "WM_CHAR selects first matching fixture");
    SendMessageW(s.hwnd, WM_CHAR, L'a', 0);
    check(tab->selected_index >= 0 && tab->EntryAt(tab->selected_index).name == L"apricot.txt",
          "WM_CHAR cycles to next matching fixture");
    ToggleQuickPreview(s);
    const auto original = s.quickPreview.item().path;
    check(s.quickPreview.visible() && original.find(L"apricot.txt") != std::wstring::npos,
          "preview opens selected fixture");
    SetFocus(s.hwnd);
    s.listTypeAhead = {};
    SendMessageW(s.hwnd, WM_CHAR, L'b', 0);
    const auto now = GetTickCount64();
    TickQuickPreviewSelection(s, now);
    check(s.quickPreview.item().path == original, "selection update is debounced");
    TickQuickPreviewSelection(s, now + 121);
    check(s.quickPreview.item().path.find(L"banana.txt") != std::wstring::npos,
          "open preview follows the newly selected file");
    ui::QuickPreviewItem internal = s.quickPreview.item();
    internal.path = original;
    internal.name = L"apricot.txt";
    s.quickPreview.Update(internal);
    TickQuickPreviewSelection(s, now + 300);
    check(s.quickPreview.item().path == original, "unchanged list selection preserves preview navigation");
    tab->ClearSelection();
    TickQuickPreviewSelection(s, now + 400);
    TickQuickPreviewSelection(s, now + 521);
    check(s.quickPreview.visible(), "empty selection keeps the preview accessible");
    s.quickPreview.Close();
    std::fclose(log);
    return failures ? 1 : 0;
}
