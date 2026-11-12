#include "../common/picker_owner_guard.h"
#include <sddl.h>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#if defined(PULSE_REMOTE_PROVIDER_TEST)
#include "../shell_bridge/file_dialog_remote_provider.h"
#include <objbase.h>
#include <vector>
#endif

namespace {
constexpr UINT kEnable = WM_APP + 101, kDestroy = WM_APP + 102;
struct Shared {
    HWND window;
    DWORD pid, thread;
    volatile LONG ready;
    DWORD guard_error, setup_error;
};
unsigned checks = 0, failures = 0;
void Check(bool value, const char* name) { ++checks; if (!value) ++failures; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); std::fflush(stdout); }
bool LowerOwnIntegrity() {
    HANDLE token = nullptr; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &token)) return false;
    PSID sid = nullptr; bool ok = ConvertStringSidToSidW(L"S-1-16-4096", &sid) != FALSE;
    if (ok) { TOKEN_MANDATORY_LABEL label{{sid, SE_GROUP_INTEGRITY}}; ok = SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid)) != FALSE; LocalFree(sid); }
    CloseHandle(token); return ok;
}
int Child(const wchar_t* mapping_arg, const wchar_t* owner_arg, bool low) {
    const auto mapping = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(_wcstoui64(mapping_arg, nullptr, 10)));
    auto* shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) return 2;
    if (low && !LowerOwnIntegrity()) { shared->setup_error = GetLastError(); InterlockedExchange(&shared->ready, 1); return 3; }
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"Pulse.RemoteOwner.IsolatedTest";
    wc.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
        static bool keep_process = false;
        if (message == kEnable) { EnableWindow(window, wp != 0); return 0; }
        if (message == kDestroy) { keep_process = true; DestroyWindow(window); return 0; }
        if (message == WM_DESTROY && !keep_process) PostQuitMessage(0);
        return DefWindowProcW(window, message, wp, lp);
    };
    RegisterClassW(&wc);
    shared->window = CreateWindowW(wc.lpszClassName, L"Pulse owned remote owner test", WS_OVERLAPPED | WS_DISABLED,
        0, 0, 120, 80, nullptr, nullptr, wc.hInstance, nullptr);
    if (!shared->window) { shared->setup_error = GetLastError(); InterlockedExchange(&shared->ready, 1); return 4; }
    shared->pid = GetCurrentProcessId(); shared->thread = GetCurrentThreadId();
    std::unique_ptr<pulse::PickerOwnerGuard> guard;
    const auto guarded = reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(owner_arg, nullptr, 10)));
    if (guarded) { guard = std::make_unique<pulse::PickerOwnerGuard>(guarded); shared->guard_error = guard->Error(); }
    InterlockedExchange(&shared->ready, 1);
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    guard.reset(); UnmapViewOfFile(shared); CloseHandle(mapping); return 0;
}
struct Process {
    HANDLE mapping = nullptr;
    Shared* shared = nullptr;
    PROCESS_INFORMATION process{};
    bool ready = false;
    Process(HWND guarded = nullptr, bool low = false) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(Shared), nullptr);
        if (!mapping) return;
        shared = static_cast<Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!shared) return;
        *shared = {};
        wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
        std::wstring command = L"\"" + std::wstring(self) + L"\" --child " + std::to_wstring(reinterpret_cast<uintptr_t>(mapping)) +
            L" " + std::to_wstring(reinterpret_cast<uintptr_t>(guarded)) + (low ? L" 1" : L" 0");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        if (!CreateProcessW(self, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return;
        const auto end = GetTickCount64() + 5000;
        while (!InterlockedCompareExchange(&shared->ready, 0, 0) && GetTickCount64() < end && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) Sleep(10);
        DWORD pid = 0;
        ready = shared->ready && !shared->setup_error && !shared->guard_error && shared->pid == process.dwProcessId &&
            GetWindowThreadProcessId(shared->window, &pid) == process.dwThreadId && pid == process.dwProcessId;
    }
    void Exit() {
        if (!process.hProcess) return;
        PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0);
        if (WaitForSingleObject(process.hProcess, 3000) != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 3000); }
    }
    ~Process() {
        Exit(); if (process.hThread) CloseHandle(process.hThread); if (process.hProcess) CloseHandle(process.hProcess);
        if (shared) UnmapViewOfFile(shared); if (mapping) CloseHandle(mapping);
    }
};
void PumpEvents() {
    const auto end = GetTickCount64() + 100;
    while (GetTickCount64() < end) { MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg); Sleep(1); }
}
void Policy() {
    using Stage = pulse::PickerOwnerFailureStage;
    const auto policy = pulse::picker_owner_detail::ForeignIdentityPolicy;
    Check(policy(true, true, SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_MEDIUM_RID) == Stage::None, "same SID/session/equal integrity allowed");
    Check(policy(false, true, SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_MEDIUM_RID) == Stage::UserMismatch, "different full SID rejected by policy");
    Check(policy(true, false, SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_MEDIUM_RID) == Stage::Session, "different session rejected by policy");
    Check(policy(true, true, SECURITY_MANDATORY_LOW_RID, SECURITY_MANDATORY_LOW_RID) == Stage::IntegrityMismatch, "low caller cannot use foreign-owner observer route");
    Check(policy(true, true, SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_HIGH_RID) == Stage::IntegrityMismatch, "caller cannot control higher-integrity owner");
    Check(policy(true, true, SECURITY_MANDATORY_MEDIUM_RID, SECURITY_MANDATORY_LOW_RID) == Stage::None, "medium caller may observe disabled lower-integrity owner");
}
void ThreeProcesses(bool low) {
    Process owner(nullptr, low); Check(owner.ready, "owned foreign window process ready"); if (!owner.ready) return;
    Process caller(owner.shared->window); Check(caller.ready, "authenticated-caller fixture holds normal modal guard for third-process window"); if (!caller.ready) return;
    const HWND window = owner.shared->window;
    const auto valid = pulse::ValidateRemotePickerOwner(window, caller.process.hProcess, false);
    std::printf("preflight caller_pid=%lu owner_pid=%lu stage=%u error=%lu caller_il=%lu owner_il=%lu\n",
        valid.caller_pid, valid.owner_pid, static_cast<unsigned>(valid.stage), valid.error, valid.identity.caller_integrity, valid.identity.owner_integrity);
    Check(!valid.error && valid.owner_pid != valid.caller_pid && valid.owner_pid == owner.process.dwProcessId && valid.caller_pid == caller.process.dwProcessId,
        "remote preflight admits identity-bound disabled third-process owner");
    Check(valid.identity.same_user && valid.identity.same_session && (!low || valid.identity.owner_integrity == SECURITY_MANDATORY_LOW_RID),
        "actual owner identity and integrity diagnostics verified");
    const HANDLE property = GetPropW(window, L"Pulse.PickerClient.OwnerGuard");
    {
        pulse::PickerOwnerGuard guard(window, pulse::PickerOwnerGuard::CallerManagedRemoteOwner{caller.process.dwProcessId});
        Check(!guard.Error() && guard.Alive(), "remote Show guard accepts third-process owner without competing caller modal mutex");
        guard.RestoreBeforeDialogHide(nullptr);
        Check(!IsWindowEnabled(window) && GetPropW(window, L"Pulse.PickerClient.OwnerGuard") == property, "remote observer does not enable or mark foreign owner");
        SendMessageW(window, kEnable, TRUE, 0);
        Check(!guard.Alive(), "owner enabling invalidates remote lifetime guard");
        const auto enabled = pulse::ValidateRemotePickerOwner(window, caller.process.hProcess, false);
        Check(enabled.error == ERROR_NOT_SUPPORTED && enabled.stage == pulse::PickerOwnerFailureStage::ForeignEnabled, "enabled third-process owner rejected with readable stage");
    }
    Check(IsWindowEnabled(window), "remote guard destruction never undoes caller-managed enable state");
    SendMessageW(window, kEnable, FALSE, 0);
    SendMessageW(caller.shared->window, kEnable, TRUE, 0);
    Check(!pulse::ValidateRemotePickerOwner(caller.shared->window, caller.process.hProcess, false).error,
        "parent-owned enabled window remains eligible for ValidateConfig before caller guards it");
    Check(pulse::ValidateRemotePickerOwner(caller.shared->window, caller.process.hProcess, true).error == ERROR_NOT_SUPPORTED,
        "Show requires caller-owned window already disabled");
    SendMessageW(caller.shared->window, kEnable, FALSE, 0);
    Check(!pulse::ValidateRemotePickerOwner(caller.shared->window, caller.process.hProcess, true).error, "parent-owned disabled window Show remains supported");
    Check(pulse::ValidateRemotePickerOwner(window, nullptr, true).error == ERROR_INVALID_HANDLE, "missing authenticated parent handle rejected");
    Check(pulse::ValidateRemotePickerOwner(reinterpret_cast<HWND>(uintptr_t{1}), caller.process.hProcess, true).error != ERROR_SUCCESS,
        "arbitrary invalid HWND rejected");
    {
        pulse::PickerOwnerGuard guard(window, pulse::PickerOwnerGuard::CallerManagedRemoteOwner{caller.process.dwProcessId});
        Check(!guard.Error() && guard.Alive(), "remote foreign guard reacquires without touching caller modal ownership");
        SendMessageW(window, kDestroy, 0, 0); PumpEvents();
        Check(WaitForSingleObject(owner.process.hProcess, 0) == WAIT_TIMEOUT && !guard.Alive(), "window destruction invalidates guard while owner process lives");
        Check(pulse::ValidateRemotePickerOwner(window, caller.process.hProcess, true).error != ERROR_SUCCESS, "destroyed HWND fails new remote preflight");
    }
}
void ForeignFocus(bool direct) {
    Process owner; Check(owner.ready, "foreign focus owner ready"); if (!owner.ready) return;
    HWND a = owner.shared->window;
    HWND b = CreateWindowW(L"STATIC", L"Owned B focus fixture", WS_OVERLAPPEDWINDOW, 420,80,280,180,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ShowWindow(b, SW_SHOW); ShowWindow(a, SW_SHOWNOACTIVATE);
    const DWORD fg_thread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
    const bool attached=fg_thread && fg_thread!=GetCurrentThreadId() && AttachThreadInput(GetCurrentThreadId(),fg_thread,TRUE);
    SetForegroundWindow(b);
    if(attached) AttachThreadInput(GetCurrentThreadId(),fg_thread,FALSE);

    for (bool switched : {false, true}) {
        SendMessageW(a, kEnable, TRUE, 0); SetForegroundWindow(b); PumpEvents(); SetForegroundWindow(a); PumpEvents();
        Check(GetForegroundWindow() == a, "A foreground over B before modal (setup)");
        if(GetForegroundWindow()!=a){DestroyWindow(b);return;}
        SendMessageW(a, kEnable, FALSE, 0);
        pulse::PickerOwnerGuard guard(a);
        HWND dialog = CreateWindowW(L"STATIC",L"Owned dialog",WS_POPUP|WS_CAPTION,100,100,240,100,a,nullptr,GetModuleHandleW(nullptr),nullptr);
        ShowWindow(dialog, SW_SHOW); SetForegroundWindow(dialog); PumpEvents();
        Check(!guard.Error() && GetForegroundWindow()==dialog,"owned foreign dialog foreground (setup)");
        if(guard.Error() || GetForegroundWindow()!=dialog){DestroyWindow(dialog);DestroyWindow(b);return;}
        if(switched) {SetForegroundWindow(b);PumpEvents();}
        const auto trace = [&](const char* stage) { DWORD fp=0; const HWND fg=GetForegroundWindow(); GetWindowThreadProcessId(fg,&fp);
            std::printf("FOCUS stage=%s A=%p B=%p D=%p fg=%p fg_pid=%lu self=%lu switched=%d\n",stage,static_cast<void*>(a),static_cast<void*>(b),static_cast<void*>(dialog),static_cast<void*>(fg),fp,GetCurrentProcessId(),switched); };
        trace("before-restore");
        guard.RestoreBeforeDialogHide(dialog); trace("after-restore-immediate"); PumpEvents(); trace("after-restore-pumped");
        if(direct && !switched) {const BOOL ok=SetForegroundWindow(a);printf("DIRECT disabled SetForegroundWindow=%d\n",ok);PumpEvents();}
        const HWND expected=switched?b:a;
        printf("FOREIGN switched=%d before_hide_expected=%d enabled=%d\n",switched,GetForegroundWindow()==expected,IsWindowEnabled(a));
        Check(GetForegroundWindow()==expected,"before hide returns A or preserves switched B");
        SetWindowPos(dialog,nullptr,0,0,0,0,SWP_HIDEWINDOW|SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_NOMOVE|SWP_NOSIZE);DestroyWindow(dialog);PumpEvents(); trace("after-destroy-pumped");
        Check(GetForegroundWindow()==expected,"after hide returns A or preserves switched B");
        Check(!IsWindowEnabled(a),"foreign owner never enabled by restoration");
    }
    DestroyWindow(b);
}
void CallerExit() {
    Process owner; Process caller(owner.ready ? owner.shared->window : nullptr);
    Check(owner.ready && caller.ready, "caller-exit fixture ready"); if (!owner.ready || !caller.ready) return;
    pulse::PickerOwnerGuard guard(owner.shared->window, pulse::PickerOwnerGuard::CallerManagedRemoteOwner{caller.process.dwProcessId});
    Check(!guard.Error() && guard.Alive(), "bound caller handle accepted before exit");
    caller.Exit();
    Check(!guard.Alive() && IsWindow(owner.shared->window) && !IsWindowEnabled(owner.shared->window), "caller exit cancels guard without mutating still-live foreign owner");
    Check(pulse::ValidateRemotePickerOwner(owner.shared->window, caller.process.hProcess, true).error == ERROR_PROCESS_ABORTED,
        "retained dead parent process object cannot authorize subsequent preflight");
}
#if defined(PULSE_REMOTE_PROVIDER_TEST)
struct RemoteShowState {
    const PulsePickerUiV2Api* api = nullptr;
    PulsePickerUiV2Session session = 0;
    unsigned ready = 0;
    HWND picker = nullptr;
    HRESULT close = E_PENDING;
};
HRESULT WINAPI RemoteCallback(void* context, const PulsePickerUiV2Event* event) {
    auto& state = *static_cast<RemoteShowState*>(context);
    if (event && event->kind == PULSE_PICKER_UI_V2_EVENT_READY) {
        ++state.ready; state.picker = event->window;
        state.close = state.api->Close(state.session, event->show_id, HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
    return S_OK;
}
void PolicyValidate(const wchar_t* executable) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(com), "policy validation STA"); if (FAILED(com)) return;
    std::shared_ptr<const pulse::shell_bridge::FileDialogInprocessProvider> provider;
    const HRESULT loaded = pulse::shell_bridge::LoadFileDialogRemoteProvider(executable, provider, true);
    Check(SUCCEEDED(loaded) && provider, "policy validation real host authenticated");
    if (SUCCEEDED(loaded) && provider) {
        PulsePickerUiV2Initialize init{sizeof(init), PULSE_PICKER_UI_V2_VERSION, {0, 0}};
        PulsePickerUiV2Session session = 0;
        const HRESULT initialized = provider->api.Initialize(&init, &session);
        Check(SUCCEEDED(initialized), "policy validation Initialize");
        if (SUCCEEDED(initialized)) {
            wchar_t directory[32768]{}; const DWORD units = GetCurrentDirectoryW(32768, directory);
            for (unsigned mode = 0; mode < 6; ++mode) {
                PulsePickerUiV2FolderPolicyConfig config{}; auto& a = config.base.base;
                a.size = mode == 0 ? sizeof(a) : mode == 1 ? sizeof(config.base) : sizeof(config);
                a.version = PULSE_PICKER_UI_V2_VERSION; a.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
                a.initial_folder = {directory, units};
                if (mode == 1 || mode >= 4) { a.label_flags = PULSE_PICKER_UI_V2_LABEL_CANCEL; config.base.cancel_button_label = {L"Leave", 5}; }
                config.folder_policy = mode == 3 || mode == 5 ? PULSE_PICKER_UI_V2_FOLDER_ALLOW_AUTO_JUMP : 0;
                const HRESULT hr = provider->api.ValidateConfig(session, &a);
                std::printf("REMOTE_POLICY mode=%u size=%u policy=%u cancel=%u hr=%08lX\n", mode, a.size, config.folder_policy, a.label_flags, static_cast<unsigned long>(hr));
                Check(hr == S_OK, "real remote Validate preserves legacy/cancel/policy configuration");
            }
            Check(SUCCEEDED(provider->api.Shutdown(session)), "policy validation Shutdown");
        }
    }
    CoUninitialize();
}
void HostRoundtrip(const wchar_t* executable) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(com), "remote integration caller STA initialized"); if (FAILED(com)) return;
    {
        Process owner;
        Check(owner.ready, "actual remote host fixture has another-process disabled owner");
        if (owner.ready) {
            GUITHREADINFO before{}; before.cbSize = sizeof(before);
            const bool before_gui = GetGUIThreadInfo(owner.process.dwThreadId, &before) != FALSE;
            const HWND window = owner.shared->window;
            const HANDLE property = GetPropW(window, L"Pulse.PickerClient.OwnerGuard");
            std::shared_ptr<const pulse::shell_bridge::FileDialogInprocessProvider> provider;
            const HRESULT loaded = pulse::shell_bridge::LoadFileDialogRemoteProvider(executable, provider, true);
            std::printf("REMOTE_LOAD hr=%08lX caller_pid=%lu owner_pid=%lu\n", static_cast<unsigned long>(loaded), GetCurrentProcessId(), owner.process.dwProcessId);
            Check(SUCCEEDED(loaded) && provider, "real Pulse remote host connects through authenticated transport");
            if (SUCCEEDED(loaded) && provider) {
                const auto& api = provider->api;
                PulsePickerUiV2Initialize init{sizeof(init), PULSE_PICKER_UI_V2_VERSION, {0, 0}};
                RemoteShowState state; state.api = &api;
                const HRESULT initialized = api.Initialize(&init, &state.session);
                Check(SUCCEEDED(initialized), "real remote Initialize succeeds");
                if (SUCCEEDED(initialized)) {
                    std::wstring folder(32768, 0);
                    const DWORD count = GetCurrentDirectoryW(static_cast<DWORD>(folder.size()), folder.data());
                    if (count && count < folder.size()) folder.resize(count); else folder.clear();
                    const std::wstring title = L"Pulse isolated remote foreign-owner regression";
                    PulsePickerUiV2Config config{}; config.size = sizeof(config); config.version = PULSE_PICKER_UI_V2_VERSION;
                    config.owner = window; config.access_policy = PULSE_PICKER_UI_V2_ACCESS_LOCAL_OPEN_V1;
                    config.initial_folder = {folder.data(), static_cast<uint32_t>(folder.size())};
                    config.title = {title.data(), static_cast<uint32_t>(title.size())};
                    const HRESULT validated = api.ValidateConfig(state.session, &config);
                    Check(SUCCEEDED(validated), "Host::Dispatch ValidateConfig accepts identity-bound third-process owner");
                    HRESULT shown = validated;
                    PulsePickerUiV2Result result{}; result.size = sizeof(result);
                    std::vector<wchar_t> path(PULSE_PICKER_UI_V2_MAX_UNITS + 1), paths(PULSE_PICKER_UI_V2_MAX_PACKED_UNITS + 1);
                    result.path = {path.data(), static_cast<uint32_t>(path.size()), 0};
                    result.paths = {paths.data(), static_cast<uint32_t>(paths.size()), 0};
                    if (SUCCEEDED(validated)) {
                        PulsePickerUiV2Callbacks callbacks{sizeof(callbacks), 0, RemoteCallback, &state, nullptr, nullptr};
                        shown = api.Show(state.session, &config, &callbacks, &result);
                    }
                    std::printf("REMOTE_HOST initialized=%08lX validated=%08lX show=%08lX ready=%u close=%08lX status=%u caller_pid=%lu owner_pid=%lu\n",
                        static_cast<unsigned long>(initialized), static_cast<unsigned long>(validated), static_cast<unsigned long>(shown), state.ready,
                        static_cast<unsigned long>(state.close), result.status, GetCurrentProcessId(), owner.process.dwProcessId);
                    Check(state.ready == 1 && SUCCEEDED(state.close) && shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
                        result.status == PULSE_PICKER_UI_V2_CLOSED && result.terminal_hr == shown,
                        "Host::Dispatch Show reaches Ready then requested cancellation returns unchanged");
                    Check(SUCCEEDED(api.Shutdown(state.session)), "remote session shuts down cleanly");
                }
            }
            GUITHREADINFO after{}; after.cbSize = sizeof(after);
            const bool after_gui = GetGUIThreadInfo(owner.process.dwThreadId, &after) != FALSE;
            Check(IsWindow(window) && !IsWindowEnabled(window) && GetPropW(window, L"Pulse.PickerClient.OwnerGuard") == property,
                "real remote cancel preserves caller-disabled owner and owner property");
            Check(before_gui && after_gui && before.hwndFocus == after.hwndFocus && before.hwndActive == after.hwndActive,
                "real remote cancel leaves foreign owner thread focus and activation unchanged");
        }
    }
    CoUninitialize();
}
#endif
}
int wmain(int argc, wchar_t** argv) {
    if(argc==2 && (std::wstring_view(argv[1])==L"--focus-foreign" || std::wstring_view(argv[1])==L"--focus-direct")){ForeignFocus(std::wstring_view(argv[1])==L"--focus-direct");printf("Checks: %u, failures: %u\n",checks,failures);return failures?1:0;}
    if (argc == 5 && std::wstring_view(argv[1]) == L"--child") return Child(argv[2], argv[3], std::wstring_view(argv[4]) == L"1");
#if defined(PULSE_REMOTE_PROVIDER_TEST)
    if (argc == 3 && std::wstring_view(argv[1]) == L"--policy-validate") {
        PolicyValidate(argv[2]); std::printf("Checks: %u, failures: %u\n", checks, failures); return failures ? 1 : 0;
    }
    if (argc == 3 && std::wstring_view(argv[1]) == L"--host") {
        HostRoundtrip(argv[2]); std::printf("Checks: %u, failures: %u\n", checks, failures); return failures ? 1 : 0;
    }
#endif
    Policy(); ThreeProcesses(false); ThreeProcesses(true); CallerExit();
    std::printf("Checks: %u, failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}

