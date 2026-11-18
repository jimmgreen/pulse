#include "../common/picker_owner_guard.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <string>
#include <sddl.h>

namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* text) {
    ++checks;
    if (!value) ++failures;
    std::printf("[%s] %s\n", value ? "PASS" : "FAIL", text);
    std::fflush(stdout);
}
constexpr UINT Block = WM_APP + 71;
struct Owner {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE destroyed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HWND> window{nullptr};
    std::atomic<DWORD> tid{0}, callback_tid{0};
    std::atomic<int> block_enable{-1};
    std::atomic<int> disables{0}, enables{0};
    std::thread worker;
    explicit Owner(bool enabled = true) : worker([this, enabled] {
        tid = GetCurrentThreadId();
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Pulse.OwnerGuard.Regression";
        RegisterClassW(&wc);
        window = CreateWindowExW(0, wc.lpszClassName, L"Owned regression fixture",
            WS_OVERLAPPED | (enabled ? 0 : WS_DISABLED), 0, 0, 100, 100,
            nullptr, nullptr, wc.hInstance, this);
        SetEvent(ready);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        // WM_NCDESTROY is delivered before the HWND becomes invalid. Signal
        // only after DispatchMessage returned and the posted quit was removed.
        SetEvent(destroyed);
    }) { WaitForSingleObject(ready, 5000); }
    ~Owner() {
        SetEvent(resume);
        if (IsWindow(window.load())) PostMessageW(window.load(), WM_CLOSE, 0, 0);
        else PostThreadMessageW(tid.load(), WM_QUIT, 0, 0);
        worker.join();
        CloseHandle(ready); CloseHandle(entered); CloseHandle(resume); CloseHandle(destroyed);
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Owner*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Owner*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_ENABLE) {
            self->callback_tid = GetCurrentThreadId();
            if (wparam) ++self->enables; else ++self->disables;
        }
        if (self && (message == Block || (message == WM_ENABLE && self->block_enable.load() == (wparam ? 1 : 0)))) {
            SetEvent(self->entered);
            WaitForSingleObject(self->resume, 10000);
        }
        if (self && message == WM_NCDESTROY) PostQuitMessage(0);
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};
bool WaitRestored(HWND window) {
    const ULONGLONG deadline = GetTickCount64() + 4000;
    while (GetTickCount64() < deadline) {
        if (IsWindowEnabled(window) && !GetPropW(window, L"Pulse.PickerClient.OwnerGuard")) return true;
        Sleep(10);
    }
    return false;
}
void Responsive() {
    Owner owner;
    Check(owner.window != nullptr, "create independently pumped same-process owner");
    {
        pulse::PickerOwnerGuard guard(owner.window);
        Check(guard.Error() == ERROR_SUCCESS, "enabled cross-thread owner is supported (old implementation rejects)");
        Check(guard.Alive() && !IsWindowEnabled(owner.window), "owner disabled before picker can start");
        Check(owner.callback_tid == owner.tid && owner.disables == 1, "disable notification executes exactly once on owner thread");
        pulse::PickerOwnerGuard competing(owner.window);
        Check(competing.Error() == ERROR_BUSY, "second guard cannot acquire the same owner");
        HWND dialog = CreateWindowExW(0, L"STATIC", L"owned picker fixture", WS_POPUP,
            0, 0, 10, 10, owner.window, nullptr, GetModuleHandleW(nullptr), nullptr);
        guard.RestoreBeforeDialogHide(dialog);
        Check(IsWindowEnabled(owner.window) && guard.Alive(), "restored owner remains alive for accepted-result check");
        DestroyWindow(dialog);
    }
    Check(WaitRestored(owner.window) && owner.enables == 1, "scope exit removes identity without double restoration");
    Check(owner.callback_tid == owner.tid, "restore notification executes on owner thread");
}
void AlreadyDisabled() {
    Owner owner(false);
    {
        pulse::PickerOwnerGuard guard(owner.window);
        Check(guard.Error() == ERROR_SUCCESS && guard.Alive(), "caller-disabled cross-thread owner remains supported");
    }
    Check(!IsWindowEnabled(owner.window) && owner.enables == 0, "caller-disabled owner is never enabled by guard");
}
void Timeout(bool in_notification) {
    Owner owner;
    if (in_notification) owner.block_enable = 0;
    else {
        PostMessageW(owner.window, Block, 0, 0);
        Check(WaitForSingleObject(owner.entered, 2000) == WAIT_OBJECT_0, "owner intentionally stops pumping");
    }
    const ULONGLONG before = GetTickCount64();
    {
        pulse::PickerOwnerGuard guard(owner.window);
        Check(guard.Error() == ERROR_TIMEOUT, "unresponsive acquisition fails with bounded timeout");
        Check(GetTickCount64() - before < 4500, "acquisition does not wait indefinitely for foreign thread");
    }
    Check(GetPropW(owner.window, L"Pulse.PickerClient.OwnerGuard") != nullptr, "timed-out request retains identity until recovery");
    SetEvent(owner.resume);
    Check(WaitRestored(owner.window), "late owner recovery restores and releases failed acquisition");
    Check(in_notification ? owner.disables == 1 && owner.enables == 1 : owner.disables == 0,
        "late request never leaves an owner disabled or applies canceled disable");
}
void RestoreTimeout() {
    Owner owner;
    auto guard = std::make_unique<pulse::PickerOwnerGuard>(owner.window);
    Check(guard->Error() == ERROR_SUCCESS, "acquire owner for restoration timeout");
    owner.block_enable = 1;
    const ULONGLONG before = GetTickCount64();
    guard.reset();
    Check(GetTickCount64() - before < 4500, "blocked restore notification does not block destructor indefinitely");
    SetEvent(owner.resume);
    Check(WaitRestored(owner.window) && owner.enables == 1, "late restore completes without touching freed guard");
}
void Destroyed() {
    Owner owner;
    pulse::PickerOwnerGuard guard(owner.window);
    Check(guard.Error() == ERROR_SUCCESS, "acquire owner before its destruction");
    PostMessageW(owner.window, WM_CLOSE, 0, 0);
    Check(WaitForSingleObject(owner.destroyed, 2000) == WAIT_OBJECT_0 && !guard.Alive(), "destroyed owner invalidates guard");
}
void AttachedQueues() {
    Owner owner;
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    const bool attached = AttachThreadInput(GetCurrentThreadId(), owner.tid, TRUE) != FALSE;
    Check(attached, "attach test input queues");
    if (attached) {
        {
            pulse::PickerOwnerGuard guard(owner.window);
            Check(guard.Error() == ERROR_SUCCESS && guard.Alive(), "posted transition supports attached input queues");
        }
        Check(WaitRestored(owner.window), "attached input queue restoration completes");
        AttachThreadInput(GetCurrentThreadId(), owner.tid, FALSE);
    }
}

struct FocusOwner {
    HWND window = nullptr, edit = nullptr, other = nullptr;
    bool switch_on_enable = false, switch_on_activate = false;
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<FocusOwner*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<FocusOwner*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_ENABLE && wparam && self->switch_on_enable)
            SetForegroundWindow(self->other);
        if (self && message == WM_ACTIVATE && LOWORD(wparam) != WA_INACTIVE && self->edit) {
            if (self->switch_on_activate) SetForegroundWindow(self->other);
            else SetFocus(self->edit);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    FocusOwner() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"Pulse.OwnerGuard.FocusRegression";
        RegisterClassW(&wc);
        WNDCLASSW plain{};
        plain.lpfnWndProc = DefWindowProcW; plain.hInstance = wc.hInstance;
        plain.lpszClassName = L"Pulse.OwnerGuard.FocusPopup";
        RegisterClassW(&plain);
        window = CreateWindowW(wc.lpszClassName, L"Pulse isolated owner focus test", WS_OVERLAPPEDWINDOW,
            80, 80, 320, 180, nullptr, nullptr, wc.hInstance, this);
        edit = CreateWindowW(L"EDIT", L"Host-selected keyboard focus", WS_CHILD | WS_VISIBLE,
            8, 8, 250, 24, window, nullptr, wc.hInstance, nullptr);
        other = CreateWindowW(plain.lpszClassName, L"Pulse isolated other window", WS_OVERLAPPEDWINDOW,
            420, 80, 320, 180, nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(window, SW_SHOWNOACTIVATE); ShowWindow(other, SW_SHOWNOACTIVATE);
    }
    ~FocusOwner() { DestroyWindow(other); DestroyWindow(window); }
};

// The host's activation callback is the deterministic
// reentrancy point; no timing sleeps or synthetic focus API stubs are used.
void FocusRestoration(bool legacy_tail = false) {
    FocusOwner owner;
    Check(owner.window && owner.edit && owner.other, "create isolated owner focus windows");
    const HWND previous_foreground = GetForegroundWindow();
    const DWORD foreground_thread = GetWindowThreadProcessId(previous_foreground, nullptr);
    const DWORD fixture_thread = GetCurrentThreadId();
    const bool attached = foreground_thread && foreground_thread != fixture_thread &&
        AttachThreadInput(fixture_thread, foreground_thread, TRUE) != FALSE;
    SetForegroundWindow(owner.window);
    if (attached) AttachThreadInput(fixture_thread, foreground_thread, FALSE);
    const bool ready = GetForegroundWindow() == owner.window;
    Check(ready, "fixture obtains foreground before running focus assertions");
    if (!ready) return;
    for (const char* action : {"accept preserves host-selected child focus", "cancel preserves host-selected child focus",
                              "close preserves host-selected child focus"}) {
        SetForegroundWindow(owner.window);
        pulse::PickerOwnerGuard guard(owner.window);
        HWND dialog = CreateWindowW(L"Pulse.OwnerGuard.FocusPopup", L"Pulse isolated picker close", WS_POPUP | WS_CAPTION,
            100, 100, 280, 120, owner.window, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(dialog, SW_SHOW); SetForegroundWindow(dialog);
        std::printf("focus fixture: guard=%lu foreground=%p dialog=%p active=%p enabled=%d\n",
            guard.Error(), static_cast<void*>(GetForegroundWindow()), static_cast<void*>(dialog),
            static_cast<void*>(GetActiveWindow()), IsWindowEnabled(dialog));
        Check(!guard.Error() && GetForegroundWindow() == dialog, "picker is foreground before restoration");
        guard.RestoreBeforeDialogHide(dialog);
        // Negative control: replay precisely the removed post-activation tail.
        // This must fail the same child-focus assertions without changing the
        // production guard or relaxing the expected result.
        if (legacy_tail && guard.Alive() && IsWindowEnabled(owner.window) && GetActiveWindow() == owner.window)
            SetFocus(owner.window);
        Check(IsWindowEnabled(owner.window) && GetForegroundWindow() == owner.window && GetFocus() == owner.edit, action);
        ShowWindow(dialog, SW_HIDE); DestroyWindow(dialog);
        Check(GetForegroundWindow() == owner.window && GetFocus() == owner.edit, "hide/destroy preserves restored host child focus");
    }
    for (int timing = 0; timing < 3; ++timing) {
        SetForegroundWindow(owner.window);
        pulse::PickerOwnerGuard guard(owner.window);
        HWND dialog = CreateWindowW(L"Pulse.OwnerGuard.FocusPopup", L"Pulse isolated picker switch", WS_POPUP | WS_CAPTION,
            100, 100, 280, 120, owner.window, nullptr, GetModuleHandleW(nullptr), nullptr);
        ShowWindow(dialog, SW_SHOW); SetForegroundWindow(dialog);
        Check(!guard.Error() && GetForegroundWindow() == dialog, "picker foreground before external activation boundary");
        if (timing == 0) SetForegroundWindow(owner.other);
        owner.switch_on_enable = timing == 1;
        owner.switch_on_activate = timing == 2;
        guard.RestoreBeforeDialogHide(dialog);
        Check(IsWindowEnabled(owner.window) && GetForegroundWindow() == owner.other && GetFocus() == owner.other,
            "restoration does not reclaim foreground after user/enable/activation switch");
        ShowWindow(dialog, SW_HIDE); DestroyWindow(dialog);
        Check(GetForegroundWindow() == owner.other && GetFocus() == owner.other, "picker hide/destroy does not reclaim switched foreground");
        owner.switch_on_enable = owner.switch_on_activate = false;
    }
    const HWND foreground = GetForegroundWindow();
    if ((foreground == owner.window || foreground == owner.other) && IsWindow(previous_foreground))
        SetForegroundWindow(previous_foreground);
}
}

constexpr UINT ForeignEnable = WM_APP + 91;
constexpr UINT ForeignDestroy = WM_APP + 92;
bool LowerIntegrity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &token)) return false;
    PSID sid = nullptr;
    bool ok = ConvertStringSidToSidW(L"S-1-16-4096", &sid) != FALSE;
    if (ok) {
        TOKEN_MANDATORY_LABEL label{{sid, SE_GROUP_INTEGRITY}};
        ok = SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid)) != FALSE;
        LocalFree(sid);
    }
    CloseHandle(token); return ok;
}
int ForeignChild(const wchar_t* argument, bool low = false, bool reverse = false) {
    const auto mapping = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argument, nullptr, 10)));
    auto* shared = static_cast<volatile UINT_PTR*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(UINT_PTR) * 8));
    if (!shared) return 2;
    if (low && !LowerIntegrity()) { shared[7] = GetLastError(); *shared = 1; return 3; }
    if (reverse) {
        pulse::PickerOwnerGuard guard(reinterpret_cast<HWND>(shared[1]));
        const auto diagnostic = guard.Diagnostics();
        shared[2] = guard.Error(); shared[3] = static_cast<UINT_PTR>(guard.FailureStage());
        shared[4] = diagnostic.caller_integrity; shared[5] = diagnostic.owner_integrity;
        shared[6] = diagnostic.same_user && diagnostic.same_session;
        *shared = 1;
        UnmapViewOfFile(const_cast<UINT_PTR*>(shared)); CloseHandle(mapping); return 0;
    }
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Pulse.ForeignOwner.Regression";
    wc.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
        if (message == ForeignEnable) { EnableWindow(window, wparam != 0); return 0; }
        static bool keep_running = false;
        if (message == ForeignDestroy) { keep_running = true; DestroyWindow(window); return 0; }
        if (message == WM_DESTROY && !keep_running) PostQuitMessage(0);
        return DefWindowProcW(window, message, wparam, lparam);
    };
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"owned foreign fixture", WS_OVERLAPPED | WS_DISABLED,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    *shared = reinterpret_cast<UINT_PTR>(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    UnmapViewOfFile(const_cast<UINT_PTR*>(shared)); CloseHandle(mapping); return 0;
}
void ForeignDisabled(bool low = false, HWND reverse_owner = nullptr) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(UINT_PTR) * 8, nullptr);
    auto* shared = mapping ? static_cast<volatile UINT_PTR*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UINT_PTR) * 8)) : nullptr;
    if (!shared) { Check(false, "foreign fixture mapping"); if (mapping) CloseHandle(mapping); return; }
    for (size_t i = 0; i < 8; ++i) shared[i] = 0;
    shared[1] = reinterpret_cast<UINT_PTR>(reverse_owner);
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    std::wstring command = L"\"" + std::wstring(self) + L"\" " + std::wstring(reverse_owner ? L"--reverse-low " : low ? L"--foreign-low " : L"--foreign ") + std::to_wstring(reinterpret_cast<UINT_PTR>(mapping));
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(self, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    Check(started != FALSE, "create owned foreign owner process");
    if (started) {
        const auto deadline = GetTickCount64() + 5000;
        while (!*shared && GetTickCount64() < deadline && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) Sleep(10);
        if (reverse_owner) {
            Check(*shared == 1 && shared[7] == 0, "self-lowered reverse fixture completed");
            Check(shared[2] == ERROR_ACCESS_DENIED && shared[3] == static_cast<UINT_PTR>(pulse::PickerOwnerFailureStage::IntegrityMismatch), "low caller to medium foreign owner rejected");
            Check(shared[4] == SECURITY_MANDATORY_LOW_RID && shared[5] >= SECURITY_MANDATORY_MEDIUM_RID && shared[6], "reverse denial records accurate same-user integrity levels");
        }
        const HWND window = reverse_owner ? nullptr : reinterpret_cast<HWND>(*shared);
        if (!reverse_owner) Check(IsWindow(window) && !IsWindowEnabled(window), "foreign disabled window ready");
        if (IsWindow(window)) {
            const HANDLE property = GetPropW(window, L"Pulse.PickerClient.OwnerGuard");
            {
                pulse::PickerOwnerGuard guard(window);
                Check(!guard.Error() && guard.Alive() && guard.FailureStage() == pulse::PickerOwnerFailureStage::None, "foreign disabled same-identity owner accepted");
                const auto identity = guard.Diagnostics();
                Check(identity.identity_available && identity.session_available && identity.same_user && identity.same_session &&
                    identity.caller_integrity >= SECURITY_MANDATORY_MEDIUM_RID &&
                    (!low || identity.owner_integrity == SECURITY_MANDATORY_LOW_RID), "foreign diagnostics identify the exact user/session/integrity relation");
                Check(GetPropW(window, L"Pulse.PickerClient.OwnerGuard") == property, "foreign window property untouched");
                pulse::PickerOwnerGuard duplicate(window);
                Check(duplicate.Error() == ERROR_BUSY && duplicate.FailureStage() == pulse::PickerOwnerFailureStage::MutexAcquire, "recursive named mutex cannot admit duplicate foreign guard");
                SendMessageW(window, ForeignEnable, TRUE, 0);
                Check(!guard.Alive(), "foreign owner enabled invalidates guard");
            }
            Check(IsWindowEnabled(window) && GetPropW(window, L"Pulse.PickerClient.OwnerGuard") == property, "foreign guard release preserves caller state");
            {
                pulse::PickerOwnerGuard enabled(window);
                Check(enabled.Error() == ERROR_NOT_SUPPORTED && enabled.FailureStage() == pulse::PickerOwnerFailureStage::ForeignEnabled, "enabled foreign owner rejected with stage");
            }
            SendMessageW(window, ForeignEnable, FALSE, 0);
            {
                pulse::PickerOwnerGuard guard(window);
                Check(!guard.Error(), "foreign guard can reacquire after release");
                SendMessageW(window, ForeignDestroy, 0, 0);
                MSG event{};
                const auto event_deadline = GetTickCount64() + 200;
                while (GetTickCount64() < event_deadline) {
                    while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&event);
                    Sleep(1);
                }
                Check(WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT && !guard.Alive(),
                    "foreign window destruction invalidates guard while process lives");
                PostThreadMessageW(child.dwThreadId, WM_QUIT, 0, 0);
                WaitForSingleObject(child.hProcess, 3000);
                Check(!guard.Alive(), "foreign owner process exit remains invalid");
            }
        }
        if (WaitForSingleObject(child.hProcess, 1000) == WAIT_TIMEOUT) { TerminateProcess(child.hProcess, 99); WaitForSingleObject(child.hProcess, 1000); }
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
    }
    UnmapViewOfFile(const_cast<UINT_PTR*>(shared)); CloseHandle(mapping);
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && (wcscmp(argv[1], L"--focus-only") == 0 || wcscmp(argv[1], L"--focus-legacy-tail") == 0)) {
        FocusRestoration(wcscmp(argv[1], L"--focus-legacy-tail") == 0);
        std::printf("Checks: %d, failures: %d\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc == 3 && wcscmp(argv[1], L"--foreign") == 0) return ForeignChild(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"--foreign-low") == 0) return ForeignChild(argv[2], true);
    if (argc == 3 && wcscmp(argv[1], L"--reverse-low") == 0) return ForeignChild(argv[2], true, true);
    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog([completed] {
        if (WaitForSingleObject(completed, 30000) != WAIT_OBJECT_0) {
            std::fputs("[FAIL] owner guard regression watchdog expired\n", stderr);
            TerminateProcess(GetCurrentProcess(), 99);
        }
    });
    ForeignDisabled(); ForeignDisabled(true);
    { Owner reverse(false); ForeignDisabled(true, reverse.window); }
    Responsive(); AlreadyDisabled(); Timeout(false); Timeout(true); RestoreTimeout(); Destroyed(); AttachedQueues();
    std::printf("Checks: %d, failures: %d\n", checks, failures);
    SetEvent(completed);
    watchdog.join();
    CloseHandle(completed);
    return failures ? 1 : 0;
}
