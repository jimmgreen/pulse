#include "dispatch_adapter.h"
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <windows.h>

namespace pulse::extensions::native {
namespace {
constexpr UINT kDispatch = WM_APP + 173;
constexpr wchar_t kClass[] = L"Pulse.NativeDispatchV1";
std::atomic<uint64_t> next_token{0};
uint64_t AllocateToken() {
    uint64_t previous = next_token.load();
    while (previous != UINT64_MAX) {
        if (next_token.compare_exchange_weak(previous, previous + 1)) return previous + 1;
    }
    return 0;
}
} // namespace
struct DispatchAdapter::State : std::enable_shared_from_this<State> {
    struct Work {
        PulseDispatchCallback callback;
        void *user;
    };
    const DWORD thread = GetCurrentThreadId();
    std::function<bool()> alive;
    std::mutex mutex;
    HWND window = nullptr;
    bool closed = false, notified = false;
    std::map<uint64_t, Work> pending;
    std::atomic<bool> active{false};
    bool WakeLocked() {
        if (closed || pending.empty() || notified || active.load())
            return !closed;
        if (window && PostMessageW(window, kDispatch, 0, 0)) {
            notified = true;
            return true;
        }
        closed = true;
        pending.clear();
        return false;
    }
    void Close() {
        if (GetCurrentThreadId() != thread)
            return;
        HWND old;
        {
            std::lock_guard lock(mutex);
            closed = true;
            notified = false;
            pending.clear();
            old = std::exchange(window, nullptr);
        }
        if (old)
            DestroyWindow(old);
    }
    void Run() {
        auto lifetime = shared_from_this();
        {
            std::lock_guard lock(mutex);
            notified = false;
            if (closed || active.load() || pending.empty())
                return;
        }
        // Owner/authorization access belongs exclusively to the UI thread.
        bool allowed = false;
        try {
            allowed = alive && alive();
        } catch (...) {
        }
        if (!allowed) {
            Close();
            return;
        }
        Work work{};
        {
            std::lock_guard lock(mutex);
            if (closed || active.load() || pending.empty())
                return;
            const auto first = pending.begin();
            work = first->second;
            pending.erase(first);
            // This is the linearization point: cancel now returns STATE, never OK.
            active.store(true);
        }
        try {
            work.callback(work.user);
        } catch (...) {
        }
        {
            std::lock_guard lock(mutex);
            active.store(false);
            WakeLocked();
        }
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto *self = reinterpret_cast<State *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<State *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == kDispatch && self) {
            self->Run();
            return 0;
        }
        if (message == WM_NCDESTROY)
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
    }
    void Create() {
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = Procedure;
        cls.lpszClassName = kClass;
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            closed = true;
            return;
        }
        window = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, this);
        if (!window)
            closed = true;
    }
    static PulseStatus PULSE_CALL Post(void *context, PulseDispatchCallback callback, void *user,
                                       uint64_t *token) noexcept {
        if (!context || !callback || !token)
            return PULSE_ERROR_ARGUMENT;
        *token = 0;
        try {
            auto lifetime = static_cast<State *>(context)->shared_from_this();
            std::lock_guard lock(lifetime->mutex);
            if (lifetime->closed || lifetime->pending.size() >= PULSE_DISPATCH_MAX_PENDING)
                return PULSE_ERROR_STATE;
            const uint64_t id = AllocateToken();
            if (!id)
                return PULSE_ERROR_STATE;
            lifetime->pending.emplace(id, Work{callback, user});
            *token = id;
            if (!lifetime->WakeLocked()) {
                *token = 0;
                return PULSE_ERROR_UNAVAILABLE;
            }
            return PULSE_OK;
        } catch (...) {
            return PULSE_ERROR_INITIALIZATION;
        }
    }
    static PulseStatus PULSE_CALL Cancel(void *context, uint64_t token) noexcept {
        if (!context || !token)
            return PULSE_ERROR_ARGUMENT;
        try {
            auto lifetime = static_cast<State *>(context)->shared_from_this();
            std::lock_guard lock(lifetime->mutex);
            return lifetime->pending.erase(token) ? PULSE_OK : PULSE_ERROR_STATE;
        } catch (...) {
            return PULSE_ERROR_STATE;
        }
    }
};
DispatchAdapter::DispatchAdapter(std::function<bool()> alive) : state_(std::make_shared<State>()) {
    state_->alive = std::move(alive);
    state_->Create();
}
DispatchAdapter::~DispatchAdapter() { Close(); }
PulseStatus DispatchAdapter::Query(uint32_t version, void *output, uint32_t size) {
    if (version != PULSE_DISPATCH_VERSION || !output || size != sizeof(PulseDispatchV1))
        return PULSE_ERROR_ABI;
    if (GetCurrentThreadId() != state_->thread)
        return PULSE_ERROR_STATE;
    std::lock_guard lock(state_->mutex);
    if (state_->closed)
        return PULSE_ERROR_STATE;
    const PulseDispatchV1 table{sizeof(PulseDispatchV1), PULSE_DISPATCH_VERSION, state_.get(), State::Post,
                                State::Cancel};
    std::memcpy(output, &table, sizeof(table));
    return PULSE_OK;
}
void DispatchAdapter::Close() { state_->Close(); }
bool DispatchAdapter::CallbackActive() const { return state_->active.load(); }
} // namespace pulse::extensions::native
