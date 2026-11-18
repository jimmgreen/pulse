#include "hover_motion_test.h"
#include "app_internal.h"
#include "app_runtime.h"
#include "app_navigation.h"
#include "../ui/ui_renderer.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace pulse {
int RunHoverMotionTest(AppState& s, const std::wstring& report) {
    wchar_t directory[32768]{};
    if (!s.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", directory, ARRAYSIZE(directory))) return 2;
    FILE* log = nullptr;
    if (_wfopen_s(&log, report.c_str(), L"wb") || !log) return 2;
    const auto fixture = std::filesystem::path(directory) / L"hover-fixture";
    std::filesystem::create_directories(fixture);
    std::filesystem::create_directories(fixture / L"00-folder");
    const auto picture = std::filesystem::path(L"bench_data/photo-protocol/02-green.png");
    if (std::filesystem::exists(picture)) std::filesystem::copy_file(picture,fixture / L"01-picture.png",std::filesystem::copy_options::overwrite_existing);
    for (int i = 0; i < 40; ++i) std::ofstream(fixture / (L"row-" + std::to_wstring(i) + L".txt")) << "hover fixture";
    ShowWindow(s.hwnd, SW_SHOWNORMAL);
    NavigateTo(s, fixture.wstring());
    auto pump = [&](DWORD ms, bool trace) {
        const auto until = GetTickCount64() + ms;
        auto frame = s.lastFrameTime;
        while (GetTickCount64() < until) {
            MSG msg{};
            if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                // Synthetic pointer stays in the fixture; the physical cursor
                // must not clear it through TrackMouseEvent's leave message.
                if (msg.hwnd == s.hwnd && (msg.message == WM_MOUSELEAVE || msg.message == WM_MOUSEMOVE)) continue;
                const auto start = std::chrono::steady_clock::now();
                TranslateMessage(&msg); DispatchMessageW(&msg);
                const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
                if (trace && elapsed > 8) std::fprintf(log, "message=%u duration=%.2f\n", msg.message, elapsed);
            } else Sleep(1);
            if (trace && frame != s.lastFrameTime) {
                const auto plate = s.renderer.HoverMotionPositionForTest();
                std::fprintf(log, "frame tick=%llu row=%d top=%.2f render=%.2f gap=%.2f\n", GetTickCount64(), s.hoverRow,plate.top,
                    s.lastFrameMs, std::chrono::duration<double,std::milli>(s.lastFrameTime-frame).count());
                frame = s.lastFrameTime;
            }
        }
    };
    pump(1500, false);
    auto* tab = ActiveTab(s);
    if (!tab || tab->loading || tab->CountBound()<12) { std::fclose(log); return 3; }
    tab->view_mode = ui::ViewMode::Details;
    InvalidateRect(s.hwnd,nullptr,FALSE); pump(250,false);
    for (DWORD dwell : {30u, 120u, 300u}) {
        std::fprintf(log,"trajectory dwell=%lu\n",dwell);
        for (int row=0; row<10; ++row) {
            const auto vm = BuildVm(s);
            const auto& pane = vm.pane_slots.front();
            const auto rect = s.renderer.ItemRectInPane(pane.pane,pane.rect,row);
            const int x = static_cast<int>(rect.left+100*s.scale), y = static_cast<int>((rect.top+rect.bottom)/2);
            const auto hit = s.renderer.HitTest(vm,D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height())),static_cast<float>(x),static_cast<float>(y));
            std::fprintf(log,"move row=%d rect=%.0f,%.0f,%.0f,%.0f hit=%d index=%d\n",row,rect.left,rect.top,rect.right,rect.bottom,static_cast<int>(hit.region),hit.index);
            SendMessageW(s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(x,y));
            pump(dwell,true);
        }
        pump(400,true);
    }
    std::fprintf(log,"trajectory continuous slow 16ms step\n");
    const auto vm = BuildVm(s);
    const auto& pane = vm.pane_slots.front();
    const auto first = s.renderer.ItemRectInPane(pane.pane,pane.rect,0);
    const auto last = s.renderer.ItemRectInPane(pane.pane,pane.rect,9);
    for (int y=static_cast<int>((first.top+first.bottom)/2); y<static_cast<int>((last.top+last.bottom)/2); y+=3) {
        SendMessageW(s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(static_cast<int>(first.left+100*s.scale),y));
        pump(16,true);
    }
    pump(400,true);
    float position = (first.top+first.bottom)*0.5f;
    SendMessageW(s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(static_cast<int>(first.left+100*s.scale),static_cast<int>(position)));
    pump(500,false);
    std::fprintf(log,"trajectory continuous fast-to-slow 16ms step\n");
    for (int step=0; position<(last.top+last.bottom)*0.5f; ++step) {
        const float speed = 2.0f+18.0f*std::exp(-static_cast<float>(step)/12.0f);
        SendMessageW(s.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(static_cast<int>(first.left+100*s.scale),static_cast<int>(position)));
        pump(16,true);
        position += speed;
    }
    pump(400,true);
    std::fclose(log);
    return 0;
}
}
