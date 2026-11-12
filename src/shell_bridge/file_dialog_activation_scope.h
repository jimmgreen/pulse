#pragma once
#include <windows.h>

namespace pulse::shell_bridge::detail {
// These modifiers leave the server location in-process. The original COM call
// still receives every bit and decides whether the combination is valid.
inline bool InprocActivationContext(DWORD context) noexcept {
    constexpr DWORD modifiers = CLSCTX_NO_CODE_DOWNLOAD | CLSCTX_NO_FAILURE_LOG | CLSCTX_DISABLE_AAA;
    return (context & CLSCTX_INPROC_SERVER) && !(context & ~(CLSCTX_INPROC_SERVER | modifiers));
}
// Shared by the single-IID and MULTI_QI entry points in this linked module.
// Original activation may delegate across APIs; only the outer entry wraps.
inline thread_local bool activation_active = false;
struct ActivationScope {
    bool nested = activation_active;
    ActivationScope() { activation_active = true; }
    ~ActivationScope() { activation_active = nested; }
    ActivationScope(const ActivationScope&) = delete;
    ActivationScope& operator=(const ActivationScope&) = delete;
};
}
