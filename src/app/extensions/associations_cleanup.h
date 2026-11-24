#pragma once

#include <functional>
#include <mutex>
#include <string_view>

namespace pulse::extensions {

namespace detail {
inline auto& AssociationsCleanupMutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::function<void(std::string_view)>& AssociationsCleanupSlot() {
    static std::function<void(std::string_view)> hook;
    return hook;
}
} // namespace detail

inline void SetPackageAssociationsCleanupHook(std::function<void(std::string_view)> hook) {
    std::lock_guard lock(detail::AssociationsCleanupMutex());
    detail::AssociationsCleanupSlot() = std::move(hook);
}

inline void NotifyPackageAssociationsCleanup(std::string_view package_id) {
    if (package_id.empty()) return;
    std::function<void(std::string_view)> hook;
    {
        std::lock_guard lock(detail::AssociationsCleanupMutex());
        hook = detail::AssociationsCleanupSlot();
    }
    if (hook) {
        try {
            hook(package_id);
        } catch (...) {}
    }
}

} // namespace pulse::extensions
