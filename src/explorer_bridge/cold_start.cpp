#include "cold_start.h"
#include <algorithm>

namespace pulse::explorer_bridge::cold_start {
Outcome Deliver(const browse::Request& request, Hooks& hooks, DWORD ready_timeout_ms) noexcept {
    if (hooks.Find() == EndpointState::Ready) return hooks.Submit(request, false, hooks.Now() + 1000);
    if (!ready_timeout_ms) return {DirectRevealResult::Rejected, ERROR_TIMEOUT};
    const uint64_t started = hooks.Now();
    const auto remaining = [&]() -> DWORD {
        const uint64_t elapsed = hooks.Now() - started;
        return elapsed >= ready_timeout_ms ? 0 : ready_timeout_ms - static_cast<DWORD>(elapsed);
    };
    const DWORD lock_error = hooks.Acquire(remaining());
    if (lock_error) return {DirectRevealResult::Rejected, lock_error};
    struct Release {
        Hooks& hooks;
        bool held = true;
        ~Release() { if (held) hooks.Release(); }
    } release{hooks};
    auto state = hooks.Find();
    if (state != EndpointState::Ready && !remaining()) return {DirectRevealResult::Rejected, ERROR_TIMEOUT};
    if (state == EndpointState::Absent) {
        const DWORD error = hooks.Start();
        if (error) return {DirectRevealResult::Rejected, error};
    }
    while (state != EndpointState::Ready) {
        const DWORD wait = remaining();
        if (!wait) return {DirectRevealResult::Rejected, ERROR_TIMEOUT};
        hooks.Pause(std::min<DWORD>(25, wait));
        state = hooks.Find();
    }
    hooks.Release();
    release.held = false;
    if (!remaining()) return {DirectRevealResult::Rejected, ERROR_TIMEOUT};
    return hooks.Submit(request, true, started + ready_timeout_ms);
}
}
