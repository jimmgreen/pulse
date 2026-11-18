#include <windows.h>
#include <atomic>
#include <cstdio>
#ifdef PULSE_WIN_E_STARTUP_BASELINE
#include "../../bench_data/shell_actions_fix/original/src/app/win_e_hotkey.h"
#else
#include "../app/win_e_hotkey.h"
#endif
#include "../app/win_e_trace.h"
#ifdef PULSE_WIN_E_STARTUP_BASELINE
#include "../../bench_data/shell_actions_fix/original/src/app/win_e_broker_client.h"
#else
#include "../app/win_e_broker_client.h"
#endif
#include "../ipc/elevated_transfer_transport.h"
#include "../common/runtime_log.h"

static std::atomic<unsigned> hook_installs{0}, hook_removes{0}, broker_starts{0};
static std::atomic<bool> loop_finished{false};
static HHOOK WINAPI TestSetWindowsHookExW(int kind, HOOKPROC, HINSTANCE, DWORD) {
    if (kind == WH_KEYBOARD_LL) ++hook_installs;
    return reinterpret_cast<HHOOK>(1);
}
static BOOL WINAPI TestUnhookWindowsHookEx(HHOOK) { ++hook_removes; return TRUE; }
static HWINEVENTHOOK WINAPI TestSetWinEventHook(DWORD, DWORD, HMODULE, WINEVENTPROC, DWORD, DWORD, DWORD) { return nullptr; }
static UINT_PTR WINAPI TestSetTimer(HWND, UINT_PTR, UINT, TIMERPROC) { return 0; }
static BOOL WINAPI TestGetMessageW(LPMSG, HWND, UINT, UINT) { loop_finished = true; return FALSE; }
static SHORT WINAPI TestGetAsyncKeyState(int) { return 0; }
namespace pulse::app {
class OpenRequestPipe {};
SingleInstanceCoordinator::SingleInstanceCoordinator() = default;
SingleInstanceCoordinator::~SingleInstanceCoordinator() = default;
bool SingleInstanceCoordinator::TrySubmitKeyboardOpen(OpenRequest&, uint64_t, HWND, UINT, WakePoster) noexcept { return true; }
WinETrace::WinETrace() = default;
WinETrace::~WinETrace() = default;
void WinETrace::Push(const WinETraceRecord&) noexcept {}
// A startup request for the real broker necessarily runs its runas launcher.
// Replace that boundary so the regression can observe the request without UAC.
WinEBrokerClient::WinEBrokerClient(HWND) { ++broker_starts; }
WinEBrokerClient::~WinEBrokerClient() = default;
void WinEBrokerClient::Poll(SingleInstanceCoordinator&, HWND, UINT, bool) {}
bool WinEBrokerClient::Active() const noexcept { return false; }
unsigned WinEBrokerClient::HeldFlags() const noexcept { return 0; }
}
namespace pulse::elevated { bool IsElevated(HANDLE) { return false; } }
namespace pulse::diagnostics::runtime { void Event(const char*, std::initializer_list<Field>, Level) noexcept {} }
#define SetWindowsHookExW TestSetWindowsHookExW
#define UnhookWindowsHookEx TestUnhookWindowsHookEx
#define SetWinEventHook TestSetWinEventHook
#define SetTimer TestSetTimer
#define GetMessageW TestGetMessageW
#define GetAsyncKeyState TestGetAsyncKeyState
#ifdef PULSE_WIN_E_STARTUP_BASELINE
#include "../../bench_data/shell_actions_fix/original/src/app/win_e_hotkey.cpp"
#else
#include "../app/win_e_hotkey.cpp"
#endif
int main() {
    using namespace pulse::app;
    SingleInstanceCoordinator coordinator;
    {
#ifdef PULSE_WIN_E_STARTUP_BASELINE
        WinEHotkey hotkey(coordinator, nullptr, WM_APP, true);
#else
        WinEHotkey hotkey(coordinator, nullptr, WM_APP);
#endif
        for (int attempt = 0; attempt < 200 && !loop_finished; ++attempt) Sleep(5);
        hotkey.Stop();
    }
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); failures += !ok;
    };
    check(loop_finished && hook_installs == 1 && hook_removes == 1, "ordinary startup installs and cleans up the Win+E keyboard hook");
    check(broker_starts == 0, "ordinary medium-integrity startup never requests the runas broker");
    WinEInputState input;
    input.ObserveModifier(VK_LWIN, true);
    check(input.chord.Press(input.Eligible(1)) == WinEChord::Action::Submit, "ordinary Win+E still submits an open intent");
    input.chord.accepted = true;
    check(input.chord.Press(input.Eligible(1)) == WinEChord::Action::Swallow && input.chord.Release(), "accepted repeats and release retain ordinary chord ownership");
    printf("hook_installs=%u hook_removes=%u automatic_broker_requests=%u\n", hook_installs.load(), hook_removes.load(), broker_starts.load());
    return failures ? 1 : 0;
}
