#include "app_input.h"
#include "app_commands.h"
#include "app_runtime.h"
#include "../ui/quick_preview_window.h"
#include "../ui/ui_renderer.h"
#include <cstdio>
#include <filesystem>
#include <cmath>

namespace pulse::ui {
struct DetailsPreviewProbe {
    static uint64_t Generation(QuickPreviewWindow& preview) { return preview.generation_; }
    static bool Playing(QuickPreviewWindow& preview) { return preview.video_.active(); }
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
    auto* tab = ActiveTab(s);
    if (!tab || tab->EntryCount() < 2) { fclose(log); return 3; }
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

