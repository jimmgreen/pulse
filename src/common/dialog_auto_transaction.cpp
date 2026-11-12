#include "dialog_auto_transaction.h"
#include <new>
#include <string_view>

namespace pulse::dialog_auto {
namespace {
struct MutationLock {
    HANDLE handle = CreateMutexW(nullptr, FALSE, L"Global\\Pulse.DialogAuto.Registration.v1");
    DWORD error = ERROR_SUCCESS;
    bool acquired = false, abandoned = false;
    MutationLock() noexcept {
        if (!handle) { error = GetLastError(); return; }
        const DWORD wait = WaitForSingleObject(handle, 30000);
        acquired = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        abandoned = wait == WAIT_ABANDONED;
        if (!acquired) error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    }
    ~MutationLock() { if (acquired) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
};
bool SamePath(const std::wstring& a, const std::wstring& b) noexcept {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    return a.size() == b.size() && CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
        b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool Reserved(std::wstring_view component) noexcept {
    const auto period = component.find(L'.');
    component = component.substr(0, period);
    const auto equal = [&](std::wstring_view value) {
        return component.size() == value.size() && CompareStringOrdinal(component.data(), static_cast<int>(component.size()),
            value.data(), static_cast<int>(value.size()), TRUE) == CSTR_EQUAL;
    };
    return equal(L"CON") || equal(L"PRN") || equal(L"AUX") || equal(L"NUL") ||
        (component.size() == 4 && component[3] >= L'1' && component[3] <= L'9' &&
            (CompareStringOrdinal(component.data(), 3, L"COM", 3, TRUE) == CSTR_EQUAL ||
             CompareStringOrdinal(component.data(), 3, L"LPT", 3, TRUE) == CSTR_EQUAL));
}
DWORD ValidateTarget(const RegistrationTarget& target) {
    if (target.architecture != Architecture::X64 && target.architecture != Architecture::X86) return ERROR_INVALID_PARAMETER;
    const auto& path = target.dll_path;
    // Match the native registration API's canonical, local DOS path contract.
    if (path.size() < 4 || path.size() >= 32768 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path.find_first_of(L"/<>\"|?*") != std::wstring::npos || path.find(L'\0') != std::wstring::npos ||
        path.find(L':', 2) != std::wstring::npos) return ERROR_INVALID_NAME;
    for (const auto c : path) if (c < 32) return ERROR_INVALID_NAME;
    size_t begin = 3;
    for (size_t n = 3; n <= path.size(); ++n) {
        if (n != path.size() && path[n] != L'\\') continue;
        const std::wstring_view component(path.data() + begin, n - begin);
        if (component.empty() || component == L"." || component == L".." ||
            component.back() == L'.' || component.back() == L' ' || Reserved(component)) return ERROR_INVALID_NAME;
        begin = n + 1;
    }
    const auto leaf = path.substr(path.find_last_of(L'\\') + 1);
    if (!SamePath(leaf, DllNameFor(target.architecture))) return ERROR_INVALID_NAME;
    std::wstring full(32768, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!length) return GetLastError();
    if (length >= full.size()) return ERROR_FILENAME_EXCED_RANGE;
    full.resize(length);
    return SamePath(path, full) ? ERROR_SUCCESS : ERROR_INVALID_NAME;
}
DWORD Inspect(const RegistrationApi& api, Architecture architecture, Status& result) noexcept {
    try { result = api.inspect(architecture); return result.error; }
    catch (const std::bad_alloc&) { result.error = ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { result.error = ERROR_UNHANDLED_EXCEPTION; }
    return result.error;
}
DWORD Change(const std::function<DWORD(Architecture, const std::wstring&)>& call,
    Architecture architecture, const std::wstring& path) noexcept {
    try { return call(architecture, path); }
    catch (const std::bad_alloc&) { return ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { return ERROR_UNHANDLED_EXCEPTION; }
}
bool SameSnapshot(const Status& a, const Status& b) noexcept {
    return a.enabled == b.enabled && SamePath(a.dll_path, b.dll_path) && !a.error && !a.conflict;
}
bool AllowedPath(const RegistrationTargetResult& item, const Status& state) noexcept {
    return state.dll_path.empty() || SamePath(state.dll_path, item.target.dll_path) ||
        (!item.before.dll_path.empty() && SamePath(state.dll_path, item.before.dll_path));
}
bool Restored(const RegistrationTargetResult& item) noexcept {
    if (item.after.error || item.after.conflict || !AllowedPath(item, item.after)) return false;
    return item.before.enabled ? item.after.enabled && SamePath(item.before.dll_path, item.after.dll_path) : !item.after.enabled;
}
void Rollback(RegistrationChangeResult& result, const RegistrationApi& api) noexcept {
    bool complete = true;
    for (auto it = result.targets.rbegin(); it != result.targets.rend(); ++it) {
        auto& item = *it;
        if (!item.attempted) continue;
        Status current;
        DWORD error = Inspect(api, item.target.architecture, current);
        if (!error && !AllowedPath(item, current)) error = ERROR_REVISION_MISMATCH;
        if (!error) {
            // A same-path partial write may be marked conflicting. The native
            // helpers still enforce subtree ownership before modifying it.
            item.rollback_attempted = true; ++result.rollback_attempted_count;
            const auto& bound_path = item.before.dll_path.empty() ? item.target.dll_path : item.before.dll_path;
            error = Change(item.before.enabled ? api.enable : api.disable_for, item.target.architecture, bound_path);
            const DWORD observed = Inspect(api, item.target.architecture, item.after);
            if (!error) error = observed;
            item.behavior_restored = Restored(item);
            if (!error && !item.behavior_restored) error = ERROR_INVALID_DATA;
        }
        item.rollback_error = error;
        if (!item.rollback_attempted) item.after = std::move(current);
        if (error) { complete = false; if (!result.rollback_error) result.rollback_error = error; }
    }
    result.rollback_complete = complete && result.rollback_attempted_count == result.attempted_count;
}
}

RegistrationChangeResult ExecuteRegistrationChange(const std::vector<RegistrationTarget>& targets,
    bool enable, const RegistrationApi& api) noexcept {
    RegistrationChangeResult result;
    MutationLock lock;
    result.abandoned_lock = lock.abandoned;
    if (lock.error) { result.error = lock.error; return result; }
    RegistrationApi resolved;
    try {
        const bool any = api.inspect || api.enable || api.enable_if_unclaimed || api.disable_for;
        if (any && !(api.inspect && api.enable && api.enable_if_unclaimed && api.disable_for)) {
            result.error = ERROR_INVALID_PARAMETER; return result;
        }
        if (any) resolved = api;
        else {
            resolved.inspect = [](Architecture architecture) { return dialog_auto::Inspect(architecture); };
            resolved.enable = [](Architecture architecture, const std::wstring& path) { return dialog_auto::Enable(architecture, path); };
            resolved.enable_if_unclaimed = [](Architecture architecture, const std::wstring& path) { return dialog_auto::EnableIfUnclaimed(architecture, path); };
            resolved.disable_for = [](Architecture architecture, const std::wstring& path) { return dialog_auto::DisableFor(architecture, path); };
        }
        if (targets.empty() || targets.size() > 2) { result.error = ERROR_INVALID_PARAMETER; return result; }
        result.targets.reserve(targets.size());
        for (const auto& target : targets) {
            const DWORD error = ValidateTarget(target);
            if (error) { result.error = error; return result; }
            for (const auto& item : result.targets) if (item.target.architecture == target.architecture) {
                result.error = ERROR_INVALID_PARAMETER; return result;
            }
            result.targets.push_back({target});
        }
        // Complete every snapshot/ownership check before the first mutation.
        for (auto& item : result.targets) {
            DWORD error = Inspect(resolved, item.target.architecture, item.before);
            if (!error && item.before.conflict) error = ERROR_ALREADY_EXISTS;
            if (!error && item.before.enabled && item.before.dll_path.empty()) error = ERROR_INVALID_DATA;
            if (!error && !item.before.dll_path.empty() && ValidateTarget({item.target.architecture, item.before.dll_path})) error = ERROR_INVALID_DATA;
            if (!error && !item.before.dll_path.empty() && !SamePath(item.before.dll_path, item.target.dll_path)) error = ERROR_ALREADY_EXISTS;
            if (!error && !enable && item.before.dll_path.empty()) error = ERROR_NOT_FOUND;
            item.error = error;
            if (error) { result.error = error; return result; }
            item.after = item.before;
        }
        for (auto& item : result.targets) {
            Status current;
            DWORD error = Inspect(resolved, item.target.architecture, current);
            if (!error && (current.conflict || !SameSnapshot(current, item.before))) error = ERROR_REVISION_MISMATCH;
            if (error) item.after = std::move(current);
            if (!error && enable && item.before.enabled) continue;
            if (!error) {
                item.attempted = true; ++result.attempted_count;
                const auto& path = item.before.dll_path.empty() ? item.target.dll_path : item.before.dll_path;
                error = Change(enable ? resolved.enable_if_unclaimed : resolved.disable_for, item.target.architecture, path);
                const DWORD observed = Inspect(resolved, item.target.architecture, item.after);
                if (!error) error = observed;
                if (!error && (item.after.conflict || !AllowedPath(item, item.after))) error = ERROR_REVISION_MISMATCH;
                if (!error && (item.after.enabled != enable || (enable && !SamePath(item.after.dll_path, item.target.dll_path)))) error = ERROR_INVALID_DATA;
            }
            item.error = error;
            if (error) { result.error = error; break; }
        }
    } catch (const std::bad_alloc&) { result.error = ERROR_NOT_ENOUGH_MEMORY; }
    catch (...) { result.error = ERROR_UNHANDLED_EXCEPTION; }
    if (result.error && result.attempted_count) Rollback(result, resolved);
    return result;
}
}
