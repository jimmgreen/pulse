#include "win_e_hotkey.h"
#include "win_e_trace.h"
#include "win_e_broker_client.h"
#include "../ipc/elevated_transfer_transport.h"
#include "../common/runtime_log.h"
#include <cstring>
#include <objbase.h>
#include <new>

namespace pulse::app {
namespace {
constexpr UINT kChordResult = WM_APP + 1;

bool Down(int key) noexcept { return (GetAsyncKeyState(key) & 0x8000) != 0; }
unsigned AsyncModifiers() noexcept {
    unsigned value = 0;
    for (const auto key : {VK_LWIN, VK_RWIN, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT})
        if (Down(key)) value |= WinEInputState::Modifier(key);
    return value;
}
DWORD OpenNativeExplorer() {
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(directory, ARRAYSIZE(directory));
    if (!length || length >= ARRAYSIZE(directory)) return ERROR_PATH_NOT_FOUND;
    const std::wstring executable = std::wstring(directory) + L"\\explorer.exe";
    // An explicit native This PC browse avoids the default Win+E verb, which
    // may route back to Pulse. A bare explorer.exe can return without a window.
    auto command = L"\"" + executable + L"\" /n,/e,::{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return GetLastError();
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return ERROR_SUCCESS;
}
struct DeliveryWatch {
    uint64_t high = 0, low = 0;
    std::shared_ptr<std::atomic<SingleInstanceCoordinator::OpenDeliveryState>> state;
};
void ReportDelivery(const DeliveryWatch& delivery, bool fallback, DWORD error) {
    diagnostics::runtime::Event("win_e_delivery", {{"state", static_cast<uint64_t>(delivery.state->load())},
        {"id_high", delivery.high}, {"id_low", delivery.low}, {"native_fallback", fallback}, {"error", error}});
}
DWORD WINAPI FinishDelivery(void* value) {
    std::unique_ptr<DeliveryWatch> delivery(static_cast<DeliveryWatch*>(value));
    auto state = delivery->state->load();
    while (state == SingleInstanceCoordinator::OpenDeliveryState::Pending ||
           state == SingleInstanceCoordinator::OpenDeliveryState::Queued) {
        Sleep(100);
        state = delivery->state->load();
    }
    const bool fallback = state == SingleInstanceCoordinator::OpenDeliveryState::Cancelled;
    DWORD error = ERROR_SUCCESS;
    if (fallback) {
        try { error = OpenNativeExplorer(); } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
    }
    ReportDelivery(*delivery, fallback, error);
    return 0;
}
void WatchDelivery(const std::array<unsigned char, 16>& id,
                   const std::shared_ptr<std::atomic<SingleInstanceCoordinator::OpenDeliveryState>>& state) {
    DeliveryWatch delivery;
    std::memcpy(&delivery.high, id.data(), sizeof(delivery.high));
    std::memcpy(&delivery.low, id.data() + sizeof(delivery.high), sizeof(delivery.low));
    delivery.state = state;
    auto* owned = new (std::nothrow) DeliveryWatch(delivery);
    if (!owned) { ReportDelivery(delivery, false, ERROR_NOT_ENOUGH_MEMORY); return; }
    HANDLE worker = CreateThread(nullptr, 0, FinishDelivery, owned, 0, nullptr);
    if (!worker) { const auto error = GetLastError(); delete owned; ReportDelivery(delivery, false, error); }
    else CloseHandle(worker);
}
}
WinEHotkey::WinEHotkey(SingleInstanceCoordinator& coordinator, HWND host, UINT wake, bool elevated_support)
    : coordinator_(coordinator), host_(host), wake_(wake), elevated_support_(elevated_support) {
    thread_ = CreateThread(nullptr, 0, Thread, this, 0, nullptr);
    if (!thread_) diagnostics::runtime::Event("win_e_hook", {{"enabled", 0}, {"error", GetLastError()}});
}
WinEHotkey::~WinEHotkey() { Stop(); }
void WinEHotkey::Stop() {
    stopping_ = true;
    if (!thread_) return;
    if (const auto id = thread_id_.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
    // No callback performs I/O or waits on the UI; join before the coordinator dies.
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_); thread_ = nullptr;
}
void WinEHotkey::Prepare() noexcept {
    prepared_ = false;
    try {
        SingleInstanceCoordinator::OpenRequest next;
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) return;
        static_assert(sizeof(id) == sizeof(next.id));
        std::memcpy(next.id.data(), &id, sizeof(id));
        next.path = L"pulse:win-e";
        next.delivery = std::make_shared<std::atomic<SingleInstanceCoordinator::OpenDeliveryState>>(
            SingleInstanceCoordinator::OpenDeliveryState::Pending);
        prepared_delivery_ = next.delivery;
        request_ = std::move(next);
        prepared_ = true;
    } catch (...) {}
}
void WinEHotkey::FinishChord() noexcept {
    if (!chord_pending_) return;
    chord_pending_ = false;
    if (pending_accepted_ && prepared_delivery_) {
        try { deliveries_.push_back({request_.id, prepared_delivery_}); }
        catch (...) { WatchDelivery(request_.id, prepared_delivery_); }
    }
    Prepare();
}
void WinEHotkey::RefreshHook(HWND foreground, unsigned reason) noexcept {
    if (stopping_ || !foreground || foreground == last_foreground_) return;
    const auto now = GetTickCount64();
    // Coalesce bursts, but retain a timer retry if foreground events are unavailable.
    if (refresh_tick_ && now - refresh_tick_ < 100) return;
    refresh_tick_ = now;
    HHOOK replacement = SetWindowsHookExW(WH_KEYBOARD_LL, Keyboard, GetModuleHandleW(nullptr), 0);
    const DWORD error = replacement ? ERROR_SUCCESS : GetLastError();
    if (replacement) {
        HHOOK previous = hook_;
        hook_ = replacement;
        if (previous) UnhookWindowsHookEx(previous);
        last_foreground_ = foreground;
        ++hook_generation_;
        active_ = true;
    }
    // Preserve the pressed/accepted chord across refresh; its repeats and keyup
    // still belong to the same owner even if foreground moved during the press.
    if (trace_) {
        WinETraceRecord record;
        record.kind = WinETraceRecord::Hook; record.foreground = foreground;
        record.tick = now; record.generation = hook_generation_;
        record.result = replacement != nullptr; record.error = error; record.phase = reason;
        trace_->Push(record);
    }
}
void CALLBACK WinEHotkey::Foreground(HWINEVENTHOOK, DWORD event, HWND window, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && current_) current_->RefreshHook(window, 1);
}
DWORD WINAPI WinEHotkey::Thread(void* value) {
    auto& self = *static_cast<WinEHotkey*>(value);
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    self.thread_id_ = GetCurrentThreadId();
    if (self.stopping_) return 0;
    current_ = &self;
    WinETrace trace;
    self.trace_ = &trace;
    self.input_.chord.down = Down('E');
    self.input_.modifiers = AsyncModifiers();
    self.Prepare();
    self.hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, Keyboard, GetModuleHandleW(nullptr), 0);
    self.active_ = self.hook_ != nullptr;
    diagnostics::runtime::Event("win_e_hook", {{"enabled", self.active_.load()},
        {"error", self.hook_ ? ERROR_SUCCESS : GetLastError()}});
    std::unique_ptr<WinEBrokerClient> broker;
    if (self.hook_) {
        if (self.elevated_support_ && !elevated::IsElevated(GetCurrentProcess())) {
            try { broker = std::make_unique<WinEBrokerClient>(self.host_); self.broker_ = broker.get(); }
            catch (...) { diagnostics::runtime::Event("win_e_broker_start", {{"success", 0}, {"error", ERROR_NOT_ENOUGH_MEMORY}}); }
        }
        self.hook_generation_ = 1;
        self.last_foreground_ = GetForegroundWindow();
        self.foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, Foreground, 0, 0, WINEVENT_OUTOFCONTEXT);
        diagnostics::runtime::Event("win_e_foreground_watch", {{"enabled", self.foreground_hook_ != nullptr},
            {"error", self.foreground_hook_ ? ERROR_SUCCESS : GetLastError()}});
        const auto timer = SetTimer(nullptr, 0, 250, nullptr);
        while (!self.stopping_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!message.hwnd && message.message == kChordResult) {
                self.FinishChord();
            } else if (!message.hwnd && message.message == WM_TIMER) {
                self.FinishChord();
                if (broker) broker->Poll(self.coordinator_, self.host_, self.wake_,
                    !self.input_.chord.down && !Down('E') && !Down(VK_LWIN) && !Down(VK_RWIN));
                self.RefreshHook(GetForegroundWindow(), 2);
                std::erase_if(self.deliveries_, [](const Delivery& delivery) {
                    const auto state = delivery.state->load();
                    if (state == SingleInstanceCoordinator::OpenDeliveryState::Pending ||
                        state == SingleInstanceCoordinator::OpenDeliveryState::Queued) return false;
                    uint64_t id_high = 0, id_low = 0;
                    std::memcpy(&id_high, delivery.id.data(), sizeof(id_high));
                    std::memcpy(&id_low, delivery.id.data() + sizeof(id_high), sizeof(id_low));
                    if (state == SingleInstanceCoordinator::OpenDeliveryState::Cancelled)
                        WatchDelivery(delivery.id, delivery.state);
                    else diagnostics::runtime::Event("win_e_delivery", {{"state", static_cast<uint64_t>(state)},
                        {"id_high", id_high}, {"id_low", id_low}, {"native_fallback", 0}, {"error", 0}});
                    return true;
                });
            } else {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
        }
        if (self.foreground_hook_) { UnhookWinEvent(self.foreground_hook_); self.foreground_hook_ = nullptr; }
        if (timer) KillTimer(nullptr, timer);
        UnhookWindowsHookEx(self.hook_); self.hook_ = nullptr;
    }
    // Turning the switch off removes the hook immediately, but an accepted
    // request still owns its delivery result. The monitor owns no AppCore data.
    for (const auto& delivery : self.deliveries_) {
        auto queued = SingleInstanceCoordinator::OpenDeliveryState::Queued;
        delivery.state->compare_exchange_strong(queued, SingleInstanceCoordinator::OpenDeliveryState::Cancelled);
        WatchDelivery(delivery.id, delivery.state);
    }
    if (!self.prepared_ && self.prepared_delivery_ &&
        self.prepared_delivery_->load() != SingleInstanceCoordinator::OpenDeliveryState::Pending) {
        auto queued = SingleInstanceCoordinator::OpenDeliveryState::Queued;
        self.prepared_delivery_->compare_exchange_strong(queued, SingleInstanceCoordinator::OpenDeliveryState::Cancelled);
        WatchDelivery(self.request_.id, self.prepared_delivery_);
    }
    self.broker_ = nullptr; broker.reset();
    self.trace_ = nullptr;
    self.active_ = false; current_ = nullptr;
    diagnostics::runtime::Event("win_e_hook", {{"enabled", 0}, {"error", 0}, {"reason", self.stopping_ ? 1u : 2u}});
    return 0;
}
LRESULT CALLBACK WinEHotkey::Keyboard(int code, WPARAM message, LPARAM data) {
    auto* self = current_;
    if (code != HC_ACTION || !self || self->stopping_) return CallNextHookEx(nullptr, code, message, data);
    const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
    const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool released = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!pressed && !released) return CallNextHookEx(nullptr, code, message, data);
    if (self->broker_) {
        const bool active = self->broker_->Active();
        if (!active && self->broker_mode_) {
            const auto held = self->broker_->HeldFlags();
            self->input_.chord.down = (held & 1) != 0;
            self->input_.chord.accepted = (held & 2) != 0;
        }
        self->broker_mode_ = active;
        if (active && !self->input_.chord.accepted) {
            if (key.vkCode == 'E') {
                if (released) self->input_.chord.Release();
                else self->input_.chord.Press(false);
            }
            return CallNextHookEx(nullptr, code, message, data);
        }
    }
    const auto modifier = WinEInputState::Modifier(key.vkCode);
    if (modifier) self->input_.ObserveModifier(key.vkCode, pressed);
    const bool win = key.vkCode == VK_LWIN || key.vkCode == VK_RWIN;
    if (key.vkCode != 'E' && !win) return CallNextHookEx(nullptr, code, message, data);
    if (key.vkCode == 'E') self->trace_->CountE(pressed);
    const auto asynchronous = AsyncModifiers();
    WinETraceRecord record;
    record.foreground = GetForegroundWindow(); record.tick = GetTickCount64();
    record.input_time = key.time; record.flags = key.flags;
    record.observed = self->input_.modifiers; record.asynchronous = asynchronous;
    record.generation = self->hook_generation_;
    if (win) {
        record.cycle = self->cycle_; record.phase = pressed ? 1u : 2u;
        self->trace_->Push(record);
        return CallNextHookEx(nullptr, code, message, data);
    }
    auto& chord = self->input_.chord;
    if (released) {
        const bool accepted = chord.Release();
        if (self->trace_chord_) {
            record.cycle = self->cycle_; record.phase = 5;
            record.result = accepted; record.action = accepted ? 2u : 0u;
            self->trace_->Push(record);
        }
        self->trace_chord_ = false;
        return accepted ? 1 : CallNextHookEx(nullptr, code, message, data);
    }
    const bool repeated = chord.down;
    if (!repeated) {
        self->trace_chord_ = ((self->input_.modifiers | asynchronous) & 3) != 0;
        if (self->trace_chord_) ++self->cycle_;
    }
    const auto action = chord.Press(self->input_.Eligible(asynchronous));
    record.cycle = self->cycle_; record.phase = repeated ? 4u : 3u;
    record.action = static_cast<uint32_t>(action);
    if (action == WinEChord::Action::Submit) {
        WinETraceRecord submission = record;
        submission.kind = WinETraceRecord::Submission;
        submission.action = self->prepared_;
        if (self->prepared_) {
            std::memcpy(&submission.id_high, self->request_.id.data(), sizeof(submission.id_high));
            std::memcpy(&submission.id_low, self->request_.id.data() + sizeof(submission.id_high), sizeof(submission.id_low));
        }
        const bool accepted = self->prepared_ && self->coordinator_.TrySubmitKeyboardOpen(
            self->request_, record.tick, self->host_, self->wake_);
        self->prepared_ = false;
        chord.accepted = accepted;
        // A second physical press can arrive before our posted completion. It
        // may fall back, but must not overwrite the first accepted delivery.
        if (!self->chord_pending_) {
            self->pending_accepted_ = accepted;
            self->chord_pending_ = true;
        }
        UINT masked = 0;
        if (accepted) {
            INPUT mask[2]{};
            mask[0].type = mask[1].type = INPUT_KEYBOARD;
            mask[0].ki.wVk = mask[1].ki.wVk = 0xE8;
            mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
            masked = SendInput(2, mask, sizeof(INPUT));
        }
        submission.result = accepted; submission.phase = masked;
        if (!PostThreadMessageW(self->thread_id_.load(), kChordResult, 0, 0)) submission.error = GetLastError();
        self->trace_->Push(submission);
    }
    record.result = chord.accepted;
    if (self->trace_chord_) self->trace_->Push(record);
    return chord.accepted ? 1 : CallNextHookEx(nullptr, code, message, data);
}
}
