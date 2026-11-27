#include "host/video/video_glass_retry.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

using pulse::desktop::video::VideoGlassRetry;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) {
        std::printf("[FAIL] %s\n", message);
        std::exit(1);
    }
}

bool NextTimer(HWND hwnd, MSG& msg, DWORD timeout = 1500) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    do {
        if (PeekMessageW(&msg, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE)) return true;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return false;
}

void DrainTimers(HWND hwnd) {
    MSG msg{};
    while (PeekMessageW(&msg, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE)) DispatchMessageW(&msg);
}
}

int main() {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr, "create message-only window");
    unsigned attempts = 0;
    unsigned notices = 0;
    int last_frame = 0;
    bool ready = false;
    auto importer = [&](const int& frame) {
        ++attempts;
        if (ready) last_frame = frame;
        return ready;
    };
    auto notice = [&] { ++notices; };
    VideoGlassRetry<int> retry;
    retry.Initialize(hwnd, importer, notice);
    retry.Submit(1);
    Check(attempts == 1 && notices == 0 && retry.Pending(), "busy import retains pending frame");
    MSG msg{};
    Check(NextTimer(hwnd, msg) && msg.lParam != 0, "busy import schedules real callback timer");
    ready = true;
    DispatchMessageW(&msg);
    Check(attempts == 2 && notices == 1 && last_frame == 1 && !retry.Pending(),
          "timer imports final static frame when texture becomes ready");
    DispatchMessageW(&msg);
    Check(attempts == 2 && notices == 1, "queued duplicate callback does not redraw");
    std::puts("[PASS] real message-only timer: busy -> ready imports once without another video frame");

    ready = false;
    retry.Submit(2);
    retry.Submit(3);
    Check(NextTimer(hwnd, msg), "replacement frame has retry timer");
    ready = true;
    DispatchMessageW(&msg);
    Check(last_frame == 3 && notices == 2 && !retry.Pending(), "newest frame replaces pending retry");
    std::puts("[PASS] newer frame replaces pending frame and successful import emits one notice");

    const unsigned before_stage = attempts;
    retry.Stage(30);
    retry.EnsureTimer();
    Check(!NextTimer(hwnd, msg, 40) && attempts == before_stage && retry.Pending(),
          "first frame before swap callback is retained without importing on old swap");
    retry.RetryNow();
    Check(last_frame == 30 && notices == 3 && !retry.Pending(),
          "swap callback imports retained first static frame");
    std::puts("[PASS] pre-swap static frame waits for swap callback without losing frozen-resume first frame");

    ready = false;
    const unsigned before_exhaustion = attempts;
    retry.Submit(4);
    for (unsigned i = 0; i < 16; ++i) {
        Check(NextTimer(hwnd, msg), "bounded retry timer arrives");
        DispatchMessageW(&msg);
    }
    Check(attempts == before_exhaustion + 17 && !retry.Pending() && last_frame == 30 && notices == 3,
          "initial attempt plus exactly sixteen retries then preserves displayed frame");
    retry.EnsureTimer();
    Check(!NextTimer(hwnd, msg, 60), "heartbeat does not revive exhausted retry budget");
    ready = true;
    retry.Submit(5);
    Check(last_frame == 5 && notices == 4, "new frame can recover after exhaustion");
    std::puts("[PASS] retry exhaustion stops after 16 timer attempts; heartbeat cannot reset budget");

    ready = false;
    retry.Submit(6);
    Check(NextTimer(hwnd, msg), "capture queued timer before freeze/stop clear");
    const unsigned before_clear = attempts;
    retry.Clear();
    DispatchMessageW(&msg);
    Check(attempts == before_clear && !retry.Pending(), "clear prevents queued callback from retrying old frame");
    retry.Close();
    retry.Submit(7);
    Check(attempts == before_clear, "closed retry rejects new frames");
    std::puts("[PASS] freeze/stop clear and close suppress already queued timer callbacks");

    auto owner = std::make_unique<VideoGlassRetry<int>>();
    owner->Initialize(hwnd, importer, notice);
    owner->Submit(8);
    Check(NextTimer(hwnd, msg), "capture queued timer before owner destruction");
    const unsigned before_destroy = attempts;
    owner.reset();
    DispatchMessageW(&msg);
    Check(attempts == before_destroy, "destroyed owner is not accessed by queued TIMERPROC");
    DrainTimers(hwnd);
    DestroyWindow(hwnd);
    std::puts("[PASS] owner destruction removes window property before stale TIMERPROC dispatch");
    return 0;
}
