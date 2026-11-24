#include "picker_owner_guard.h"
#include "current_user_security.h"
#include <array>
#include <string>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
namespace pulse {
namespace {
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value) CloseHandle(value); }
    void Reset(HANDLE h) { if (value) CloseHandle(value); value = h; }
    HANDLE Get() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
// Requests contain no pointers in window messages. The registry owns each request
// until owner-thread restoration (or owner-thread exit) completes.
struct ThreadOwner : std::enable_shared_from_this<ThreadOwner> {
    static constexpr const wchar_t* Property = L"Pulse.PickerClient.OwnerGuard";
    static constexpr DWORD TimeoutMs = 2000;
    HWND window;
    DWORD thread;
    const UINT_PTR token = NextToken();
    HHOOK hook = nullptr;
    Handle owner_thread, disabled_event, restored_event, finished_event;
    std::atomic<bool> restore{false}, release{false}, finished{false}, recovery{false};
    std::atomic<bool> restored{false};
    std::atomic<DWORD> result{ERROR_IO_PENDING};
    bool busy = false, changed = false; // Accessed only on the owner thread.
    explicit ThreadOwner(HWND hwnd, DWORD tid) : window(hwnd), thread(tid),
        owner_thread(OpenThread(SYNCHRONIZE, FALSE, tid)),
        disabled_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        restored_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        finished_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    static auto& Registry() {
        static auto* entries = new std::unordered_map<HWND, std::shared_ptr<ThreadOwner>>;
        return *entries;
    }
    static auto& Lock() { static auto* lock = new std::mutex; return *lock; }
    static UINT_PTR NextToken() { static std::atomic<UINT_PTR> next{1}; return next.fetch_add(1); }
    static UINT Message() { static const UINT message = RegisterWindowMessageW(L"Pulse.PickerClient.OwnerTransition.v1"); return message; }
    bool Identity() const {
        DWORD pid = 0;
        return GetWindowThreadProcessId(window, &pid) == thread && pid == GetCurrentProcessId() &&
            GetPropW(window, Property) == this;
    }
    void Finish() {
        if (finished.exchange(true)) return;
        if (Identity()) RemovePropW(window, Property);
        if (hook) UnhookWindowsHookEx(hook);
        {
            std::lock_guard lock(Lock());
            auto it = Registry().find(window);
            if (it != Registry().end() && it->second.get() == this) Registry().erase(it);
        }
        SetEvent(disabled_event.Get());
        SetEvent(restored_event.Get());
        SetEvent(finished_event.Get());
    }
    void OnMessage(UINT message) {
        if (busy || finished.load()) return;
        if (message != Message() && !restore.load()) return;
        if (!Identity()) { result = ERROR_INVALID_WINDOW_HANDLE; Finish(); return; }
        busy = true;
        if (!restore.load() && result.load() == ERROR_IO_PENDING) {
            changed = IsWindowEnabled(window) != FALSE;
            if (changed) EnableWindow(window, FALSE);
            result = Identity() && !IsWindowEnabled(window) ? ERROR_SUCCESS : ERROR_INVALID_WINDOW_HANDLE;
            SetEvent(disabled_event.Get());
        }
        // A timeout may race with WM_ENABLE. Roll back on this same thread as
        // soon as that notification returns, even if its original caller left.
        if (restore.load()) {
            // Publish the restoration transition before EnableWindow changes
            // WS_DISABLED; the monitor must not mistake that interval for loss.
            restored = true;
            if (changed && Identity()) { changed = false; EnableWindow(window, TRUE); }
            SetEvent(restored_event.Get());
            if (release.load()) Finish();
        }
        busy = false;
    }
    static LRESULT CALLBACK Hook(int code, WPARAM wparam, LPARAM lparam) {
        if (code == HC_ACTION && wparam == PM_REMOVE) {
            auto* message = reinterpret_cast<MSG*>(lparam);
            std::shared_ptr<ThreadOwner> state;
            {
                std::lock_guard lock(Lock());
                auto it = Registry().find(message->hwnd);
                if (it != Registry().end()) state = it->second;
            }
            if (state) {
                const UINT original = message->message;
                const bool ours = original != Message() || message->wParam == state->token;
                if (original == Message()) { message->message = WM_NULL; message->hwnd = nullptr; }
                if (ours) state->OnMessage(original);
            }
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    DWORD Start() {
        if (!owner_thread || !disabled_event || !restored_event || !finished_event || !Message()) { Finish(); return ERROR_NOT_ENOUGH_MEMORY; }
        HMODULE module = nullptr;
        // Unhook does not wait for an already-running hook. Pin its code before
        // publishing it, including the exceptional path with a hung WM_ENABLE.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&Hook), &module)) { const DWORD error = GetLastError(); Finish(); return error; }
        {
            std::lock_guard lock(Lock());
            Registry().emplace(window, shared_from_this());
        }
        hook = SetWindowsHookExW(WH_GETMESSAGE, Hook, module, thread);
        if (!hook) { const DWORD error = GetLastError(); Finish(); return error; }
        if (!PostMessageW(window, Message(), token, 0)) { const DWORD error = GetLastError(); Finish(); return error; }
        // Post + event avoids SendMessageTimeout's same-input-queue exception.
        if (WaitForSingleObject(disabled_event.Get(), TimeoutMs) != WAIT_OBJECT_0) return ERROR_TIMEOUT;
        return result.load();
    }
    static DWORD WINAPI Recover(void* context) {
        std::unique_ptr<std::shared_ptr<ThreadOwner>> keep(static_cast<std::shared_ptr<ThreadOwner>*>(context));
        const auto state = *keep;
        const HANDLE events[]{state->finished_event.Get(), state->owner_thread.Get()};
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) state->Finish();
        return 0;
    }
    void DeferRecovery() {
        if (recovery.exchange(true) || finished.load()) return;
        auto* keep = new std::shared_ptr<ThreadOwner>(shared_from_this());
        Handle worker(CreateThread(nullptr, 0, Recover, keep, 0, nullptr));
        if (!worker) delete keep; // Registry still safely retains an outstanding hook.
    }
    bool Restore(bool final, bool wait = true) {
        if (final) release = true;
        restore = true;
        if (finished.load()) return true;
        if (!Identity()) { Finish(); return true; }
        PostMessageW(window, Message(), token, 0);
        const HANDLE event = final ? finished_event.Get() : restored_event.Get();
        if (wait && WaitForSingleObject(event, TimeoutMs) == WAIT_OBJECT_0) return true;
        if (final) DeferRecovery();
        return false;
    }
};
struct ForeignLifetime {
    HWND window = nullptr;
    DWORD pid = 0, thread = 0;
    std::atomic<bool> destroyed{false};
};
std::mutex foreign_events_mutex;
std::unordered_map<HWINEVENTHOOK, std::shared_ptr<ForeignLifetime>> foreign_events;
void CALLBACK ForeignEvent(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD) {
    if (event != EVENT_OBJECT_DESTROY || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    std::shared_ptr<ForeignLifetime> lifetime;
    {
        std::lock_guard lock(foreign_events_mutex);
        auto found = foreign_events.find(hook);
        if (found != foreign_events.end()) lifetime = found->second;
    }
    // The hook is filtered to the bound process and thread. Never query a possibly
    // reused HWND here: the destruction event describes its earlier generation.
    if (lifetime && lifetime->window == window && lifetime->thread == thread) lifetime->destroyed = true;
}
std::mutex foreign_mutex;
std::unordered_set<std::wstring> foreign_owners;
struct ProcessIdentity {
    std::array<BYTE, SECURITY_MAX_SID_SIZE> user{};
    DWORD integrity = 0, session = 0;
};
bool Identity(HANDLE process, ProcessIdentity& identity) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return false;
    Handle token(raw);
    alignas(void*) std::array<BYTE, 512> data{};
    DWORD size = 0;
    if (!GetTokenInformation(token.Get(), TokenUser, data.data(), static_cast<DWORD>(data.size()), &size)) return false;
    const auto user = reinterpret_cast<const TOKEN_USER*>(data.data());
    if (!IsValidSid(user->User.Sid) || !CopySid(static_cast<DWORD>(identity.user.size()), identity.user.data(), user->User.Sid)) return false;
    if (!GetTokenInformation(token.Get(), TokenIntegrityLevel, data.data(), static_cast<DWORD>(data.size()), &size)) return false;
    const auto label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(data.data());
    if (!IsValidSid(label->Label.Sid)) return false;
    const BYTE count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (!count) return false;
    identity.integrity = *GetSidSubAuthority(label->Label.Sid, count - 1);
    return GetTokenInformation(token.Get(), TokenSessionId, &identity.session, sizeof(identity.session), &size) && size == sizeof(identity.session);
}
}
namespace picker_owner_detail {
PickerOwnerFailureStage ForeignIdentityPolicy(bool same_user, bool same_session,
    DWORD caller_integrity, DWORD owner_integrity) noexcept {
    if (!same_session) return PickerOwnerFailureStage::Session;
    if (!same_user) return PickerOwnerFailureStage::UserMismatch;
    if (caller_integrity < SECURITY_MANDATORY_MEDIUM_RID || owner_integrity > caller_integrity)
        return PickerOwnerFailureStage::IntegrityMismatch;
    return PickerOwnerFailureStage::None;
}
}
PickerRemoteOwnerValidation ValidateRemotePickerOwner(HWND window, HANDLE authenticated_parent, bool showing) noexcept {
    PickerRemoteOwnerValidation result;
    const auto fail = [&](PickerOwnerFailureStage stage, DWORD error) {
        result.stage = stage; result.error = error ? error : ERROR_GEN_FAILURE; return result;
    };
    if (!authenticated_parent) return fail(PickerOwnerFailureStage::OpenProcess, ERROR_INVALID_HANDLE);
    SetLastError(ERROR_SUCCESS);
    result.caller_pid = GetProcessId(authenticated_parent);
    if (!result.caller_pid) return fail(PickerOwnerFailureStage::OpenProcess, GetLastError());
    if (WaitForSingleObject(authenticated_parent, 0) != WAIT_TIMEOUT) return fail(PickerOwnerFailureStage::ProcessTime, ERROR_PROCESS_ABORTED);
    FILETIME exit{}, kernel{}, user{};
    if (!GetProcessTimes(authenticated_parent, &result.caller_created, &exit, &kernel, &user))
        return fail(PickerOwnerFailureStage::ProcessTime, GetLastError());
    ProcessIdentity host, caller;
    if (!Identity(GetCurrentProcess(), host) || !Identity(authenticated_parent, caller))
        return fail(PickerOwnerFailureStage::TokenIdentity, GetLastError());
    const auto caller_policy = picker_owner_detail::ForeignIdentityPolicy(
        EqualSid(host.user.data(), caller.user.data()) != FALSE, host.session == caller.session,
        host.integrity, caller.integrity);
    if (caller_policy != PickerOwnerFailureStage::None) return fail(caller_policy, ERROR_ACCESS_DENIED);
    if (!window) return result;
    result.owner_thread = GetWindowThreadProcessId(window, &result.owner_pid);
    if (!result.owner_thread || !result.owner_pid || !IsWindow(window))
        return fail(PickerOwnerFailureStage::Window, ERROR_INVALID_WINDOW_HANDLE);
    if (result.owner_pid == GetCurrentProcessId()) return fail(PickerOwnerFailureStage::Window, ERROR_ACCESS_DENIED);
    Handle owner(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, result.owner_pid));
    if (!owner) return fail(PickerOwnerFailureStage::OpenProcess, GetLastError());
    if (!GetProcessTimes(owner.Get(), &result.owner_created, &exit, &kernel, &user))
        return fail(PickerOwnerFailureStage::ProcessTime, GetLastError());
    ProcessIdentity target;
    if (!Identity(owner.Get(), target)) return fail(PickerOwnerFailureStage::TokenIdentity, GetLastError());
    auto& diagnostic = result.identity;
    diagnostic.identity_available = diagnostic.session_available = true;
    diagnostic.same_user = EqualSid(caller.user.data(), target.user.data()) != FALSE;
    diagnostic.same_session = caller.session == target.session;
    diagnostic.caller_integrity = caller.integrity; diagnostic.owner_integrity = target.integrity;
    const auto policy = picker_owner_detail::ForeignIdentityPolicy(diagnostic.same_user, diagnostic.same_session,
        diagnostic.caller_integrity, diagnostic.owner_integrity);
    if (policy != PickerOwnerFailureStage::None) return fail(policy, ERROR_ACCESS_DENIED);
    if (showing && IsWindowEnabled(window))
        return fail(PickerOwnerFailureStage::ForeignEnabled, ERROR_NOT_SUPPORTED);
    DWORD final_pid = 0;
    if (!IsWindow(window) || GetWindowThreadProcessId(window, &final_pid) != result.owner_thread || final_pid != result.owner_pid ||
        WaitForSingleObject(owner.Get(), 0) != WAIT_TIMEOUT || WaitForSingleObject(authenticated_parent, 0) != WAIT_TIMEOUT)
        return fail(PickerOwnerFailureStage::WindowIdentity, ERROR_INVALID_WINDOW_HANDLE);
    return result;
}
namespace {
DWORD WaitOwnerHandles(DWORD count, const HANDLE* handles, DWORD timeout) {
    const ULONGLONG start = GetTickCount64();
    bool quit = false; int quit_code = 0;
    for (;;) {
        const ULONGLONG elapsed = GetTickCount64() - start;
        const DWORD remaining = timeout == INFINITE ? INFINITE : elapsed >= timeout ? 0 : timeout - static_cast<DWORD>(elapsed);
        const DWORD result = MsgWaitForMultipleObjectsEx(count, handles, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result != WAIT_OBJECT_0 + count) {
            if (quit) PostQuitMessage(quit_code);
            return result;
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { quit = true; quit_code = static_cast<int>(message.wParam); }
            else { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    }
}
constexpr const wchar_t* LeaseProperty = L"Pulse.PickerClient.ForeignLease.v1";
std::wstring LeaseEvent(DWORD pid) { return L"Local\\Pulse.PickerClient.ForeignLease.Release." + std::to_wstring(pid); }
struct ForeignLease {
    Handle process, release;
    ~ForeignLease() { if (release) SetEvent(release.Get()); }
    DWORD Start(HWND window, const wchar_t* executable) {
        if (!executable || !*executable) return ERROR_NOT_SUPPORTED;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        Handle ready(CreateEventW(&sa, TRUE, FALSE, nullptr));
        release.Reset(CreateEventW(&sa, TRUE, FALSE, nullptr));
        Handle parent;
        HANDLE inherited_parent = nullptr;
        if (!ready || !release || !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
            &inherited_parent, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, TRUE, 0)) return GetLastError();
        parent.Reset(inherited_parent);
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::unique_ptr<BYTE[]> storage(new BYTE[bytes]);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.get());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return GetLastError();
        struct Cleanup { LPPROC_THREAD_ATTRIBUTE_LIST p; ~Cleanup() { DeleteProcThreadAttributeList(p); } } cleanup{attributes};
        HANDLE handles[]{ready.Get(), parent.Get(), release.Get()};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr)) return GetLastError();
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --picker-owner-lease " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(window)) + L" " + std::to_wstring(reinterpret_cast<UINT_PTR>(parent.Get())) +
            L" " + std::to_wstring(reinterpret_cast<UINT_PTR>(ready.Get())) + L" " + std::to_wstring(reinterpret_cast<UINT_PTR>(release.Get()));
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
            nullptr, nullptr, &startup.StartupInfo, &child)) return GetLastError();
        process.Reset(child.hProcess); CloseHandle(child.hThread);
        HANDLE wait[]{ready.Get(), process.Get()};
        const DWORD status = WaitOwnerHandles(2, wait, 3000);
        if (status != WAIT_OBJECT_0 || !release || IsWindowEnabled(window)) {
            if (release) SetEvent(release.Get());
            // The helper also observes the retained parent handle. A startup that
            // misses this deadline must not later acquire a lease (ready timeout).
            return ERROR_TIMEOUT;
        }
        return ERROR_SUCCESS;
    }
    bool Restore() {
        if (!release) return false;
        SetEvent(release.Get());
        return WaitOwnerHandles(1, &process.value, 2000) == WAIT_OBJECT_0;
    }
};
bool ReleaseForeignLease(HWND window, std::atomic<bool>& foreign_restored) {
    const DWORD pid = static_cast<DWORD>(reinterpret_cast<UINT_PTR>(GetPropW(window, LeaseProperty)));
    if (!pid) return false;
    Handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
    Handle release(OpenEventW(EVENT_MODIFY_STATE, FALSE, LeaseEvent(pid).c_str()));
    if (!process || !release) return false;
    // Mark before signaling the lease helper so a concurrent Alive() poll on
    // another thread never misclassifies our own pre-hide EnableWindow(TRUE).
    foreign_restored.store(true, std::memory_order_release);
    SetEvent(release.Get());
    return WaitOwnerHandles(1, &process.value, 2000) == WAIT_OBJECT_0 && IsWindowEnabled(window);
}
std::wstring PreHideEventName(const std::wstring& owner_key, bool done) {
    return owner_key + (done ? L".PreHideDone" : L".PreHide");
}
std::wstring BuildOwnerKey(DWORD pid, FILETIME creation, HWND window) {
    return L"Local\\Pulse.PickerClient.Owner." + std::to_wstring(pid) + L"." +
        std::to_wstring(creation.dwHighDateTime) + L"." + std::to_wstring(creation.dwLowDateTime) + L"." +
        std::to_wstring(reinterpret_cast<UINT_PTR>(window));
}
DWORD WaitHandlePumpingSentMessages(HANDLE handle, DWORD timeout_ms) noexcept {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const ULONGLONG elapsed = GetTickCount64() - start;
        const DWORD remaining = elapsed >= timeout_ms ? 0u : timeout_ms - static_cast<DWORD>(elapsed);
        // Pump only inter-thread sent messages (e.g. WM_ACTIVATE/WM_NCACTIVATE
        // sent synchronously when the caller calls SetActiveWindow on an owner
        // that owns our cross-process popup). Otherwise a plain WaitForSingleObject
        // deadlocks the caller's CompletePreHideRestore until timeout!
        const DWORD wait = MsgWaitForMultipleObjectsEx(1, &handle, remaining, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) return WAIT_OBJECT_0;
        if (wait == WAIT_OBJECT_0 + 1) {
            MSG msg{};
            PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) return WAIT_OBJECT_0;
            if (remaining == 0) return WAIT_TIMEOUT;
            continue;
        }
        return wait;
    }
}
bool RequestCallerPreHideRestore(HWND window, DWORD pid, FILETIME creation, std::atomic<bool>* foreign_restored = nullptr) noexcept {
    if (!window || !pid) return false;
    try {
        const auto key = BuildOwnerKey(pid, creation, window);
        Handle trigger(OpenEventW(EVENT_MODIFY_STATE, FALSE, PreHideEventName(key, false).c_str()));
        Handle done(OpenEventW(SYNCHRONIZE, FALSE, PreHideEventName(key, true).c_str()));
        if (!trigger || !done) return false;
        // Mark before signaling the caller so neither a background Alive() poll
        // nor a slow caller WM_ENABLE repaint (>250ms, e.g. Photoshop) can flip
        // Alive() to false and discard an accepted Save/Open result.
        if (foreign_restored) foreign_restored->store(true, std::memory_order_release);
        SetEvent(trigger.Get());
        const DWORD wait = WaitHandlePumpingSentMessages(done.Get(), 1000);
        return wait == WAIT_OBJECT_0 || IsWindowEnabled(window) != FALSE;
    } catch (...) {
        return false;
    }
}
bool DialogIsForeground(HWND dialog, HWND& foreground) noexcept {
    foreground = GetForegroundWindow();
    bool was_foreground = foreground == dialog || IsChild(dialog, foreground);
    for (HWND current = foreground; !was_foreground && current; current = GetWindow(current, GW_OWNER)) {
        was_foreground = current == dialog;
    }
    return was_foreground;
}
}
// A separate process survives a crashing broker/remote host. Its main thread
// watches cancellation while a worker may be blocked in foreign WM_ENABLE.
int RunPickerOwnerLease(int argc, wchar_t** argv) noexcept {
    if (argc != 6) return 2;
    const auto window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(_wcstoui64(argv[2], nullptr, 10)));
    Handle parent(reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argv[3], nullptr, 10))));
    Handle ready(reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argv[4], nullptr, 10))));
    Handle cancel(reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argv[5], nullptr, 10))));
    const auto checked = ValidateRemotePickerOwner(window, parent.Get(), false);
    if (checked.error || !IsWindowEnabled(window)) return 3;
    CurrentUserSecurityAttributes security;
    if (!security) return 4;
    Handle release(CreateEventW(security.get(), TRUE, FALSE, LeaseEvent(GetCurrentProcessId()).c_str()));
    if (!release) return 5;
    Handle owner(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, checked.owner_pid));
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!owner || !GetProcessTimes(owner.Get(), &created, &exit, &kernel, &user) ||
        CompareFileTime(&created, &checked.owner_created) != 0) return 6;
    auto lifetime = std::make_shared<ForeignLifetime>();
    lifetime->window = window; lifetime->pid = checked.owner_pid; lifetime->thread = checked.owner_thread;
    const auto hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, ForeignEvent,
        checked.owner_pid, checked.owner_thread, WINEVENT_OUTOFCONTEXT);
    if (!hook) return 6;
    { std::lock_guard lock(foreign_events_mutex); foreign_events.emplace(hook, lifetime); }
    struct HookCleanup { HWINEVENTHOOK hook; ~HookCleanup() {
        UnhookWinEvent(hook); std::lock_guard lock(foreign_events_mutex); foreign_events.erase(hook);
    } } hook_cleanup{hook};
    struct Work {
        HWND window; DWORD pid, thread; HANDLE ready, release, parent, cancel;
        ForeignLifetime* lifetime;
        bool Same() const {
            DWORD current = 0;
            return !lifetime->destroyed.load() && GetWindowThreadProcessId(window, &current) == thread && current == pid &&
                GetPropW(window, LeaseProperty) == reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(GetCurrentProcessId()));
        }
        static DWORD WINAPI Run(void* context) {
            auto& w = *static_cast<Work*>(context);
            if (GetPropW(w.window, LeaseProperty) || !SetPropW(w.window, LeaseProperty,
                reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(GetCurrentProcessId())))) return 6;
            if (!w.Same()) return 7;
            if (!IsWindowEnabled(w.window)) { RemovePropW(w.window, LeaseProperty); return 7; }
            if (WaitForSingleObject(w.cancel, 0) != WAIT_TIMEOUT || WaitForSingleObject(w.release, 0) != WAIT_TIMEOUT || WaitForSingleObject(w.parent, 0) != WAIT_TIMEOUT) {
                RemovePropW(w.window, LeaseProperty); return 8;
            }
            EnableWindow(w.window, FALSE);
            if (w.Same() && !IsWindowEnabled(w.window)) SetEvent(w.ready);
            HANDLE waits[]{w.release, w.parent, w.cancel};
            WaitForMultipleObjects(3, waits, FALSE, INFINITE);
            if (w.Same()) { EnableWindow(w.window, TRUE); if (w.Same()) RemovePropW(w.window, LeaseProperty); }
            return 0;
        }
    } work{window, checked.owner_pid, checked.owner_thread, ready.Get(), release.Get(), parent.Get(), cancel.Get(), lifetime.get()};
    Handle worker(CreateThread(nullptr, 0, Work::Run, &work, 0, nullptr));
    if (!worker) return 9;
    HANDLE startup[]{ready.Get(), worker.Get(), parent.Get()};
    if (WaitOwnerHandles(3, startup, 2500) != WAIT_OBJECT_0) SetEvent(release.Get());
    WaitOwnerHandles(1, &worker.value, INFINITE);
    return 0;
}
struct PickerOwnerGuard::Impl {
    static constexpr const wchar_t* Property = L"Pulse.PickerClient.OwnerGuard";
    HWND window = nullptr;
    bool changed = false, external = false, locked = false, marked = false;
    DWORD pid = 0, thread = 0;
    FILETIME creation{};
    Handle process, mutex, remote_process;
    Handle pre_hide_event, pre_hide_done;
    bool pre_hide_completed = false;
    DWORD remote_pid = 0;
    FILETIME remote_creation{};
    std::shared_ptr<ThreadOwner> cross_thread;
    std::unique_ptr<ForeignLease> foreign_lease;
    std::atomic<bool> foreign_restored{false};
    DWORD error = ERROR_SUCCESS;
    PickerOwnerFailureStage stage = PickerOwnerFailureStage::None;
    PickerOwnerIdentityDiagnostics diagnostics;
    std::wstring foreign_key;
    bool foreign_registered = false;
    HWINEVENTHOOK foreign_hook = nullptr;
    std::shared_ptr<ForeignLifetime> foreign_lifetime;
    explicit Impl(HWND value, DWORD remote_parent = 0, const wchar_t* lease_executable = nullptr) : window(value) {
        if (!window) return;
        FILETIME validated_owner_creation{};
        stage = PickerOwnerFailureStage::Window;
        thread = GetWindowThreadProcessId(window, &pid);
        if (!thread || !pid || !IsWindow(window)) { error = ERROR_INVALID_WINDOW_HANDLE; return; }
        if (remote_parent) {
            stage = PickerOwnerFailureStage::OpenProcess;
            remote_process.Reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, remote_parent));
            if (!remote_process) { error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return; }
            const auto checked = ValidateRemotePickerOwner(window, remote_process.Get(), true);
            diagnostics = checked.identity;
            if (checked.error) { error = checked.error; stage = checked.stage; return; }
            remote_pid = checked.caller_pid; remote_creation = checked.caller_created;
            validated_owner_creation = checked.owner_created;
            if (checked.owner_pid != pid || checked.owner_thread != thread) {
                stage = PickerOwnerFailureStage::WindowIdentity; error = ERROR_INVALID_WINDOW_HANDLE; return;
            }
        }
        stage = PickerOwnerFailureStage::OpenProcess;
        process.Reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        if (!process) { error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return; }
        stage = PickerOwnerFailureStage::ProcessTime;
        FILETIME exit{}, kernel{}, user{};
        if (!GetProcessTimes(process.Get(), &creation, &exit, &kernel, &user)) { error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return; }
        if (remote_parent && CompareFileTime(&validated_owner_creation, &creation) != 0) {
            error = ERROR_INVALID_DATA; return;
        }
        external = thread != GetCurrentThreadId();
        if (external) {
            DWORD own_session = 0, owner_session = 0;
            ProcessIdentity own, other;
            stage = PickerOwnerFailureStage::Session;
            if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || !ProcessIdToSessionId(pid, &owner_session)) {
                error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return;
            }
            diagnostics.session_available = true;
            diagnostics.same_session = own_session == owner_session;
            if (!diagnostics.same_session) { error = ERROR_ACCESS_DENIED; return; }
            stage = PickerOwnerFailureStage::TokenIdentity;
            SetLastError(ERROR_SUCCESS);
            if (!Identity(GetCurrentProcess(), own) || !Identity(process.Get(), other)) {
                error = GetLastError(); if (!error) error = ERROR_ACCESS_DENIED; return;
            }
            diagnostics.identity_available = true;
            diagnostics.same_user = EqualSid(own.user.data(), other.user.data()) != FALSE;
            diagnostics.caller_integrity = own.integrity;
            diagnostics.owner_integrity = other.integrity;
            stage = PickerOwnerFailureStage::UserMismatch;
            if (!diagnostics.same_user) { error = ERROR_ACCESS_DENIED; return; }
            stage = PickerOwnerFailureStage::IntegrityMismatch;
            // A medium-or-higher caller may manage its own user's lower-
            // integrity window using the isolated lease helper, never the
            // reverse. Same-process transitions retain their equal-IL contract.
            const bool foreign = pid != GetCurrentProcessId();
            if (foreign ? (picker_owner_detail::ForeignIdentityPolicy(true, true, own.integrity, other.integrity) != PickerOwnerFailureStage::None)
                        : own.integrity != other.integrity) {
                error = ERROR_ACCESS_DENIED; return;
            }
            // Cross-thread EnableWindow sends synchronous messages. Caller-managed
            // already-disabled owners are supported without sending any message.

        }
        const auto name = BuildOwnerKey(pid, creation, window);
        std::unique_ptr<CurrentUserSecurityAttributes> security;
        if (!remote_parent) {
            stage = PickerOwnerFailureStage::SecurityAttributes;
            security = std::make_unique<CurrentUserSecurityAttributes>();
            if (!*security) { error = ERROR_ACCESS_DENIED; return; }
            stage = PickerOwnerFailureStage::MutexCreate;
            mutex.Reset(CreateMutexW(security->get(), FALSE, name.c_str()));
            if (!mutex) { error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return; }
            stage = PickerOwnerFailureStage::MutexAcquire;
            const DWORD acquired = WaitForSingleObject(mutex.Get(), 0);
            if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) {
                error = acquired == WAIT_TIMEOUT ? ERROR_BUSY : GetLastError();
                if (!error) error = ERROR_GEN_FAILURE; return;
            }
            locked = true;
        }
        // Only publish the cross-process PreHide handshake events when this guard
        // actually owns the owner's disabled transition (matching PreHideEvent()).
        // Otherwise callers that pre-disable their own owner (e.g. MFC CFileDialog)
        // never wait on PreHideEvent, and publishing the named events stalls the
        // remote host's RequestCallerPreHideRestore() for a full 1000 ms timeout.
        auto arm_pre_hide = [&] {
            if (remote_parent || !security || !*security) return;
            HANDLE trigger = CreateEventW(security->get(), TRUE, FALSE, PreHideEventName(name, false).c_str());
            HANDLE done = CreateEventW(security->get(), TRUE, FALSE, PreHideEventName(name, true).c_str());
            if (!trigger || !done) {
                if (trigger) CloseHandle(trigger);
                if (done) CloseHandle(done);
                return;
            }
            ResetEvent(trigger);
            ResetEvent(done);
            pre_hide_event.Reset(trigger);
            pre_hide_done.Reset(done);
        };
        stage = PickerOwnerFailureStage::WindowIdentity;
        if (!SameWindow()) { error = ERROR_INVALID_WINDOW_HANDLE; return; }
        if (pid != GetCurrentProcessId()) {
            if (IsWindowEnabled(window)) {
                stage = PickerOwnerFailureStage::ForeignEnabled;
                if (remote_parent || !lease_executable) { error = ERROR_NOT_SUPPORTED; return; }
                foreign_lease = std::make_unique<ForeignLease>();
                error = foreign_lease->Start(window, lease_executable);
                if (error) return;
            }
            foreign_key = name;
            std::lock_guard guard(foreign_mutex);
            if (!foreign_owners.insert(foreign_key).second) { stage = PickerOwnerFailureStage::MutexAcquire; error = ERROR_BUSY; return; }
            foreign_registered = true;
            try {
                foreign_lifetime = std::make_shared<ForeignLifetime>();
                foreign_lifetime->window = window; foreign_lifetime->pid = pid; foreign_lifetime->thread = thread;
                stage = PickerOwnerFailureStage::EventHook;
                SetLastError(ERROR_SUCCESS);
                foreign_hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, ForeignEvent,
                    pid, thread, WINEVENT_OUTOFCONTEXT);
                if (!foreign_hook) { error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return; }
                {
                    std::lock_guard event_lock(foreign_events_mutex);
                    foreign_events.emplace(foreign_hook, foreign_lifetime);
                }
            } catch (...) {
                stage = PickerOwnerFailureStage::EventHook;
                error = ERROR_NOT_ENOUGH_MEMORY;
                return;
            }
            stage = PickerOwnerFailureStage::WindowIdentity;
            if (!SameWindow() || IsWindowEnabled(window)) { error = ERROR_INVALID_WINDOW_HANDLE; return; }
            if (foreign_lease) arm_pre_hide();
            return;
        }
        stage = PickerOwnerFailureStage::WindowProperty;
        if (GetPropW(window, Property)) { error = ERROR_BUSY; return; }
        if (!SetPropW(window, Property, this)) {
            error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return;
        }
        marked = true;
        if (external && pid == GetCurrentProcessId() && IsWindowEnabled(window)) {
            cross_thread = std::make_shared<ThreadOwner>(window, thread);
            if (!SetPropW(window, Property, cross_thread.get())) {
                error = GetLastError(); cross_thread.reset(); return;
            }
            stage = PickerOwnerFailureStage::ThreadTransition;
            error = cross_thread->Start();
            if (error) cross_thread->Restore(true, false);
            else arm_pre_hide();
            return;
        }
        changed = !external && IsWindowEnabled(window);
        if (changed) {
            EnableWindow(window, FALSE);
            arm_pre_hide();
        }
    }
    bool SameWindow() const {
        if (!window) return true;
        DWORD current_pid = 0;
        FILETIME started{}, exit{}, kernel{}, user{};
        if (remote_process && (WaitForSingleObject(remote_process.Get(), 0) != WAIT_TIMEOUT ||
            GetProcessId(remote_process.Get()) != remote_pid ||
            !GetProcessTimes(remote_process.Get(), &started, &exit, &kernel, &user) ||
            CompareFileTime(&remote_creation, &started) != 0)) return false;
        return process && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT && IsWindow(window) &&
            GetWindowThreadProcessId(window, &current_pid) == thread && current_pid == pid &&
            GetProcessTimes(process.Get(), &started, &exit, &kernel, &user) &&
            CompareFileTime(&creation, &started) == 0;
    }
    bool Alive() const {
        if (foreign_registered) {
            // Out-of-context events are delivered on the creating STA as it pumps.
            // PID/TID/creation checks remain necessary before queued events arrive.
            return foreign_lifetime && !foreign_lifetime->destroyed.load() && SameWindow() && (foreign_restored.load(std::memory_order_acquire) || (foreign_lease && WaitForSingleObject(foreign_lease->process.Get(), 0) == WAIT_OBJECT_0) || !IsWindowEnabled(window));
        }
        return !window || (marked && SameWindow() &&
            GetPropW(window, Property) == (cross_thread ? static_cast<void*>(cross_thread.get()) : static_cast<void*>(const_cast<Impl*>(this))) &&
            (!external || (cross_thread && cross_thread->restored.load()) || !IsWindowEnabled(window)));
    }
    HANDLE PreHideEvent() const noexcept {
        if (error || pre_hide_completed || !pre_hide_event) return nullptr;
        if (!changed && !cross_thread && !foreign_lease) return nullptr;
        return pre_hide_event.Get();
    }
    void CompletePreHideRestore() noexcept {
        if (pre_hide_completed) return;
        pre_hide_completed = true;
        if (pre_hide_event) ResetEvent(pre_hide_event.Get());
        if (!error && Alive()) {
            bool restored_now = false;
            if (foreign_lease) {
                foreign_restored.store(true, std::memory_order_release);
                if (foreign_lease->Restore()) restored_now = true;
                else if (!IsWindowEnabled(window)) foreign_restored.store(false, std::memory_order_release);
            } else if (cross_thread) {
                if (cross_thread->Restore(false)) restored_now = true;
            } else if (changed) {
                changed = false;
                EnableWindow(window, TRUE);
                restored_now = true;
            }
            if (restored_now && Alive() && IsWindow(window) && IsWindowEnabled(window)) {
                // Host verified dialog-was-foreground and granted AllowSetForegroundWindow
                // before signaling PreHideEvent; activating here on the owner's thread
                // makes the owner foreground BEFORE the host hides its popup.
                SetForegroundWindow(window);
                if (!external) SetActiveWindow(window);
            }
        }
        if (pre_hide_done) SetEvent(pre_hide_done.Get());
    }
    void RestoreBeforeDialogHide(HWND dialog, bool assume_foreground = false) noexcept {
        if(error || !Alive() || !IsWindow(dialog)) return;
        DWORD dialog_pid=0;
        if(GetWindowThreadProcessId(dialog,&dialog_pid)!=GetCurrentThreadId() ||
            dialog_pid!=GetCurrentProcessId() || GetWindow(dialog,GW_OWNER)!=window) return;
        HWND foreground = nullptr;
        const bool was_foreground = assume_foreground || DialogIsForeground(dialog, foreground);
        // Restore an enabled activation target before hiding the active popup.
        // EnableWindow sends messages, so recheck identity before touching focus.
        diagnostics.restore_attempted = true;
        diagnostics.restore_owned_disable = changed || static_cast<bool>(cross_thread) || static_cast<bool>(foreign_lease);
        diagnostics.dialog_was_foreground = was_foreground;
        if (was_foreground && external) {
            AllowSetForegroundWindow(pid);
        }
        if (foreign_lease) {
            foreign_restored.store(true, std::memory_order_release);
            if (!foreign_lease->Restore()) {
                if (!IsWindowEnabled(window)) foreign_restored.store(false, std::memory_order_release);
                return;
            }
        } else if (remote_process && GetPropW(window, LeaseProperty)) {
            if (!ReleaseForeignLease(window, foreign_restored)) return;
        } else if (remote_process && was_foreground) {
            // Ask the caller's STA PickerOwnerGuard to re-enable & activate the
            // owner BEFORE we hide the foreground popup, so Windows never raises
            // the window behind the disabled owner.
            if (RequestCallerPreHideRestore(window, pid, creation, &foreign_restored)) {
                diagnostics.restore_owned_disable = true;
            }
        }
        if (cross_thread && !cross_thread->Restore(false)) return;
        if (changed) {
            changed=false;
            EnableWindow(window,TRUE);
        }
        // Native IFileDialog also activates an originally disabled same-thread
        // owner before hiding. Its caller retains responsibility for enabling it.
        if(was_foreground && Alive()) {
            const HWND now=GetForegroundWindow();
            if(now==foreground || now==dialog || now==window || IsChild(dialog,now)) {
                diagnostics.activation_attempted = true;
                if (external) {
                    // The foreground picker may hand activation back without
                    // changing caller-owned disabled state or attaching queues.
                    if (now != window) SetForegroundWindow(window);
                } else SetActiveWindow(window);
                // Cross-thread foreground activation may complete asynchronously;
                // this diagnostic is an immediate observation, not a failure verdict.
                diagnostics.activation_succeeded = GetForegroundWindow() == window;
                // Activation synchronously calls the host's WM_ACTIVATE and
                // WM_SETFOCUS handlers. They own focus restoration (including
                // choosing a child control or switching to another window).
                // A second SetFocus here overwrites that reentrant decision.
            } else diagnostics.restore_skipped_switched = true;
        } else if (!was_foreground) diagnostics.restore_skipped_switched = true;
    }
    ~Impl() {
        if (foreign_hook) {
            UnhookWinEvent(foreign_hook);
            std::lock_guard lock(foreign_events_mutex);
            foreign_events.erase(foreign_hook);
        }
        if (foreign_registered) { std::lock_guard guard(foreign_mutex); foreign_owners.erase(foreign_key); }
        if (cross_thread) cross_thread->Restore(true, !error);
        if (marked && window && SameWindow() && GetPropW(window, Property) == this) {
            RemovePropW(window, Property);
            if (changed) EnableWindow(window, TRUE); // Only this same-thread guard's transition.
        }
        if (locked) ReleaseMutex(mutex.Get());
    }
};
PickerOwnerGuard::PickerOwnerGuard(HWND window) : impl_(std::make_unique<Impl>(window)) {}
PickerOwnerGuard::PickerOwnerGuard(HWND window, CallerManagedRemoteOwner parent)
    : impl_(std::make_unique<Impl>(window, parent.authenticated_parent_pid)) {}
PickerOwnerGuard::PickerOwnerGuard(HWND window, const wchar_t* executable)
    : impl_(std::make_unique<Impl>(window, 0, executable)) {}
PickerOwnerGuard::~PickerOwnerGuard() = default;
DWORD PickerOwnerGuard::Error() const noexcept { return impl_->error; }
PickerOwnerFailureStage PickerOwnerGuard::FailureStage() const noexcept { return impl_->error ? impl_->stage : PickerOwnerFailureStage::None; }
PickerOwnerIdentityDiagnostics PickerOwnerGuard::Diagnostics() const noexcept { return impl_->diagnostics; }
bool PickerOwnerGuard::Alive() const { return !impl_->error && impl_->Alive(); }
HWND PickerOwnerGuard::Window() const noexcept { return impl_->window; }
void PickerOwnerGuard::RestoreBeforeDialogHide(HWND dialog) noexcept { impl_->RestoreBeforeDialogHide(dialog, false); }
namespace picker_owner_detail {
void RestoreBeforeDialogHideForTest(PickerOwnerGuard& guard, HWND dialog) noexcept {
    if (guard.impl_) guard.impl_->RestoreBeforeDialogHide(dialog, true);
}
}
HANDLE PickerOwnerGuard::PreHideEvent() const noexcept { return impl_->PreHideEvent(); }
void PickerOwnerGuard::CompletePreHideRestore() noexcept { impl_->CompletePreHideRestore(); }
void RestoreExternalPickerOwnerBeforeHide(HWND dialog, HWND owner) noexcept {
    if (!dialog || !owner || !IsWindow(dialog) || !IsWindow(owner)) return;
    DWORD dialog_pid = 0;
    if (GetWindowThreadProcessId(dialog, &dialog_pid) != GetCurrentThreadId() ||
        dialog_pid != GetCurrentProcessId() || GetWindow(dialog, GW_OWNER) != owner) return;
    DWORD owner_pid = 0;
    if (!GetWindowThreadProcessId(owner, &owner_pid) || !owner_pid || owner_pid == GetCurrentProcessId()) return;
    HWND foreground = nullptr;
    if (!DialogIsForeground(dialog, foreground)) return;
    AllowSetForegroundWindow(owner_pid);
    Handle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, owner_pid));
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (proc && GetProcessTimes(proc.Get(), &creation, &exit, &kernel, &user)) {
        (void)RequestCallerPreHideRestore(owner, owner_pid, creation);
    }
    const HWND now = GetForegroundWindow();
    if (IsWindow(owner) && (now == foreground || now == dialog || now == owner || IsChild(dialog, now))) {
        if (now != owner) SetForegroundWindow(owner);
    }
}
}
