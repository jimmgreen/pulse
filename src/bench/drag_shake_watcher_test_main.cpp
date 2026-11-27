#include "drag_shake_watcher.h"
#ifdef PULSE_SHELF_BASELINE
#include "double_ctrl.h"
bool pulse::DoubleCtrlWatcher::QuietMoment() { return false; }
#endif
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pulse::app;
namespace {
int failures = 0;
void Check(bool pass, const char* name) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
    if (!pass) ++failures;
}
bool Shake(DragShakeDetector& detector, uint64_t start, int amplitude = 60) {
    bool triggered = false;
    int i = 0;
    for (int x : {0, amplitude, 0, amplitude, 0})
        triggered |= detector.Feed(x, 1.0f, start + 100 * i++);
    return triggered;
}
#ifndef PULSE_SHELF_BASELINE
std::atomic<int> created_windows{0};
LRESULT CALLBACK FailFirstWindow(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE && ++created_windows == 1) return FALSE;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
bool WaitRunning(DragShakeWatcher& watcher) {
    for (int i = 0; i < 100 && !watcher.Running(); ++i) Sleep(5);
    return watcher.Running();
}
bool Registered(USHORT usage) {
    UINT count = 0;
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == UINT(-1)) return false;
    std::vector<RAWINPUTDEVICE> devices(count);
    if (count && GetRegisteredRawInputDevices(devices.data(), &count, sizeof(RAWINPUTDEVICE)) == UINT(-1))
        return false;
    for (const auto& device : devices) if (device.usUsagePage == 1 && device.usUsage == usage) return true;
    return false;
}
#endif
}

int main(int argc, char** argv) {
#ifndef PULSE_SHELF_BASELINE
    if (argc > 1 && std::strcmp(argv[1], "--startup-failure") == 0) {
        // An isolated process-local window class rejects its first creation.
        // This drives Run's actual early return without modifying production
        // code, global desktop state, or the user's window registrations.
        WNDCLASSW cls{};
        cls.lpfnWndProc = FailFirstWindow;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"PulseShelfMouseInput";
        Check(RegisterClassW(&cls) != 0, "startup failure fixture class registered");
        HWND target = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
        DragShakeWatcher watcher;
        watcher.Update(target, WM_APP + 11, false, 1);
        for (int i = 0; i < 100 && !created_windows.load(); ++i) Sleep(5);
        Check(created_windows == 1 && !watcher.Running(), "first listener window creation fails");
        for (int i = 0; i < 100 && !watcher.Running(); ++i) {
            Sleep(5);
            watcher.Update(target, WM_APP + 11, false, 1);
        }
        Check(watcher.Running() && created_windows == 2, "same target Update restarts exited listener");
        watcher.Stop();
        Check(!watcher.Running() && !Registered(2), "retried listener stops and removes mouse registration");
        DestroyWindow(target);
        std::printf("RESULT failures=%d\n", failures);
        return failures ? 1 : 0;
    }
#else
    (void)argc;
    (void)argv;
#endif
    DragShakeDetector shake;
    Check(Shake(shake, 100), "medium shake recognizes three reversals");
    Check(!Shake(shake, 600), "shake cooldown rejects retrigger");
    Check(Shake(shake, 2000), "shake re-arms after cooldown");
    DragShakeDetector small_motion;
    Check(!Shake(small_motion, 100, 5), "small cursor movement does not shake");
    DragShakeDetector low;
    low.SetSensitivity(0);
    Check(!Shake(low, 100), "low sensitivity needs fourth reversal");
    Check(low.Feed(60, 1.0f, 600), "low sensitivity fourth reversal triggers");
#ifndef PULSE_SHELF_BASELINE
    DragShortcutDetector shortcut;
    shortcut.Key(VK_LCONTROL, true);
    shortcut.Key(VK_LSHIFT, true);
    Check(shortcut.Key('S', true), "Ctrl Shift then S edge triggers");
    Check(!shortcut.Key('S', true), "S auto-repeat does not retrigger");
    Check(!shortcut.Key('S', false), "S release does not trigger");
    Check(shortcut.Key('S', true), "second S press re-arms without modifier release");
    shortcut.Reset();
    Check(!shortcut.Key('S', true), "S before modifiers does not trigger");
    Check(!shortcut.Key(VK_LCONTROL, true) && !shortcut.Key(VK_LSHIFT, true) &&
          !shortcut.Key('S', true), "later modifiers and S repeat do not fabricate an edge");
    shortcut.Key('S', false);
    Check(shortcut.Key('S', true), "new S edge after modifiers triggers");
    shortcut.Reset();
    shortcut.Seed(VK_LCONTROL, true);
    shortcut.Seed(VK_LSHIFT, true);
    shortcut.Seed('S', true);
    Check(!shortcut.Key('S', true), "S already held when listener attaches stays suppressed");
    shortcut.Key('S', false);
    shortcut.Key(VK_RCONTROL, true);
    shortcut.Key(VK_RSHIFT, true);
    shortcut.Key(VK_LCONTROL, false);
    shortcut.Key(VK_LSHIFT, false);
    Check(shortcut.Key('S', true), "releasing left modifiers preserves held right modifiers");
    shortcut.Key('S', false);
    shortcut.Key(VK_RMENU, true);
    Check(!shortcut.Key('S', true), "Alt chord does not summon shelf");
    shortcut.Key(VK_RMENU, false);
    Check(!shortcut.Key('S', true), "Alt release does not fabricate S edge");
    shortcut.Key('S', false);
    Check(shortcut.Key('S', true), "fresh edge after Alt release works");
    shortcut.Reset();
    shortcut.Key(VK_LCONTROL, true);
    shortcut.Key(VK_RSHIFT, true);
    const bool short_press = shortcut.Key('S', true);
    shortcut.Key('S', false);
    shortcut.Key(VK_LCONTROL, false);
    shortcut.Key(VK_RSHIFT, false);
    Check(short_press, "sub-poll-interval S press retains event-time modifier snapshot");
    Check(!shortcut.Key(256, true), "out-of-range key is ignored");
    SetShelfOwnDragActive(true);
    SetInternalFileDragActive(true);
    Check(!IsSystemOleDragActive(), "shelf-owned drag suppresses even internal drag flag");
    SetShelfOwnDragActive(false);
    SetInternalFileDragActive(false);

    // These are real message-only windows and a real Raw Input registration,
    // with no injected input, desktop focus changes, or user configuration.
    HWND first = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    HWND second = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Check(first && second, "isolated listener targets created");
    DragShakeWatcher watcher;
    watcher.Update(first, WM_APP + 11, false, 1);
    Check(WaitRunning(watcher), "shake disabled still runs independent shortcut listener");
    Check(Registered(2), "shelf owns mouse Raw Input registration");
    Check(!Registered(6), "shelf creates no keyboard Raw Input registration");
    watcher.Update(second, WM_APP + 11, true, 2);
    DestroyWindow(first);
    Check(WaitRunning(watcher), "target switch drains old listener and starts new target");
    watcher.Stop();
    Check(!watcher.Running() && !Registered(2) && !Registered(6), "stop joins and removes registration");
    bool restarted = true;
    for (int i = 0; i < 20; ++i) {
        watcher.Update(second, WM_APP + 11, (i % 2) != 0, i % 3);
        if (i % 2) restarted &= WaitRunning(watcher);
        watcher.Stop(); // includes stop immediately after startup, before message queue exists
        restarted &= !watcher.Running() && !Registered(2);
    }
    Check(restarted, "twenty start-stop cycles including immediate stop leave no registration");
    watcher.Update(second, WM_APP + 11, false, 1);
    Check(WaitRunning(watcher), "listener restarts after stop");
    watcher.Update(nullptr, 0, false, 1);
    Check(!watcher.Running() && !Registered(2), "null target stops listener");
    DestroyWindow(second);
#endif
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

