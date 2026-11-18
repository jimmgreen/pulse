#pragma once

namespace pulse::shell_bridge::detail {
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
