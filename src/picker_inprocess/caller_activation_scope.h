#pragma once
#include <windows.h>

namespace pulse::picker_inprocess {
// Keep the captured context alive independently of the current activation stack.
class CallerActivation {
public:
    CallerActivation() = default;
    CallerActivation(const CallerActivation&) = delete;
    CallerActivation& operator=(const CallerActivation&) = delete;
    ~CallerActivation() { if (captured_ && handle_) ReleaseActCtx(handle_); }
    bool Capture() noexcept {
        if (captured_) return false;
        captured_ = GetCurrentActCtx(&handle_) != FALSE;
        return captured_;
    }
    bool Push(ULONG_PTR& cookie) const noexcept {
        return captured_ && ActivateActCtx(handle_, &cookie) != FALSE;
    }
private:
    HANDLE handle_ = nullptr;
    bool captured_ = false;
};
class CallerActivationFrame {
public:
    explicit CallerActivationFrame(const CallerActivation& caller) noexcept : entered_(caller.Push(cookie_)) {}
    CallerActivationFrame(const CallerActivationFrame&) = delete;
    CallerActivationFrame& operator=(const CallerActivationFrame&) = delete;
    ~CallerActivationFrame() { if (entered_) DeactivateActCtx(0, cookie_); }
    bool Entered() const noexcept { return entered_; }
private:
    ULONG_PTR cookie_ = 0;
    bool entered_ = false;
};
}
