#include "../ui/video_preview.h"
#include <objbase.h>
#include <cstdio>
#include <functional>

using pulse::ui::VideoPreview;
static int failures = 0;
static void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
static void Pump(unsigned ms) {
    const auto end = GetTickCount64() + ms;
    do {
        MSG msg{};
        while (GetTickCount64() < end && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(5);
    } while (GetTickCount64() < end);
}
static bool Until(const std::function<bool()>& condition) {
    const auto end = GetTickCount64() + 10000;
    do { Pump(20); if (condition()) return true; } while (GetTickCount64() < end);
    return false;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"paused media fixture", WS_OVERLAPPEDWINDOW,
        -30000, -30000, 300, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(owner != nullptr, "isolated hidden owner created");
    {
        VideoPreview video;
        for (int i = 1; i < argc; ++i) {
            video.Open(owner, argv[i], false, false);
            video.SetVolume(0.0f);
            Check(Until([&] { return video.Snapshot().ready; }), "media reaches ready");
            const auto before = video.Snapshot();
            Pump(500);
            const auto paused = video.Snapshot();
            Check(SUCCEEDED(paused.error) && !paused.playing && paused.position == before.position,
                "initial or switched media remains paused without advancing");
            video.Play(true);
            Check(Until([&] { const auto s = video.Snapshot(); return s.playing && s.position > paused.position; }),
                "manual play advances media");
            video.Play(false);
            Pump(250);
            const auto stopped = video.Snapshot();
            Pump(350);
            Check(!video.Snapshot().playing && video.Snapshot().position == stopped.position,
                "manual pause stops position");
            video.Play(true);
        }
        video.Reset();
        Pump(300);
    }
    DestroyWindow(owner);
    CoUninitialize();
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
