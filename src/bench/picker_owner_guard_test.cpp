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
void CrossThreadFocus() {
    const auto pump = [] { const auto end=GetTickCount64()+100; do { MSG msg{}; while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&msg); Sleep(1); } while(GetTickCount64()<end); };
    Owner owner;
    HWND a = owner.window;
    HWND b = CreateWindowW(L"STATIC", L"Owned B cross-thread", WS_OVERLAPPEDWINDOW,420,80,240,160,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ShowWindow(b, SW_SHOW); ShowWindow(a, SW_SHOWNOACTIVATE);
    const DWORD fg_thread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
    const bool attached=fg_thread && fg_thread!=GetCurrentThreadId() && AttachThreadInput(GetCurrentThreadId(),fg_thread,TRUE);
    SetForegroundWindow(b);
    if(attached) AttachThreadInput(GetCurrentThreadId(),fg_thread,FALSE);

    for (bool switched : {false,true}) {
        SetForegroundWindow(b); pump(); SetForegroundWindow(a); pump();
        Check(GetForegroundWindow()==a,"cross-thread A foreground over B (setup)");
        if(GetForegroundWindow()!=a){DestroyWindow(b);return;}
        pulse::PickerOwnerGuard guard(a);
        HWND dialog = CreateWindowW(L"STATIC",L"Owned cross-thread dialog",WS_POPUP|WS_CAPTION,80,80,200,100,a,nullptr,GetModuleHandleW(nullptr),nullptr);
        ShowWindow(dialog,SW_SHOW);SetForegroundWindow(dialog); pump();
        Check(!guard.Error() && GetForegroundWindow()==dialog,"cross-thread picker foreground (setup)");
        if(guard.Error() || GetForegroundWindow()!=dialog){DestroyWindow(dialog);DestroyWindow(b);return;}
        if(switched) { SetForegroundWindow(b); pump(); }
        const auto trace = [&](const char* stage) { DWORD fp=0; const HWND fg=GetForegroundWindow(); GetWindowThreadProcessId(fg,&fp);
            std::printf("FOCUS stage=%s A=%p B=%p D=%p fg=%p fg_pid=%lu self=%lu switched=%d\n",stage,static_cast<void*>(a),static_cast<void*>(b),static_cast<void*>(dialog),static_cast<void*>(fg),fp,GetCurrentProcessId(),switched); };
        trace("before-restore");
        guard.RestoreBeforeDialogHide(dialog); trace("after-restore-immediate"); pump(); trace("after-restore-pumped");
        const HWND expected=switched?b:a;
        DWORD fg_pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&fg_pid);printf("CROSS foreground_pid=%lu self=%lu isA=%d isB=%d switched=%d\n",fg_pid,GetCurrentProcessId(),GetForegroundWindow()==a,GetForegroundWindow()==b,switched);
        Check(IsWindowEnabled(a),"only guard-owned cross-thread disable restored");
        Check(GetForegroundWindow()==expected,"cross-thread restoration returns A or preserves user B");
        SetWindowPos(dialog,nullptr,0,0,0,0,SWP_HIDEWINDOW|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_NOMOVE|SWP_NOSIZE);
        DestroyWindow(dialog); pump(); trace("after-destroy-pumped");
        Check(GetForegroundWindow()==expected,"cross-thread composed hide/destroy preserves foreground");
    }
    DestroyWindow(b);
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
    ShowWindow(owner.window, SW_SHOWNOACTIVATE);
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
    for (int timing = 0; timing < 3; ++timing) {
        SetForegroundWindow(owner.window);
        const HWND saved_focus = GetFocus();
        EnableWindow(owner.window, FALSE); // MFC-style caller owns this transition.
        {
            pulse::PickerOwnerGuard guard(owner.window);
            HWND dialog = CreateWindowW(L"Pulse.OwnerGuard.FocusPopup", L"Caller-disabled owner",
                WS_POPUP | WS_CAPTION, 100, 100, 280, 120, owner.window, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            ShowWindow(dialog, SW_SHOW); SetForegroundWindow(dialog);
            Check(!guard.Error() && GetForegroundWindow() == dialog,
                "caller-disabled picker obtains foreground");
            if (timing == 1) SetForegroundWindow(owner.other);
            owner.switch_on_activate = timing == 2;
            guard.RestoreBeforeDialogHide(dialog);
            Check(!IsWindowEnabled(owner.window), "guard never enables caller-disabled owner");
            const HWND expected = timing == 0 ? owner.window : owner.other;
            Check(GetForegroundWindow() == expected,
                "disabled owner activation respects user and activation-handler switches");
            ShowWindow(dialog, SW_HIDE); DestroyWindow(dialog);
            Check(GetForegroundWindow() == expected,
                "disabled-owner hide and destroy preserve chosen foreground");
        }
        Check(!IsWindowEnabled(owner.window), "guard destruction preserves caller-owned disabled state");
        EnableWindow(owner.window, TRUE);
        // MFC restores saved focus itself after re-enabling its modal owner.
        if (GetForegroundWindow() == owner.window) SetFocus(saved_focus);
        Check(timing ? GetForegroundWindow() == owner.other :
            GetForegroundWindow() == owner.window && GetFocus() == owner.edit,
            "caller restoration keeps child focus or externally chosen foreground");
        owner.switch_on_activate = false;
    }
    {
        FocusOwner destroyed;
        EnableWindow(destroyed.window, FALSE);
        pulse::PickerOwnerGuard guard(destroyed.window);
        HWND dialog = CreateWindowW(L"Pulse.OwnerGuard.FocusPopup", L"Destroyed owner",
            WS_POPUP, 0, 0, 20, 20, destroyed.window, nullptr, GetModuleHandleW(nullptr), nullptr);
        DestroyWindow(destroyed.window);
        destroyed.window = nullptr;
        guard.RestoreBeforeDialogHide(dialog);
        Check(!guard.Alive(), "destroyed caller-disabled owner never receives restoration");
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

int PreHideSlowChild(const wchar_t* argument) {
    const auto mapping = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argument, nullptr, 10)));
    auto* shared = static_cast<volatile UINT_PTR*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UINT_PTR) * 8));
    if (!shared) return 2;
    const DWORD delay_ms = static_cast<DWORD>(shared[1]);
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Pulse.PreHideSlow.Regression";
    wc.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
        if (message == WM_NCCREATE) {
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        }
        if (message == WM_ENABLE && wparam) {
            const DWORD delay = static_cast<DWORD>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (delay) Sleep(delay);
        }
        if (message == WM_DESTROY) PostQuitMessage(0);
        return DefWindowProcW(window, message, wparam, lparam);
    };
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"Slow caller owner", WS_OVERLAPPEDWINDOW,
        80, 80, 220, 140, nullptr, nullptr, wc.hInstance, reinterpret_cast<LPVOID>(static_cast<UINT_PTR>(delay_ms)));
    ShowWindow(window, SW_SHOWNOACTIVATE);
    pulse::PickerOwnerGuard caller_guard(window);
    shared[2] = caller_guard.Error();
    shared[3] = caller_guard.PreHideEvent() ? 1 : 0;
    *shared = reinterpret_cast<UINT_PTR>(window);
    HANDLE pre_hide = caller_guard.PreHideEvent();
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        if (shared[4] == 1) break;
        const DWORD count = pre_hide ? 1u : 0u;
        const DWORD wait = MsgWaitForMultipleObjects(count, count ? &pre_hide : nullptr, FALSE, 20, QS_ALLINPUT);
        if (pre_hide && wait == WAIT_OBJECT_0) {
            caller_guard.CompletePreHideRestore();
            pre_hide = nullptr;
            shared[5] = 1;
        }
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(window);
    UnmapViewOfFile(const_cast<UINT_PTR*>(shared)); CloseHandle(mapping);
    return 0;
}

void PreHideSlowRestore(DWORD delay_ms, const char* label, bool private_desktop = false) {
    HDESK previous_desktop = nullptr;
    HDESK desktop = nullptr;
    std::wstring desktop_arg;
    if (private_desktop) {
        desktop_arg = L"PulseOwnerGuardSlow_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        previous_desktop = GetThreadDesktop(GetCurrentThreadId());
        desktop = CreateDesktopW(desktop_arg.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        const bool on_private = desktop && SetThreadDesktop(desktop);
        Check(on_private, "slow pre-hide runs on isolated private desktop");
        if (!on_private) { if (desktop) CloseDesktop(desktop); return; }
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(UINT_PTR) * 8, nullptr);
    auto* shared = mapping ? static_cast<volatile UINT_PTR*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UINT_PTR) * 8)) : nullptr;
    if (!shared) {
        Check(false, "pre-hide slow fixture mapping");
        if (mapping) CloseHandle(mapping);
        if (private_desktop) { SetThreadDesktop(previous_desktop); CloseDesktop(desktop); }
        return;
    }
    for (size_t i = 0; i < 8; ++i) shared[i] = 0;
    shared[1] = delay_ms;
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --pre-hide-slow-child " + std::to_wstring(reinterpret_cast<UINT_PTR>(mapping));
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
    if (private_desktop) startup.lpDesktop = desktop_arg.data();
    const BOOL started = CreateProcessW(self, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    Check(started != FALSE, label);
    if (started) {
        const auto deadline = GetTickCount64() + 5000;
        while (!*shared && GetTickCount64() < deadline && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) Sleep(10);
        const HWND owner_hwnd = reinterpret_cast<HWND>(*shared);
        Check(IsWindow(owner_hwnd) && shared[2] == 0 && shared[3] == 1 && !IsWindowEnabled(owner_hwnd),
            "slow caller owner disabled with active PreHideEvent");
        if (IsWindow(owner_hwnd)) {
            pulse::PickerOwnerGuard remote_guard(owner_hwnd, pulse::PickerOwnerGuard::CallerManagedRemoteOwner{child.dwProcessId});
            Check(!remote_guard.Error() && remote_guard.Alive(), "remote guard attaches to caller-guarded owner");
            if (!remote_guard.Error()) {
                WNDCLASSW popup_wc{}; popup_wc.lpfnWndProc = DefWindowProcW; popup_wc.hInstance = GetModuleHandleW(nullptr);
                popup_wc.lpszClassName = L"Pulse.PreHideSlow.Popup";
                RegisterClassW(&popup_wc);
                HWND dialog = CreateWindowW(popup_wc.lpszClassName, L"Slow restore popup", WS_POPUP | WS_CAPTION,
                    120, 120, 240, 120, owner_hwnd, nullptr, popup_wc.hInstance, nullptr);
                ShowWindow(dialog, SW_SHOW);
                SetForegroundWindow(dialog);
                const bool interactive_fg = GetForegroundWindow() == dialog;
                std::atomic<bool> stop_monitor{false};
                std::atomic<bool> saw_dead{false};
                std::thread monitor([&] {
                    while (!stop_monitor.load(std::memory_order_acquire)) {
                        if (!remote_guard.Alive()) { saw_dead.store(true, std::memory_order_release); return; }
                        Sleep(5);
                    }
                });
                if (interactive_fg) remote_guard.RestoreBeforeDialogHide(dialog);
                else pulse::picker_owner_detail::RestoreBeforeDialogHideForTest(remote_guard, dialog);
                const bool alive_after_restore = remote_guard.Alive();
                const auto wait_done = GetTickCount64() + 3000;
                while (!shared[5] && GetTickCount64() < wait_done) {
                    MSG msg{};
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    Sleep(10);
                }
                const bool alive_after_complete = remote_guard.Alive();
                stop_monitor.store(true, std::memory_order_release);
                monitor.join();
                Check(!saw_dead.load(std::memory_order_acquire), "concurrent Alive() monitor never cancels accepted result during slow pre-hide restore");
                Check(alive_after_restore && alive_after_complete, "remote guard remains Alive() during and after slow caller pre-hide restore");
                const bool completed = shared[5] == 1 && IsWindowEnabled(owner_hwnd);
                if (!completed) {
                    std::printf("  diag delay=%lu shared5=%llu enabled=%d iswin=%d exit=%lu diag_restored=%d\n",
                        delay_ms, static_cast<unsigned long long>(shared[5]),
                        IsWindowEnabled(owner_hwnd) ? 1 : 0, IsWindow(owner_hwnd) ? 1 : 0,
                        WaitForSingleObject(child.hProcess, 0),
                        remote_guard.Diagnostics().restore_owned_disable ? 1 : 0);
                }
                Check(completed, "caller completed pre-hide EnableWindow(TRUE)");
                if (delay_ms < 900) {
                    Check(remote_guard.Diagnostics().restore_owned_disable, "diagnostics records caller-owned pre-hide restore completion");
                }
                DestroyWindow(dialog);
            }
        }
        shared[4] = 1;
        if (WaitForSingleObject(child.hProcess, 3000) == WAIT_TIMEOUT) { TerminateProcess(child.hProcess, 99); WaitForSingleObject(child.hProcess, 1000); }
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
    }
    UnmapViewOfFile(const_cast<UINT_PTR*>(shared)); CloseHandle(mapping);
    if (private_desktop) {
        SetThreadDesktop(previous_desktop);
        CloseDesktop(desktop);
    }
}

int PreHideAlreadyDisabledChild(const wchar_t* argument) {
    const auto mapping = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argument, nullptr, 10)));
    auto* shared = static_cast<volatile UINT_PTR*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UINT_PTR) * 8));
    if (!shared) return 2;
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Pulse.PreHideDisabled.Regression";
    wc.lpfnWndProc = [](HWND window, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT {
        if (message == WM_DESTROY) PostQuitMessage(0);
        return DefWindowProcW(window, message, wparam, lparam);
    };
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"Already-disabled caller owner", WS_OVERLAPPEDWINDOW,
        80, 80, 220, 140, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    // Simulate MFC CFileDialog::DoModal disabling its owner before calling Show(owner).
    EnableWindow(window, FALSE);
    pulse::PickerOwnerGuard caller_guard(window);
    shared[2] = caller_guard.Error();
    shared[3] = caller_guard.PreHideEvent() ? 1 : 0;
    *shared = reinterpret_cast<UINT_PTR>(window);
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        if (shared[4] == 1) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(window);
    UnmapViewOfFile(const_cast<UINT_PTR*>(shared)); CloseHandle(mapping);
    return 0;
}

void PreHideAlreadyDisabled() {
    const auto desktop_name = L"PulseOwnerGuardDisabled_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    HDESK previous_desktop = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    const bool on_private = desktop && SetThreadDesktop(desktop);
    Check(on_private, "already-disabled pre-hide runs on isolated private desktop");
    if (!on_private) { if (desktop) CloseDesktop(desktop); return; }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(UINT_PTR) * 8, nullptr);
    auto* shared = mapping ? static_cast<volatile UINT_PTR*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UINT_PTR) * 8)) : nullptr;
    if (!shared) {
        Check(false, "already-disabled pre-hide fixture mapping");
        if (mapping) CloseHandle(mapping);
        SetThreadDesktop(previous_desktop); CloseDesktop(desktop);
        return;
    }
    for (size_t i = 0; i < 8; ++i) shared[i] = 0;
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --pre-hide-disabled-child " + std::to_wstring(reinterpret_cast<UINT_PTR>(mapping));
    std::wstring desktop_arg = desktop_name;
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = desktop_arg.data();
    PROCESS_INFORMATION child{};
    const BOOL started = CreateProcessW(self, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
    Check(started != FALSE, "start already-disabled caller owner (MFC CFileDialog shape)");
    if (started) {
        const auto deadline = GetTickCount64() + 5000;
        while (!*shared && GetTickCount64() < deadline && WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) Sleep(10);
        const HWND owner_hwnd = reinterpret_cast<HWND>(*shared);
        Check(IsWindow(owner_hwnd) && shared[2] == 0 && shared[3] == 0 && !IsWindowEnabled(owner_hwnd),
            "already-disabled caller owner has error=0 and null PreHideEvent");
        if (IsWindow(owner_hwnd)) {
            LARGE_INTEGER freq{}; QueryPerformanceFrequency(&freq);
            WNDCLASSW popup_wc{}; popup_wc.lpfnWndProc = DefWindowProcW; popup_wc.hInstance = GetModuleHandleW(nullptr);
            popup_wc.lpszClassName = L"Pulse.PreHideDisabled.Popup";
            RegisterClassW(&popup_wc);
            double max_ms = 0.0;
            bool all_ok = true;
            for (int run = 1; run <= 3; ++run) {
                pulse::PickerOwnerGuard remote_guard(owner_hwnd, pulse::PickerOwnerGuard::CallerManagedRemoteOwner{child.dwProcessId});
                if (remote_guard.Error() || !remote_guard.Alive()) { all_ok = false; break; }
                HWND dialog = CreateWindowW(popup_wc.lpszClassName, L"Disabled owner popup", WS_POPUP | WS_CAPTION,
                    120, 120, 240, 120, owner_hwnd, nullptr, popup_wc.hInstance, nullptr);
                ShowWindow(dialog, SW_SHOWNOACTIVATE);
                LARGE_INTEGER t0{}, t1{};
                QueryPerformanceCounter(&t0);
                pulse::picker_owner_detail::RestoreBeforeDialogHideForTest(remote_guard, dialog);
                QueryPerformanceCounter(&t1);
                const double elapsed_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
                if (elapsed_ms > max_ms) max_ms = elapsed_ms;
                const bool alive = remote_guard.Alive();
                const bool still_disabled = !IsWindowEnabled(owner_hwnd);
                const bool restored = remote_guard.Diagnostics().restore_owned_disable;
                std::printf("PRE_HIDE_ALREADY_DISABLED run=%d elapsed_ms=%.2f restored=%d alive=%d enabled=%d\n",
                    run, elapsed_ms, restored ? 1 : 0, alive ? 1 : 0, still_disabled ? 0 : 1);
                std::fflush(stdout);
                all_ok &= alive && still_disabled && !restored;
                DestroyWindow(dialog);
            }
            Check(all_ok, "remote guard preserves already-disabled caller owner state and stays Alive()");
            Check(max_ms < 100.0, "remote RestoreBeforeDialogHide does not stall 1000ms on already-disabled caller owner");
        }
        shared[4] = 1;
        if (WaitForSingleObject(child.hProcess, 3000) == WAIT_TIMEOUT) { TerminateProcess(child.hProcess, 99); WaitForSingleObject(child.hProcess, 1000); }
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
    }
    UnmapViewOfFile(const_cast<UINT_PTR*>(shared)); CloseHandle(mapping);
    SetThreadDesktop(previous_desktop);
    CloseDesktop(desktop);
}

int wmain(int argc, wchar_t** argv) {
    if(argc==2 && wcscmp(argv[1],L"--focus-cross-thread")==0){CrossThreadFocus();printf("Checks: %d, failures: %d\n",checks,failures);return failures?1:0;}
    if (argc == 2 && wcscmp(argv[1], L"--pre-hide-already-disabled") == 0) {
        PreHideAlreadyDisabled();
        std::printf("Checks: %d, failures: %d\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc == 3 && wcscmp(argv[1], L"--pre-hide-disabled-child") == 0) return PreHideAlreadyDisabledChild(argv[2]);
    if (argc == 2 && (wcscmp(argv[1], L"--pre-hide-slow") == 0 || wcscmp(argv[1], L"--pre-hide-slow-private") == 0)) {
        const bool priv = wcscmp(argv[1], L"--pre-hide-slow-private") == 0;
        PreHideSlowRestore(350, "start 350ms slow WM_ENABLE caller (Photoshop shape)", priv);
        PreHideSlowRestore(1200, "start 1200ms over-timeout WM_ENABLE caller", priv);
        std::printf("Checks: %d, failures: %d\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc == 3 && wcscmp(argv[1], L"--pre-hide-slow-child") == 0) return PreHideSlowChild(argv[2]);
    if (argc == 2 && wcscmp(argv[1], L"--foreign-only") == 0) {
        ForeignDisabled(); ForeignDisabled(true);
        { Owner reverse(false); ForeignDisabled(true, reverse.window); }
        std::printf("Checks: %d, failures: %d\n", checks, failures);
        return failures ? 1 : 0;
    }
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
    PreHideAlreadyDisabled();
    PreHideSlowRestore(350, "start 350ms slow WM_ENABLE caller (Photoshop shape)");
    PreHideSlowRestore(1200, "start 1200ms over-timeout WM_ENABLE caller");
    Responsive(); AlreadyDisabled(); Timeout(false); Timeout(true); RestoreTimeout(); Destroyed(); AttachedQueues();
    std::printf("Checks: %d, failures: %d\n", checks, failures);
    SetEvent(completed);
    watchdog.join();
    CloseHandle(completed);
    return failures ? 1 : 0;
}

