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
    DWORD integrity = 0;
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
    return true;
}
}
struct PickerOwnerGuard::Impl {
    static constexpr const wchar_t* Property = L"Pulse.PickerClient.OwnerGuard";
    HWND window = nullptr;
    bool changed = false, external = false, locked = false, marked = false;
    DWORD pid = 0, thread = 0;
    FILETIME creation{};
    Handle process, mutex;
    std::shared_ptr<ThreadOwner> cross_thread;
    DWORD error = ERROR_SUCCESS;
    PickerOwnerFailureStage stage = PickerOwnerFailureStage::None;
    PickerOwnerIdentityDiagnostics diagnostics;
    std::wstring foreign_key;
    bool foreign_registered = false;
    HWINEVENTHOOK foreign_hook = nullptr;
    std::shared_ptr<ForeignLifetime> foreign_lifetime;
    explicit Impl(HWND value, DWORD remote_parent = 0) : window(value) {
        if (!window) return;
        stage = PickerOwnerFailureStage::Window;
        thread = GetWindowThreadProcessId(window, &pid);
        if (!thread || !pid || !IsWindow(window)) { error = ERROR_INVALID_WINDOW_HANDLE; return; }
        if (remote_parent && (pid != remote_parent || pid == GetCurrentProcessId())) { error = ERROR_ACCESS_DENIED; return; }
        stage = PickerOwnerFailureStage::OpenProcess;
        process.Reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
        if (!process) { error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return; }
        stage = PickerOwnerFailureStage::ProcessTime;
        FILETIME exit{}, kernel{}, user{};
        if (!GetProcessTimes(process.Get(), &creation, &exit, &kernel, &user)) { error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return; }
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
            // Foreign owners are only observed. A medium-or-higher caller may
            // observe its own user's lower-integrity disabled window, never the
            // reverse. Same-process transitions retain their equal-IL contract.
            const bool foreign = pid != GetCurrentProcessId();
            if (foreign ? (own.integrity < SECURITY_MANDATORY_MEDIUM_RID || other.integrity > own.integrity)
                        : own.integrity != other.integrity) {
                error = ERROR_ACCESS_DENIED; return;
            }
            // Cross-thread EnableWindow sends synchronous messages. Caller-managed
            // already-disabled owners are supported without sending any message.
            if (pid != GetCurrentProcessId() && IsWindowEnabled(window)) {
                OutputDebugStringW(L"Pulse PickerClient: external owner is enabled; caller-managed disabled owner required (ERROR_NOT_SUPPORTED).\n");
                stage = PickerOwnerFailureStage::ForeignEnabled;
                error = ERROR_NOT_SUPPORTED; return;
            }
        }
        const auto name = L"Local\\Pulse.PickerClient.Owner." + std::to_wstring(pid) + L"." +
            std::to_wstring(creation.dwHighDateTime) + L"." + std::to_wstring(creation.dwLowDateTime) + L"." +
            std::to_wstring(reinterpret_cast<UINT_PTR>(window));
        if (!remote_parent) {
        stage = PickerOwnerFailureStage::SecurityAttributes;
        CurrentUserSecurityAttributes security;
        if (!security) { error = ERROR_ACCESS_DENIED; return; }
        stage = PickerOwnerFailureStage::MutexCreate;
        mutex.Reset(CreateMutexW(security.get(), FALSE, name.c_str()));
        if (!mutex) { error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; return; }
        stage = PickerOwnerFailureStage::MutexAcquire;
        const DWORD acquired = WaitForSingleObject(mutex.Get(), 0);
        if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) {
            error = acquired == WAIT_TIMEOUT ? ERROR_BUSY : GetLastError();
            if (!error) error = ERROR_GEN_FAILURE; return;
        }
        locked = true;
        }
        stage = PickerOwnerFailureStage::WindowIdentity;
        if (!SameWindow()) { error = ERROR_INVALID_WINDOW_HANDLE; return; }
        if (pid != GetCurrentProcessId()) {
            if (IsWindowEnabled(window)) { stage = PickerOwnerFailureStage::ForeignEnabled; error = ERROR_NOT_SUPPORTED; return; }
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
            return;
        }
        changed = !external && IsWindowEnabled(window);
        if (changed) EnableWindow(window, FALSE);
    }
    bool SameWindow() const {
        if (!window) return true;
        DWORD current_pid = 0;
        FILETIME started{}, exit{}, kernel{}, user{};
        return process && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT && IsWindow(window) &&
            GetWindowThreadProcessId(window, &current_pid) == thread && current_pid == pid &&
            GetProcessTimes(process.Get(), &started, &exit, &kernel, &user) &&
            CompareFileTime(&creation, &started) == 0;
    }
    bool Alive() const {
        if (foreign_registered) {
            // Out-of-context events are delivered on the creating STA as it pumps.
            // PID/TID/creation checks remain necessary before queued events arrive.
            return foreign_lifetime && !foreign_lifetime->destroyed.load() && SameWindow() && !IsWindowEnabled(window);
        }
        return !window || (marked && SameWindow() &&
            GetPropW(window, Property) == (cross_thread ? static_cast<void*>(cross_thread.get()) : static_cast<void*>(const_cast<Impl*>(this))) &&
            (!external || (cross_thread && cross_thread->restored.load()) || !IsWindowEnabled(window)));
    }
    void RestoreBeforeDialogHide(HWND dialog) noexcept {
        if (cross_thread) {
            if (!error && Alive() && IsWindow(dialog) && GetWindow(dialog, GW_OWNER) == window)
                cross_thread->Restore(false);
            return;
        }
        if(error || external || !changed || GetCurrentThreadId()!=thread || !Alive() || !IsWindow(dialog)) return;
        DWORD dialog_pid=0;
        if(GetWindowThreadProcessId(dialog,&dialog_pid)!=thread || dialog_pid!=pid || GetWindow(dialog,GW_OWNER)!=window) return;
        const HWND foreground=GetForegroundWindow();
        bool was_foreground=foreground==dialog || IsChild(dialog,foreground);
        for(HWND current=foreground; !was_foreground && current; current=GetWindow(current,GW_OWNER))
            was_foreground=current==dialog;
        // Restore an enabled activation target before hiding the active popup.
        // EnableWindow sends messages, so recheck identity before touching focus.
        changed=false;
        EnableWindow(window,TRUE);
        if(was_foreground && Alive() && IsWindowEnabled(window)) {
            const HWND now=GetForegroundWindow();
            if(now==foreground || now==dialog || now==window || IsChild(dialog,now)) {
                SetActiveWindow(window);
                // Activation synchronously calls the host's WM_ACTIVATE and
                // WM_SETFOCUS handlers. They own focus restoration (including
                // choosing a child control or switching to another window).
                // A second SetFocus here overwrites that reentrant decision.
            }
        }
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
PickerOwnerGuard::~PickerOwnerGuard() = default;
DWORD PickerOwnerGuard::Error() const noexcept { return impl_->error; }
PickerOwnerFailureStage PickerOwnerGuard::FailureStage() const noexcept { return impl_->error ? impl_->stage : PickerOwnerFailureStage::None; }
PickerOwnerIdentityDiagnostics PickerOwnerGuard::Diagnostics() const noexcept { return impl_->diagnostics; }
bool PickerOwnerGuard::Alive() const { return !impl_->error && impl_->Alive(); }
HWND PickerOwnerGuard::Window() const noexcept { return impl_->window; }
void PickerOwnerGuard::RestoreBeforeDialogHide(HWND dialog) noexcept { impl_->RestoreBeforeDialogHide(dialog); }
}
