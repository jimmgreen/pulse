#include "app_input.h"
#include "app_list_selection.h"
#include "app_runtime.h"
#include "listing_selection_restore.h"
#include "partial_redraw.h"
#include "../ui/ui_renderer.h"
#include "../ui/view_layout.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include <utility>

using namespace pulse;

int RunSelectionRefreshProbe(AppState& s, FILE* log) {
    auto* tab = ActiveTab(s);
    if (!s.isolatedTest || !tab || !log || tab->loading || tab->EntryCount() < 2500) return 2;
    // Keep the actual loaded fixture intact. No navigation or enumeration is started.
    struct Restore {
        AppState& state;
        app::Tab& tab;
        app::Tab saved;
        bool single_open;
        bool shot_active;
        Restore(AppState& state, app::Tab& tab) : state(state), tab(tab), saved(std::move(tab)),
            single_open(state.appPrefs.single_click_open), shot_active(state.shot.active) { tab = app::Tab{}; }
        ~Restore() {
            CancelListSelection(state);
            tab = std::move(saved);
            state.appPrefs.single_click_open = single_open;
            state.shot.active = shot_active;
            InvalidateRect(state.hwnd, nullptr, FALSE);
        }
    } restore(s, *tab);
    s.appPrefs.single_click_open = false;
    s.shot.active = false;
    ShowWindow(s.hwnd, SW_SHOWMAXIMIZED);
    const bool pixels_only = GetEnvironmentVariableW(L"PULSE_TEST_SELECTION_PIXELS", nullptr, 0) != 0;
    wchar_t pixel_case_text[8]{};
    GetEnvironmentVariableW(L"PULSE_TEST_SELECTION_PIXEL_CASE", pixel_case_text, ARRAYSIZE(pixel_case_text));
    const int pixel_case = std::clamp(_wtoi(pixel_case_text), 0, 4);
    if (pixels_only) {
        // This synchronous probe intentionally does not dispatch background
        // directory replies into its synthetic tab. Sample our painted window,
        // not Windows' hung-window surrogate during the calibration loop.
        DisableProcessWindowsGhosting();
        SetWindowPos(s.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
    UpdateWindow(s.hwnd);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(2500);
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    for (int i = 0; i < 2500; ++i) {
        auto& entry = (*entries)[i];
        entry.name = L"duplicate.txt";
        entry.full_path = L"C:\\pulse-fixture-only\\duplicate.txt";
        entry.recycle_path = L"C:\\pulse-fixture-only\\$R" + std::to_wstring(i);
        entry.attrs = FILE_ATTRIBUTE_NORMAL;
        entry.mtime = now;
    }
    tab->current_path = tab->snapshot_path = L"pulse:recycle";
    tab->view_mode = ui::ViewMode::Details;
    tab->group_by = 2;
    tab->SetSnapshot(entries);
    tab->snapshot_path = tab->current_path;
    int failures = 0;
    const auto check = [&](bool okay, const char* label) {
        fprintf(log, "[%s] %s\n", okay ? "PASS" : "FAIL", label);
        fflush(log);
        failures += !okay;
    };
    const auto keys = [&] {
        std::set<std::wstring> result;
        for (int index : tab->SelectedIndices()) result.insert(app::ListingSelectionKey(tab->EntryAt(index)));
        return result;
    };
    const auto mouse = [&](UINT message, WPARAM flags, POINT point) {
        SendMessageW(s.hwnd, message, flags, MAKELPARAM(point.x, point.y));
    };
    for (int scenario = pixels_only ? pixel_case : 0; scenario < (pixels_only ? pixel_case + 1 : 4); ++scenario) {
        CancelListSelection(s);
        tab->ClearSelection();
        tab->scroll_y = 0;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        UpdateWindow(s.hwnd);
        const auto vm = BuildVm(s);
        const ui::PaneViewModel* pane = nullptr;
        D2D1_RECT_F bounds{};
        for (const auto& slot : vm.pane_slots) if (slot.focused) { pane = &slot.pane; bounds = slot.rect; break; }
        if (!pane) { fprintf(log, "[ERROR] no focused pane slot\n"); fflush(log); return 2; }
        const auto list = s.renderer.PaneListRect(*pane, bounds);
        ui::ViewLayout layout(pane->view_mode, list, pane->EntryCount(), pane->scroll_x,
            pane->scroll_y, s.scale, s.renderer.ListRowHeightDip(*pane, list), pane->Groups());
        std::vector<POINT> points;
        std::vector<int> sources;
        const auto range = layout.VisibleRange();
        for (int row = range.first; row <= range.second && row < static_cast<int>(pane->EntryCount()); ++row) {
            const auto rect = layout.ItemRect(row);
            if (rect.top < list.top || rect.bottom > list.bottom) continue;
            const int y = static_cast<int>((rect.top + rect.bottom) / 2);
            const int source = pane->SourceIndex(row);
            for (int x = static_cast<int>(list.right - 40 * s.scale); x > list.left; --x) {
                const auto hit = s.renderer.HitTest(vm,
                    {0, 0, float(s.compositor.Width()), float(s.compositor.Height())}, float(x), float(y));
                if (hit.region == ui::HitTestResult::Row && hit.index == source &&
                    !s.renderer.PointInItemGrip(*pane, bounds, source, float(x), float(y))) {
                    points.push_back({x, y}); sources.push_back(source); break;
                }
            }
        }
        fprintf(log, "[LAYOUT] scenario=%d window=%ux%u scale=%.2f groups=%zu visible_range=%d..%d usable_rows=%zu entries=%zu list=(%.1f,%.1f,%.1f,%.1f) slot=(%.1f,%.1f,%.1f,%.1f)\n",
            scenario, s.compositor.Width(), s.compositor.Height(), s.scale, pane->Groups() ? pane->Groups()->size() : 0,
            range.first, range.second, points.size(), pane->EntryCount(), list.left, list.top, list.right, list.bottom,
            bounds.left, bounds.top, bounds.right, bounds.bottom);
        fflush(log);
        if (points.size() < 10 || !pane->Groups() || pane->Groups()->empty()) {
            fprintf(log, "[ERROR] grouped fixture requires ten fully visible rows\n"); fflush(log); return 2;
        }
        mouse(WM_LBUTTONDOWN, MK_LBUTTON, points[3]);
        mouse(WM_MOUSEMOVE, MK_LBUTTON, points[6]);
        UpdateMarqueeFrame(s);
        mouse(WM_LBUTTONUP, 0, points[6]);
        if (pixels_only) {
            // Settle only renderer motion, without dispatching directory results
            // into the synthetic snapshot. Then exercise a hover-only damage
            // notification arriving after a semantic selection change.
            for (int frame = 0; frame < 80; ++frame) {
                s.renderer.TickMotion(GetTickCount64());
                InvalidateRect(s.hwnd, nullptr, FALSE);
                UpdateWindow(s.hwnd);
                Sleep(20);
            }
            const auto samples = [&] {
                const auto dwm = LoadLibraryW(L"dwmapi.dll");
                if (dwm) {
                    const auto flush = reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(dwm, "DwmFlush"));
                    if (flush) flush();
                    FreeLibrary(dwm);
                }
                std::vector<COLORREF> colors;
                HDC screen = GetDC(nullptr);
                for (const POINT row : points) {
                    POINT sample{static_cast<LONG>(list.left + 320 * s.scale), row.y};
                    ClientToScreen(s.hwnd, &sample);
                    colors.push_back(GetPixel(screen, sample.x, sample.y));
                }
                ReleaseDC(nullptr, screen);
                return colors;
            };
            const auto multiple_pixels = samples();
            fprintf(log, "[PIXEL] stable_partial_safe=%d\n", s.renderer.PartialRedrawSafe(BuildVm(s)));
            if (scenario == 4) {
                POINT name = points[6];
                bool found = false;
                for (int x = static_cast<int>(list.left); x < list.right; ++x) {
                    if (s.renderer.PointInItemName(*pane, bounds, sources[6], static_cast<float>(x), static_cast<float>(name.y))) {
                        name.x = x; found = true; break;
                    }
                }
                if (!found) { fprintf(log, "[ERROR] filename point not found\n"); return 2; }
                mouse(WM_MOUSEMOVE, 0, name);
                UpdateWindow(s.hwnd);
                mouse(WM_LBUTTONDOWN, MK_LBUTTON, name);
                UpdateWindow(s.hwnd);
                check(s.listSelection.PendingFile(), "filename press arms deferred single selection");
                mouse(WM_MOUSEMOVE, 0, name);
                check(tab->SelectedCount() == 1 && tab->IsSelected(sources[6]), "no-button move commits exact single selection");
            } else if (scenario == 2) tab->ClearSelection();
            else if (scenario == 3) tab->SelectIndices({sources[0], sources[1], sources[8], sources[9]});
            else tab->SelectOnly(sources[scenario == 1 ? 1 : 6]);
            const auto single_vm = BuildVm(s);
            fprintf(log, "[PIXEL] changed_partial_safe=%d count=%zu\n",
                s.renderer.PartialRedrawSafe(single_vm), static_cast<size_t>(tab->SelectedCount()));
            RECT damage{0, points[6].y - 20, s.compositor.Width(), s.compositor.Height()};
            if (scenario != 4) app::InvalidatePartial(s.hwnd, damage);
            UpdateWindow(s.hwnd);
            const auto incremental_pixels = samples();
            InvalidateRect(s.hwnd, nullptr, FALSE);
            UpdateWindow(s.hwnd);
            const auto full_pixels = samples();
            for (int row = 3; row < 6; ++row) {
                fprintf(log, "[PIXEL] row=%d multi=%08lx incremental=%08lx full=%08lx\n",
                    row, multiple_pixels[row], incremental_pixels[row], full_pixels[row]);
                check(multiple_pixels[row] != full_pixels[row], "pixel sample distinguishes selected from deselected row");
                check(incremental_pixels[row] == full_pixels[row], "first displayed pixels clear every previous selection highlight");
            }
            continue;
        }
        check(tab->SelectedCount() > 1 && tab->SelectedCount() < points.size() &&
            tab->SelectedCount() < tab->EntryCount() && !tab->all_selected,
            "real input creates strictly partial visible grouped selection");
        app::CapturePendingListingSelection(*tab);
        const int target = scenario == 1 ? 1 : scenario == 3 ? 5 : 4;
        if (scenario == 2) SendMessageW(s.hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        else {
            mouse(WM_LBUTTONDOWN, MK_LBUTTON, points[target]);
            mouse(WM_LBUTTONUP, 0, points[target]);
        }
        const auto wanted = keys();
        check(scenario == 2 ? wanted.empty() : wanted.size() == 1 && tab->IsSelected(sources[target]),
            "latest user input commits clear or exact single identity before refresh");
        auto pending = app::TakeListingSelection(*tab, tab->current_path);
        auto reordered = std::make_shared<std::vector<fs::DirEntry>>(*tab->snapshot);
        std::rotate(reordered->begin(), reordered->begin() + 1, reordered->end());
        tab->SetSnapshot(reordered);
        tab->snapshot_path = tab->current_path;
        app::RestoreListingSelection(*tab, pending, s.pane, &s.places);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        UpdateWindow(s.hwnd);
        const auto first = BuildVm(s);
        std::set<std::wstring> displayed;
        for (const auto& slot : first.pane_slots) {
            if (!slot.focused) continue;
            for (int row = 0; row < static_cast<int>(slot.pane.EntryCount()); ++row) {
                const int source = slot.pane.SourceIndex(row);
                if (slot.pane.IsRowSelected(source)) displayed.insert(app::ListingSelectionKey(tab->EntryAt(source)));
            }
        }
        check(keys() == wanted && displayed == wanted && tab->SelectedCount() == wanted.size(),
            "refresh and first frame preserve latest stable recycle identity without duplicate-name selection");
        fprintf(log, "[STATE] scenario=%d user_changed=%d selected_count=%zu first_frame_count=%zu all=%d\n",
            scenario, pending.user_changed, static_cast<size_t>(tab->SelectedCount()), displayed.size(), tab->all_selected);
        fflush(log);
    }
    return failures ? 1 : 0;
}
