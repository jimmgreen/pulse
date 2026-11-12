#pragma once
#include "dialog_auto_registration.h"
#include <cstddef>
#include <functional>
#include <vector>

namespace pulse::dialog_auto {
struct RegistrationTarget {
    Architecture architecture = Architecture::X64;
    std::wstring dll_path;
};
struct RegistrationApi {
    std::function<Status(Architecture)> inspect;
    std::function<DWORD(Architecture, const std::wstring&)> enable;
    std::function<DWORD(Architecture, const std::wstring&)> enable_if_unclaimed;
    std::function<DWORD(Architecture, const std::wstring&)> disable_for;
};
struct RegistrationTargetResult {
    RegistrationTarget target;
    Status before, after;
    DWORD error = ERROR_SUCCESS, rollback_error = ERROR_SUCCESS;
    bool attempted = false, rollback_attempted = false, behavior_restored = false;
};
struct RegistrationChangeResult {
    DWORD error = ERROR_SUCCESS, rollback_error = ERROR_SUCCESS;
    size_t attempted_count = 0, rollback_attempted_count = 0;
    bool rollback_complete = false, abandoned_lock = false;
    std::vector<RegistrationTargetResult> targets;
};
// One recursive global registration lock spans preflight, changes and rollback.
// All four Api callbacks must be provided, or all absent to use the native API.
// Recovery restores enabled/disabled behavior and ownership constraints, not
// registry bytes: a formerly empty disabled journal may retain the target path.
RegistrationChangeResult ExecuteRegistrationChange(const std::vector<RegistrationTarget>& targets,
    bool enable, const RegistrationApi& api = {}) noexcept;
}
