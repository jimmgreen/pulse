#include "dialog_auto_binding.h"
#include "process_token_restriction.h"
#include <cstring>
#include <cwchar>

namespace pulse::dialog_auto_binding {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
DWORD Error() noexcept { const DWORD error = GetLastError(); return error ? error : ERROR_GEN_FAILURE; }
bool SameTime(FILETIME left, FILETIME right) noexcept {
    return left.dwHighDateTime == right.dwHighDateTime && left.dwLowDateTime == right.dwLowDateTime;
}
bool NonzeroTime(FILETIME value) noexcept { return value.dwHighDateTime || value.dwLowDateTime; }
bool NonzeroGuid(const GUID& value) noexcept {
    const GUID zero{};
    return std::memcmp(&value, &zero, sizeof(value)) != 0;
}
DWORD ValidateIdentity(const ProcessIdentityV1& value) noexcept {
    if (!value.pid || !NonzeroTime(value.created) || value.session_id == MAXDWORD ||
        value.sid_size < 8 || value.sid_size > SECURITY_MAX_SID_SIZE || value.user_sid[0] != SID_REVISION ||
        value.user_sid[1] > SID_MAX_SUB_AUTHORITIES || value.sid_size != 8u + 4u * value.user_sid[1]) return ERROR_INVALID_DATA;
    if (!IsValidSid(const_cast<BYTE*>(value.user_sid)) || GetLengthSid(const_cast<BYTE*>(value.user_sid)) != value.sid_size) return ERROR_INVALID_SID;
    for (size_t i = value.sid_size; i < sizeof(value.user_sid); ++i) if (value.user_sid[i]) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
bool SameUserSession(const ProcessIdentityV1& left, const ProcessIdentityV1& right) noexcept {
    return left.session_id == right.session_id && left.sid_size == right.sid_size &&
        std::memcmp(left.user_sid, right.user_sid, left.sid_size) == 0;
}
DWORD LiveHandle(HANDLE process) noexcept {
    if (!process) return ERROR_INVALID_HANDLE;
    const DWORD wait = WaitForSingleObject(process, 0);
    if (wait == WAIT_TIMEOUT) return ERROR_SUCCESS;
    if (wait == WAIT_OBJECT_0) return ERROR_PROCESS_ABORTED;
    return wait == WAIT_FAILED ? Error() : ERROR_GEN_FAILURE;
}
template<class T> DWORD ReadToken(HANDLE token, TOKEN_INFORMATION_CLASS type, T& value) noexcept {
    DWORD returned = 0;
    if (!GetTokenInformation(token, type, &value, sizeof(value), &returned)) return Error();
    return returned == sizeof(value) ? ERROR_SUCCESS : ERROR_BAD_LENGTH;
}
DWORD CaptureIdentity(HANDLE process, ProcessIdentityV1& output) noexcept {
    ProcessIdentityV1 value{};
    value.pid = GetProcessId(process);
    if (!value.pid) return Error();
    FILETIME exited{}, kernel{}, user_time{};
    if (!GetProcessTimes(process, &value.created, &exited, &kernel, &user_time)) return Error();
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return Error();
    if (const DWORD error = pulse::security::CheckUnrestrictedToken(token.value)) return error;
    TOKEN_ELEVATION elevation{};
    if (const DWORD error = ReadToken(token.value, TokenElevation, elevation)) return error;
    if (!elevation.TokenIsElevated) return ERROR_ELEVATION_REQUIRED;
    DWORD container = 0;
    if (const DWORD error = ReadToken(token.value, TokenIsAppContainer, container)) return error;
    if (container) return ERROR_ACCESS_DENIED;
    if (const DWORD error = ReadToken(token.value, TokenSessionId, value.session_id)) return error;
    alignas(TOKEN_USER) BYTE user_buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
    DWORD returned = 0;
    if (!GetTokenInformation(token.value, TokenUser, user_buffer, sizeof(user_buffer), &returned)) return Error();
    if (returned < sizeof(TOKEN_USER) || returned > sizeof(user_buffer)) return ERROR_BAD_LENGTH;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(user_buffer);
    const uintptr_t base = reinterpret_cast<uintptr_t>(user_buffer);
    const uintptr_t sid = reinterpret_cast<uintptr_t>(user->User.Sid);
    if (sid < base || sid - base > returned || returned - (sid - base) < 8) return ERROR_INVALID_SID;
    const auto* bytes = static_cast<const BYTE*>(user->User.Sid);
    if (bytes[0] != SID_REVISION || bytes[1] > SID_MAX_SUB_AUTHORITIES) return ERROR_INVALID_SID;
    value.sid_size = 8u + 4u * bytes[1];
    if (value.sid_size > returned - (sid - base)) return ERROR_INVALID_SID;
    std::memcpy(value.user_sid, bytes, value.sid_size);
    if (const DWORD error = ValidateIdentity(value)) return error;
    output = value;
    return ERROR_SUCCESS;
}
}
DWORD ValidateFormat(const BindingV1& value) noexcept {
    if (value.magic != kMagic || value.version != kVersion || value.size != sizeof(value) || value.reserved ||
        value.reserved_tail || !value.generation || !value.helper_pid || !NonzeroTime(value.helper_created) ||
        !NonzeroGuid(value.nonce)) return ERROR_INVALID_DATA;
    return ValidateIdentity(value.target);
}
DWORD MatchTarget(const BindingV1& bound, const ProcessIdentityV1& current) noexcept {
    if (const DWORD error = ValidateFormat(bound)) return error;
    if (const DWORD error = ValidateIdentity(current)) return error;
    if (bound.target.pid != current.pid || !SameTime(bound.target.created, current.created)) return ERROR_INVALID_DATA;
    return SameUserSession(bound.target, current) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
DWORD ValidateLive(const BindingV1& bound, const BindingV1& observed,
    bool helper_alive, bool enabled, uint64_t current_generation) noexcept {
    if (const DWORD error = ValidateFormat(bound)) return error;
    if (const DWORD error = ValidateFormat(observed)) return error;
    if (!helper_alive) return ERROR_PROCESS_ABORTED;
    if (!enabled) return ERROR_ACCESS_DISABLED_BY_POLICY;
    if (!current_generation || bound.generation != current_generation || observed.generation != current_generation)
        return ERROR_REVISION_MISMATCH;
    if (const DWORD error = MatchTarget(bound, observed.target)) return error;
    if (bound.helper_pid != observed.helper_pid || !SameTime(bound.helper_created, observed.helper_created) ||
        std::memcmp(&bound.nonce, &observed.nonce, sizeof(bound.nonce)) != 0) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
DWORD LocatorBasename(const ProcessIdentityV1& identity, std::wstring& output) noexcept {
    output.clear();
    if (const DWORD error = ValidateIdentity(identity)) return error;
    try {
        wchar_t name[96]{};
        if (swprintf_s(name, L"%lu-%lu-%08lX%08lX", identity.session_id, identity.pid,
            identity.created.dwHighDateTime, identity.created.dwLowDateTime) < 0) return ERROR_INVALID_DATA;
        output = name;
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
DWORD CaptureProcessWithHelper(HANDLE target, HANDLE helper, uint64_t generation,
    const GUID& nonce, BindingV1& output) noexcept {
    output = {};
    if (!generation || !NonzeroGuid(nonce)) return ERROR_INVALID_PARAMETER;
    if (const DWORD error = LiveHandle(target)) return error;
    if (const DWORD error = LiveHandle(helper)) return error;
    Handle impersonation;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &impersonation.value)) return ERROR_ACCESS_DENIED;
    const DWORD thread_error = GetLastError();
    if (thread_error != ERROR_NO_TOKEN) return thread_error ? thread_error : ERROR_GEN_FAILURE;
    ProcessIdentityV1 caller{}, target_identity{}, helper_identity{};
    if (const DWORD error = CaptureIdentity(GetCurrentProcess(), caller)) return error;
    if (const DWORD error = CaptureIdentity(target, target_identity)) return error;
    if (const DWORD error = CaptureIdentity(helper, helper_identity)) return error;
    if (!SameUserSession(caller, target_identity) || !SameUserSession(caller, helper_identity)) return ERROR_ACCESS_DENIED;
    // A handle remains bound to one process instance even if its PID is reused.
    // Recheck both handles after token sampling; the lease owner keeps the helper
    // handle and checks liveness again at each routing decision.
    if (const DWORD error = LiveHandle(target)) return error;
    if (const DWORD error = LiveHandle(helper)) return error;
    BindingV1 value{};
    value.magic = kMagic; value.version = kVersion; value.size = sizeof(value);
    value.target = target_identity; value.generation = generation;
    value.helper_pid = helper_identity.pid; value.helper_created = helper_identity.created; value.nonce = nonce;
    if (const DWORD error = ValidateFormat(value)) return error;
    output = value;
    return ERROR_SUCCESS;
}
DWORD CaptureCurrentWithHelper(HANDLE helper, uint64_t generation, const GUID& nonce, BindingV1& output) noexcept {
    return CaptureProcessWithHelper(GetCurrentProcess(), helper, generation, nonce, output);
}
}
