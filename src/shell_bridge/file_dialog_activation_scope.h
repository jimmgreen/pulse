#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <cstdio>

namespace pulse::shell_bridge::detail {
// These modifiers leave the server location in-process. The original COM call
// still receives every bit and decides whether the combination is valid.
inline bool InprocActivationContext(DWORD context) noexcept {
    constexpr DWORD modifiers = CLSCTX_NO_CODE_DOWNLOAD | CLSCTX_NO_FAILURE_LOG | CLSCTX_DISABLE_AAA;
    return (context & CLSCTX_INPROC_SERVER) && !(context & ~(CLSCTX_INPROC_SERVER | modifiers));
}
// Record only the outer Open/Save activation. In particular a successful native
// COM call is not evidence that a wrapper was published or that Show was used.
template<class Observer>
void TraceActivation(const Observer& observer, bool nested, REFCLSID clsid, const char* stage,
                     DWORD context, HRESULT hr) noexcept {
    if (!observer || nested || (clsid != CLSID_FileOpenDialog && clsid != CLSID_FileSaveDialog)) return;
    const DWORD error = GetLastError();
    char text[112]{};
    std::snprintf(text, sizeof(text), "%s context=0x%08lX hr=0x%08lX", stage, context, static_cast<unsigned long>(hr));
    try { observer(text, nullptr); } catch (...) {}
    SetLastError(error);
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
