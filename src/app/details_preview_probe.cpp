#include "app_input.h"
#include "app_commands.h"
#include "app_runtime.h"
#include "group_wheel_ui.h"
#include "../ui/ui_motion.h"
#include <dwmapi.h>
#include "../ui/quick_preview_window.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <cmath>

namespace pulse::ui {
struct DetailsPreviewProbe {
    static uint64_t Generation(QuickPreviewWindow& preview) { return preview.generation_; }
    static void LogState(QuickPreviewWindow& preview, FILE* log) {
        fprintf(log, "[STATE] hwnd=%p visible=%d embedded=%d native_kind=%d markdown_data=%d generation=%llu path=%ls\n",
                static_cast<void*>(preview.hwnd_), preview.visible(), preview.embedded_,
                static_cast<int>(preview.native_kind_), preview.markdown_.HasData(),
                static_cast<unsigned long long>(preview.generation_), preview.item_.path.c_str());
        fflush(log);
    }
    static bool Playing(QuickPreviewWindow& preview) { return preview.video_.active(); }
    static bool Markdown(QuickPreviewWindow& preview) {
        preview.Render();
        return preview.native_kind_ == QuickPreviewWindow::NativeKind::Markdown && preview.markdown_.HasData();
    }
    static D2D1_POINT_2F Scroll(QuickPreviewWindow& preview) {
        return {preview.markdown_.scroll_x_, preview.markdown_.scroll_};
    }
    template<class Check>
    static void ScrollbarInput(QuickPreviewWindow& preview, const Check& check) {
        const auto point = [](float x, float y) { return MAKELPARAM(static_cast<int>(x), static_cast<int>(y)); };
        for (bool horizontal : {false, true}) {
            preview.Render();
            const auto bar = preview.markdown_.Scrollbar(horizontal);
            check(bar.maximum > 0 && bar.travel > 0, horizontal ? "wide Markdown has horizontal scrollbar" :
                  "long Markdown has vertical scrollbar");
            if (bar.maximum <= 0 || bar.travel <= 0) continue;
            const float x = (bar.thumb.left + bar.thumb.right) * 0.5f;
            const float y = (bar.thumb.top + bar.thumb.bottom) * 0.5f;
            const float before = horizontal ? preview.markdown_.scroll_x_ : preview.markdown_.scroll_;
            SendMessageW(preview.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, point(x, y));
            check(preview.markdown_.DraggingScrollbar() && GetCapture() == preview.hwnd_ && !preview.selecting_,
                  "Markdown thumb routes to scrollbar capture, not text selection");
            const float delta = (std::min)(bar.travel * 0.2f, 40.0f * preview.scale_);
            SendMessageW(preview.hwnd_, WM_MOUSEMOVE, MK_LBUTTON,
                         point(x + (horizontal ? delta : 0), y + (horizontal ? 0 : delta)));
            const float after = horizontal ? preview.markdown_.scroll_x_ : preview.markdown_.scroll_;
            check(after > before && preview.sel_anchor_ == preview.sel_focus_ && !preview.selecting_,
                  "HWND thumb drag scrolls content without selecting text");
            SendMessageW(preview.hwnd_, WM_LBUTTONUP, 0,
                         point(x + (horizontal ? delta : 0), y + (horizontal ? 0 : delta)));
            check(!preview.markdown_.DraggingScrollbar() && GetCapture() != preview.hwnd_ &&
                  (horizontal ? preview.markdown_.scroll_x_ : preview.markdown_.scroll_) == after,
                  "button up preserves final scroll and ends Markdown scrollbar capture");
            preview.Render();
            const auto moved = preview.markdown_.Scrollbar(horizontal);
            const float tx = horizontal ? moved.hit.left + 1 : (moved.hit.left + moved.hit.right) * 0.5f;
            const float ty = horizontal ? (moved.hit.top + moved.hit.bottom) * 0.5f : moved.hit.top + 1;
            SendMessageW(preview.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, point(tx, ty));
            check(!preview.markdown_.DraggingScrollbar() && !preview.selecting_,
                  "track click pages instead of beginning thumb or text drag");
            SendMessageW(preview.hwnd_, WM_LBUTTONUP, 0, point(tx, ty));
            check((horizontal ? preview.markdown_.scroll_x_ : preview.markdown_.scroll_) < after && !preview.selecting_,
                  "HWND track click pages Markdown without text selection");
            for (const UINT cancel : {WM_CANCELMODE, WM_CAPTURECHANGED}) {
                preview.Render();
                const auto current = preview.markdown_.Scrollbar(horizontal);
                const auto thumb = point((current.thumb.left + current.thumb.right) * 0.5f,
                                         (current.thumb.top + current.thumb.bottom) * 0.5f);
                SendMessageW(preview.hwnd_, cancel == WM_CAPTURECHANGED ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN,
                             MK_LBUTTON, thumb);
                check(preview.markdown_.DraggingScrollbar() && GetCapture() == preview.hwnd_ &&
                      !preview.selecting_ && preview.sel_anchor_ == preview.sel_focus_,
                      "click or double-click starts thumb drag without text selection");
                if (cancel == WM_CAPTURECHANGED) ReleaseCapture();
                else SendMessageW(preview.hwnd_, cancel, 0, 0);
                check(!preview.markdown_.DraggingScrollbar() && GetCapture() != preview.hwnd_,
                      "cancel or capture loss ends Markdown scrollbar drag");
            }
        }
        preview.markdown_.Scroll(-2.0f);
        preview.markdown_.ScrollHorizontal(-2.0f);
        preview.Render();
        check(preview.markdown_.scroll_ > 0.0f && preview.markdown_.scroll_x_ > 0.0f,
              "flyout preservation baseline has nonzero vertical and horizontal Markdown scroll");
    }
    static VideoPreview::State Media(QuickPreviewWindow& preview) { return preview.video_.Snapshot(); }
    static void SetVolume(QuickPreviewWindow& preview, float value) { preview.SetPlaybackVolume(value); }
    static float Opacity(QuickPreviewWindow& preview) { return preview.PlaybackBarOpacity(); }
    static bool CompactMedia(QuickPreviewWindow& preview) {
        const auto g = preview.PlaybackLayout();
        const auto content = preview.ContentRect();
        return preview.PlaybackHeight() <= 80.1f*preview.scale_ &&
            g.buttons[g.play_index].right-g.buttons[g.play_index].left <= 28.1f*preview.scale_ &&
            content.bottom-content.top > preview.PlaybackHeight()*1.5f &&
            g.volume_track.right > g.volume_track.left &&
            g.audio_tracks.right > g.audio_tracks.left && g.subtitles.right > g.subtitles.left;
    }
    static bool Loaded(QuickPreviewWindow& preview) {
        return preview.native_kind_ != QuickPreviewWindow::NativeKind::None || preview.video_.active();
    }
    static bool Capture(QuickPreviewWindow& preview, const std::wstring& path) {
        preview.Render();
        return preview.compositor_.SaveSnapshot(path.c_str());
    }
    static bool ControlsFit(QuickPreviewWindow& preview) {
        RECT client{}; GetClientRect(preview.hwnd(), &client);
        const auto g = preview.PlaybackLayout();
        for (const auto r : {g.track, g.volume, g.audio_tracks, g.subtitles, g.speed}) {
            if (r.right > r.left && r.bottom > r.top &&
                (r.left < 0 || r.top < 0 || r.right > client.right || r.bottom > client.bottom)) return false;
        }
        return true;
    }
};
}

namespace {

// Capture the desktop composition, including child HWNDs; a compositor-only
// snapshot cannot expose the child-airspace regression this probe exercises.
bool CaptureDetailsScreen(HWND hwnd, const std::filesystem::path& path) {
    if (GetForegroundWindow() != hwnd) return false;
    DwmFlush();
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{};
    ClientToScreen(hwnd, &origin);
    if (client.right <= 0 || client.bottom <= 0) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = client.right;
    info.bmiHeader.biHeight = -client.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const DWORD bytes = static_cast<DWORD>(client.right) * static_cast<DWORD>(client.bottom) * 4;
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    void* pixels = nullptr;
    HBITMAP bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
    bool okay = bitmap && BitBlt(memory, 0, 0, client.right, client.bottom, screen,
                                origin.x, origin.y, SRCCOPY | CAPTUREBLT);
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
    return okay;
}

bool DetailsRegionMatches(HWND parent, HWND preview, const D2D1_RECT_F& cover) {
    RECT window{};
    GetWindowRect(preview, &window);
    RECT bounds{0, 0, window.right - window.left, window.bottom - window.top};
    RECT cut{static_cast<LONG>(std::floor(cover.left)), static_cast<LONG>(std::floor(cover.top)),
             static_cast<LONG>(std::ceil(cover.right)), static_cast<LONG>(std::ceil(cover.bottom))};
    MapWindowPoints(parent, nullptr, reinterpret_cast<POINT*>(&cut), 2);
    OffsetRect(&cut, -window.left, -window.top);
    RECT overlap{};
    if (!IntersectRect(&overlap, &bounds, &cut)) return false;
    HRGN expected = CreateRectRgnIndirect(&bounds);
    HRGN hole = CreateRectRgnIndirect(&cut);
    HRGN actual = CreateRectRgn(0, 0, 0, 0);
    bool okay = expected && hole && actual;
    if (okay) {
        CombineRgn(expected, expected, hole, RGN_DIFF);
        okay = GetWindowRgn(preview, actual) != ERROR && EqualRgn(expected, actual);
    }
    if (expected) DeleteObject(expected);
    if (hole) DeleteObject(hole);
    if (actual) DeleteObject(actual);
    return okay;
}

bool DetailsRegionRestored(HWND preview) {
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    if (!region) return false;
    const bool restored = GetWindowRgn(preview, region) == ERROR;
    DeleteObject(region);
    return restored;
}

} // namespace

int RunDetailsPreviewProbe(pulse::AppState& s, const wchar_t* report) {
    using namespace pulse;
    FILE* log = nullptr; _wfopen_s(&log, report, L"w");
    if (!log || !s.isolatedTest) return 2;
    int failures = 0;
    const auto check = [&](bool okay, const char* name) {
        fprintf(log, "[%s] %s\n", okay ? "PASS" : "FAIL", name); fflush(log);
        failures += !okay;
    };
    const auto pump = [](DWORD ms) {
        const auto end = GetTickCount64() + ms;
        MSG msg{};
        while (GetTickCount64() < end) {
            while (GetTickCount64() < end && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            Sleep(1);
        }
    };
    const bool occlusion_only = GetEnvironmentVariableW(L"PULSE_TEST_DETAILS_PREVIEW_OCCLUSION", nullptr, 0) != 0;
    auto* tab = ActiveTab(s);
    if (!tab || tab->EntryCount() < (occlusion_only ? 1u : 2u)) { fclose(log); return 3; }
    const auto folder = std::filesystem::path(report).parent_path();
    s.showDetailsPanel = true;
    s.renderer.SetDetailsPanelVisible(true);
    s.detailsPreviewEnabled = true;
    ShowWindow(s.hwnd, SW_RESTORE);
    const int dpi = static_cast<int>(96.0f * s.scale);
    SetWindowPos(s.hwnd, nullptr, 50, 50, MulDiv(1200, dpi, 96), MulDiv(800, dpi, 96), SWP_NOZORDER);
    SetForegroundWindow(s.hwnd);
    SetFocus(s.hwnd);
    const auto select = [&](int index) {
        tab->SelectOnly(index);
        InvalidateRect(s.hwnd, nullptr, FALSE); UpdateWindow(s.hwnd);
        SyncDetailsPreview(s); pump(700);
    };
    if (occlusion_only) {
        // Suppress first-run teaching bubbles for this isolated capture only.
        s.teachTip = s.teachCandidate = -1;
        s.teachShownThisSession = true;
        // Keep the 1200-DIP window above the renderer's 1000-DIP details
        // visibility breakpoint. The wide pane still overlaps both flyouts.
        s.renderer.SetDetailsPanelWidth(450.0f);
        int markdown = -1;
        for (int i = 0; i < static_cast<int>(tab->EntryCount()); ++i) {
            if (_wcsicmp(std::filesystem::path(tab->EntryAt(i).name).extension().c_str(), L".md") == 0) {
                markdown = i;
                break;
            }
        }
        check(markdown >= 0, "isolated fixture contains long, wide Markdown");
        if (markdown >= 0) {
            select(markdown);
            auto& preview = s.detailsPreview;
            // Allow a cold helper launch plus bounded transport retries before
            // declaring readiness failed; this is not a UI-fixture fallback.
            const auto deadline = GetTickCount64() + 20000;
            while (!ui::DetailsPreviewProbe::Markdown(preview) && GetTickCount64() < deadline) pump(50);
            const auto bounds = s.renderer.DetailsPreviewBounds();
            fprintf(log, "[STATE] details=%d enabled=%d loading=%d selected=%zu iconic=%d quick_preview=%d bounds=%.1f,%.1f,%.1f,%.1f scale=%.2f\n",
                    s.showDetailsPanel, s.detailsPreviewEnabled, tab->loading,
                    static_cast<size_t>(tab->SelectedCount()), IsIconic(s.hwnd), s.quickPreview.visible(),
                    bounds.left, bounds.top, bounds.right, bounds.bottom, s.scale);
            ui::DetailsPreviewProbe::LogState(preview, log);
            const bool ready = ui::DetailsPreviewProbe::Markdown(preview);
            check(ready, "fixture uses native Markdown child");
            if (!ready) {
                CaptureDetailsScreen(s.hwnd, folder / L"details-native-not-ready.bmp");
                preview.Close();
                fprintf(log, "[STATE] dependent scrollbar/occlusion checks not run: native Markdown unavailable\nfailures=%d\n", failures);
                fclose(log);
                return 1;
            }
            ui::DetailsPreviewProbe::ScrollbarInput(preview, check);
            const auto generation = ui::DetailsPreviewProbe::Generation(preview);
            const auto scroll = ui::DetailsPreviewProbe::Scroll(preview);
            const HWND child = preview.hwnd();
            const auto preserved = [&] {
                const auto now = ui::DetailsPreviewProbe::Scroll(preview);
                return child == preview.hwnd() && preview.visible() &&
                    ui::DetailsPreviewProbe::Generation(preview) == generation &&
                    now.x == scroll.x && now.y == scroll.y;
            };
            SetFocus(s.hwnd);
            for (bool sort : {true, false}) {
                if (sort) OpenSortWheel(s);
                else OpenGroupWheel(s);
                auto& wheel = s.renderer.GroupWheelPicker();
                check(wheel.IsOpen() && DetailsRegionMatches(s.hwnd, child, wheel.OcclusionBounds()),
                      sort ? "sort flyout excludes overlapping child before first frame" :
                             "group flyout excludes overlapping child before first frame");
                pump(350);
                for (int i = 0; i < 30; ++i) SyncDetailsPreview(s);
                check(preserved(), "flyout and repeated sync preserve Markdown scroll, HWND and generation");
                check(CaptureDetailsScreen(s.hwnd, folder / (sort ? L"details-sort-occlusion.bmp" : L"details-group-occlusion.bmp")),
                      "actual desktop composition captured with flyout over child");
                wheel.Close();
                SyncDetailsPreview(s);
                if (wheel.Visible()) {
                    check(DetailsRegionMatches(s.hwnd, child, wheel.OcclusionBounds()),
                          "child stays clipped during closing animation");
                }
                pump(450);
                SyncDetailsPreview(s);
                check(!wheel.Visible() && DetailsRegionRestored(child) && preserved(),
                      "closing restores full child without reload or scroll reset");
            }
            // Explicit animated fixture also exercises close/reopen when the
            // desktop has animations disabled; no user preference is changed.
            auto& wheel = s.renderer.GroupWheelPicker();
            ui::GroupWheelData data;
            data.options.emplace_back();
            RECT client{}; GetClientRect(s.hwnd, &client);
            RECT child_rect{}; GetWindowRect(child, &child_rect);
            MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&child_rect), 2);
            const D2D1_RECT_F anchor{static_cast<float>(child_rect.left), 60.0f * s.scale,
                                   static_cast<float>(child_rect.right), 80.0f * s.scale};
            const D2D1_RECT_F client_bounds{0, 0, static_cast<float>(client.right), static_cast<float>(client.bottom)};
            wheel.Open(data, anchor, client_bounds, s.scale, true);
            SyncDetailsPreview(s);
            wheel.Tick(ui::motion::NowMs() + 300);
            wheel.Close();
            SyncDetailsPreview(s);
            check(!wheel.IsOpen() && wheel.Visible() && DetailsRegionMatches(s.hwnd, child, wheel.OcclusionBounds()),
                  "animated close retains occlusion before its final tick");
            wheel.Open(data, anchor, client_bounds, s.scale, true);
            SyncDetailsPreview(s);
            check(DetailsRegionMatches(s.hwnd, child, wheel.OcclusionBounds()) && preserved(),
                  "reopen during fade keeps child clipped and session unchanged");
            wheel.Close();
            wheel.Tick(ui::motion::NowMs() + 300);
            SyncDetailsPreview(s);
            check(!wheel.Visible() && DetailsRegionRestored(child) && preserved(),
                  "final animation tick restores preview without reloading");
            preview.SetEmbeddedOcclusion({-100, -100, -50, -50});
            check(DetailsRegionRestored(child) && preserved(), "nonoverlapping flyout leaves preview intact");
            InvalidateRect(s.hwnd, nullptr, FALSE); UpdateWindow(s.hwnd); pump(100);
            check(CaptureDetailsScreen(s.hwnd, folder / L"details-occlusion-restored.bmp"),
                  "actual restored desktop composition captured");
        }
        s.detailsPreview.Close();
        fprintf(log, "failures=%d\n", failures); fclose(log);
        return failures ? 1 : 0;
    }
    select(0);
    auto& preview = s.detailsPreview;
    check(preview.visible() && preview.embedded() && GetParent(preview.hwnd()) == s.hwnd &&
          (GetWindowLongPtrW(preview.hwnd(), GWL_STYLE) & WS_CHILD), "sidebar uses child QuickPreviewWindow");
    check(GetFocus() == s.hwnd, "initial sidebar preview does not steal list focus");
    const auto generation = ui::DetailsPreviewProbe::Generation(preview);
    for (int i = 0; i < 30; ++i) SyncDetailsPreview(s);
    check(ui::DetailsPreviewProbe::Generation(preview) == generation, "unchanged selection does not restart preview");
    for (int i = 0; i < static_cast<int>(tab->EntryCount()); ++i) {
        select(i); pump(1800);
        check(preview.item().name == tab->EntryAt(i).name, "selected item identity matches preview");
        check(ui::DetailsPreviewProbe::Loaded(preview), "fixture loads through shared preview engine");
        check(ui::DetailsPreviewProbe::ControlsFit(preview), "narrow sidebar controls fit client");
        if (ui::DetailsPreviewProbe::Playing(preview)) {
            check(ui::DetailsPreviewProbe::CompactMedia(preview), "compact media gives picture priority and retains controls");
            const auto before = ui::DetailsPreviewProbe::Media(preview);
            pump(800);
            const auto after = ui::DetailsPreviewProbe::Media(preview);
            check(after.ready && SUCCEEDED(after.error) && after.position > before.position,
                  "embedded video really plays and advances");
            ui::DetailsPreviewProbe::SetVolume(preview, 0.35f); pump(300);
            check(std::abs(ui::DetailsPreviewProbe::Media(preview).volume - 0.35f) < 0.01f,
                  "sidebar volume reaches shared player");
            SendMessageW(preview.hwnd(), WM_KEYDOWN, VK_SPACE, 0); pump(300);
            check(!ui::DetailsPreviewProbe::Media(preview).playing && preview.visible(),
                  "Space in sidebar pauses without closing");
            SendMessageW(preview.hwnd(), WM_KEYDOWN, VK_SPACE, 0); pump(300);
            SendMessageW(preview.hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
            check(preview.visible() && GetFocus() == s.hwnd, "Escape returns focus without reopen loop");
        }
        const auto path = folder / (L"sidebar-" + std::to_wstring(i) + L".png");
        check(ui::DetailsPreviewProbe::Capture(preview, path.wstring()), "sidebar screenshot saved");
    }
    tab->SelectOnly(0); InvalidateRect(s.hwnd,nullptr,FALSE); UpdateWindow(s.hwnd); SyncDetailsPreview(s);
    tab->SelectOnly(1); InvalidateRect(s.hwnd,nullptr,FALSE); UpdateWindow(s.hwnd); SyncDetailsPreview(s);
    pump(800);
    check(preview.item().name == tab->EntryAt(1).name, "rapid switching keeps latest item");
    tab->SelectAll(); SyncDetailsPreview(s);
    check(!preview.visible() && !ui::DetailsPreviewProbe::Playing(preview), "multiple selection closes and stops preview");
    tab->ClearSelection(); SyncDetailsPreview(s);
    check(!preview.visible(), "empty selection stays closed");
    select(0);
    s.detailsPreviewEnabled = false; SyncDetailsPreview(s);
    check(!preview.visible(), "disabled sidebar preview closes");
    s.detailsPreviewEnabled = true; select(0);
    ToggleQuickPreview(s); pump(600); SyncDetailsPreview(s);
    check(s.quickPreview.visible() && !preview.visible(), "Space preview excludes sidebar playback");
    s.quickPreview.Close(); pump(600); SyncDetailsPreview(s);
    check(preview.visible(), "sidebar resumes after Space preview closes");
    ShowWindow(s.hwnd, SW_MINIMIZE); pump(300); SyncDetailsPreview(s);
    check(!preview.visible() && !ui::DetailsPreviewProbe::Playing(preview), "minimize stops sidebar preview");
    ShowWindow(s.hwnd, SW_RESTORE); select(0);
    s.showDetailsPanel = false; SyncDetailsPreview(s);
    check(!preview.visible(), "hidden details panel closes preview");
    s.showDetailsPanel = true;
    s.renderer.SetDetailsPanelVisible(true);
    s.renderer.SetDetailsPanelWidth(300.0f);
    SetThemeMode(s, 2);
    select(static_cast<int>(tab->EntryCount()) - 1); pump(1500);
    check(ui::DetailsPreviewProbe::ControlsFit(preview), "minimum width dark sidebar controls fit");
    pump(2600);
    check(ui::DetailsPreviewProbe::Opacity(preview) == 1.0f, "sidebar playback controls remain visible when idle");
    check(ui::DetailsPreviewProbe::Capture(preview, (folder / L"sidebar-dark-narrow.png").wstring()),
          "dark narrow sidebar screenshot saved");
    if (GetEnvironmentVariableW(L"PULSE_TEST_DETAILS_PREVIEW_HOLD", nullptr, 0)) {
        fprintf(log, "[STATE] visual inspection ready\n"); fflush(log);
        pump(60000);
    }
    s.detailsPreview.Close();
    fprintf(log, "failures=%d\n", failures); fclose(log);
    return failures ? 1 : 0;
}

