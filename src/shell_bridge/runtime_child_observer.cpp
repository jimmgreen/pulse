#include "../common/process_token_restriction.h"
#include "runtime_child_observer.h"
#if defined(PULSE_DIALOG_UI_PROOF)
#include "runtime_child_process.h"
#include <type_traits>
#include <vector>
#include <memory>
#include <utility>
namespace pulse::shell_bridge::runtime {
decltype(&::CreateProcessA) original_observed_create_a = &::CreateProcessA;
decltype(&::CreateProcessAsUserW) original_observed_as_user = &::CreateProcessAsUserW;
decltype(&::CreateProcessWithTokenW) original_observed_with_token = &::CreateProcessWithTokenW;
decltype(&::CreateProcessWithLogonW) original_observed_with_logon = &::CreateProcessWithLogonW;
namespace {
struct TokenHandle {
    HANDLE value = nullptr;
    ~TokenHandle() { if (value) CloseHandle(value); }
};
DWORD TokenError() { const DWORD e = GetLastError(); return e ? e : ERROR_FUNCTION_FAILED; }
ChildEligibilityDiagnostic TokenFailure(AsUserTokenReason reason, DWORD error = ERROR_ACCESS_DENIED) {
    return {error, static_cast<DWORD>(reason)};
}
template<class T> bool TokenValue(HANDLE token, TOKEN_INFORMATION_CLASS kind, T& value) {
    DWORD bytes = 0;
    return GetTokenInformation(token, kind, &value, sizeof(value), &bytes) && bytes == sizeof(value);
}
std::vector<BYTE> TokenBuffer(HANDLE token, TOKEN_INFORMATION_CLASS kind, DWORD& error) {
    DWORD bytes = 0; GetTokenInformation(token, kind, nullptr, 0, &bytes);
    if (!bytes || bytes > 65536) { error = TokenError(); return {}; }
    std::vector<BYTE> result(bytes);
    if (!GetTokenInformation(token, kind, result.data(), bytes, &bytes)) { error = TokenError(); return {}; }
    return result;
}
bool SidEqual(PSID a, PSID b) { return a && b && IsValidSid(a) && IsValidSid(b) && EqualSid(a, b); }
bool LuidEqual(LUID a, LUID b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }
ChildEligibilityDiagnostic EquivalentToken(HANDLE candidate) noexcept {
    try {
        TokenHandle impersonation;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &impersonation.value))
            return TokenFailure(AsUserTokenReason::Impersonation);
        const DWORD impersonation_error = GetLastError();
        if (impersonation_error != ERROR_NO_TOKEN) return TokenFailure(AsUserTokenReason::Impersonation, impersonation_error);
        TokenHandle own;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &own.value)) return TokenFailure(AsUserTokenReason::Query, TokenError());
        // NULL remains NULL in the native API call; this handle is for comparison
        // only, representing the optional caller-context token contract.
        const HANDLE source = candidate ? candidate : own.value;
        TOKEN_STATISTICS own_before{}, source_before{};
        TOKEN_TYPE type{}; DWORD own_session = 0, source_session = 0;
        if (!TokenValue(source, TokenType, type) || !TokenValue(own.value, TokenStatistics, own_before) ||
            !TokenValue(source, TokenStatistics, source_before) || !TokenValue(own.value, TokenSessionId, own_session) ||
            !TokenValue(source, TokenSessionId, source_session)) return TokenFailure(AsUserTokenReason::Query, TokenError());
        if (type != TokenPrimary) return TokenFailure(AsUserTokenReason::NotPrimary);
        if (source_session != own_session) return TokenFailure(AsUserTokenReason::Session);
        if (!LuidEqual(source_before.AuthenticationId, own_before.AuthenticationId)) return TokenFailure(AsUserTokenReason::Authentication);
        const auto compare_flag = [&](TOKEN_INFORMATION_CLASS kind, AsUserTokenReason reason) {
            DWORD a = 0, b = 0;
            if (!TokenValue(own.value, kind, a) || !TokenValue(source, kind, b)) return TokenFailure(AsUserTokenReason::Query, TokenError());
            return a || b ? TokenFailure(reason) : ChildEligibilityDiagnostic{};
        };
        for (const auto [kind, reason] : {std::pair{TokenElevation, AsUserTokenReason::Elevated},
            std::pair{TokenIsAppContainer, AsUserTokenReason::AppContainer},
            std::pair{TokenUIAccess, AsUserTokenReason::UiAccess}}) {
            const auto result = compare_flag(kind, reason); if (result.error) return result;
        }
        for (HANDLE checked_token : {own.value, source}) {
            const DWORD restriction = security::CheckUnrestrictedToken(checked_token);
            if (restriction) return TokenFailure(restriction == ERROR_ACCESS_DENIED ? AsUserTokenReason::Restricted : AsUserTokenReason::Query, restriction);
        }
        DWORD error = ERROR_SUCCESS;
        for (const auto [kind, reason] : {std::pair{TokenUser, AsUserTokenReason::User},
            std::pair{TokenIntegrityLevel, AsUserTokenReason::Integrity}, std::pair{TokenOwner, AsUserTokenReason::Owner},
            std::pair{TokenPrimaryGroup, AsUserTokenReason::PrimaryGroup}}) {
            auto a = TokenBuffer(own.value, kind, error), b = TokenBuffer(source, kind, error);
            if (error) return TokenFailure(AsUserTokenReason::Query, error);
            // All four documented records begin with the SID pointer.
            const PSID sa = *reinterpret_cast<PSID const*>(a.data()), sb = *reinterpret_cast<PSID const*>(b.data());
            if (!SidEqual(sa, sb)) return TokenFailure(reason);
            if (kind == TokenIntegrityLevel && (!*GetSidSubAuthorityCount(sb) ||
                *GetSidSubAuthority(sb, *GetSidSubAuthorityCount(sb) - 1) < SECURITY_MANDATORY_MEDIUM_RID)) return TokenFailure(reason);
        }
        auto ga = TokenBuffer(own.value, TokenGroups, error), gb = TokenBuffer(source, TokenGroups, error);
        if (error) return TokenFailure(AsUserTokenReason::Query, error);
        const auto* a = reinterpret_cast<const TOKEN_GROUPS*>(ga.data()); const auto* b = reinterpret_cast<const TOKEN_GROUPS*>(gb.data());
        if (a->GroupCount != b->GroupCount || a->GroupCount > 1024) return TokenFailure(AsUserTokenReason::Groups);
        for (DWORD i = 0; i < a->GroupCount; ++i) {
            bool same = false;
            for (DWORD j = 0; j < b->GroupCount; ++j) if (a->Groups[i].Attributes == b->Groups[j].Attributes && SidEqual(a->Groups[i].Sid, b->Groups[j].Sid)) { same = true; break; }
            if (!same) return TokenFailure(AsUserTokenReason::Groups);
        }
        auto pa = TokenBuffer(own.value, TokenPrivileges, error), pb = TokenBuffer(source, TokenPrivileges, error);
        if (error) return TokenFailure(AsUserTokenReason::Query, error);
        const auto* ap = reinterpret_cast<const TOKEN_PRIVILEGES*>(pa.data()); const auto* bp = reinterpret_cast<const TOKEN_PRIVILEGES*>(pb.data());
        if (ap->PrivilegeCount != bp->PrivilegeCount || ap->PrivilegeCount > 1024) return TokenFailure(AsUserTokenReason::Privileges);
        for (DWORD i = 0; i < ap->PrivilegeCount; ++i) {
            bool same = false;
            for (DWORD j = 0; j < bp->PrivilegeCount; ++j) if (ap->Privileges[i].Attributes == bp->Privileges[j].Attributes && LuidEqual(ap->Privileges[i].Luid, bp->Privileges[j].Luid)) { same = true; break; }
            if (!same) return TokenFailure(AsUserTokenReason::Privileges);
        }
        TOKEN_MANDATORY_POLICY own_policy{}, source_policy{}; TOKEN_STATISTICS own_after{}, source_after{};
        if (!TokenValue(own.value, TokenMandatoryPolicy, own_policy) || !TokenValue(source, TokenMandatoryPolicy, source_policy) ||
            !TokenValue(own.value, TokenStatistics, own_after) || !TokenValue(source, TokenStatistics, source_after)) return TokenFailure(AsUserTokenReason::Query, TokenError());
        if (own_policy.Policy != source_policy.Policy) return TokenFailure(AsUserTokenReason::MandatoryPolicy);
        if (!LuidEqual(own_before.ModifiedId, own_after.ModifiedId) || !LuidEqual(source_before.ModifiedId, source_after.ModifiedId))
            return TokenFailure(AsUserTokenReason::ChangedDuringQuery, ERROR_RETRY);
        return {};
    } catch (const std::bad_alloc&) { return TokenFailure(AsUserTokenReason::Query, ERROR_NOT_ENOUGH_MEMORY); }
    catch (...) { return TokenFailure(AsUserTokenReason::Query, ERROR_INVALID_DATA); }
}
// Does not set W's recursion flag: if the native API calls W internally, the
// existing W routing rules continue to apply exactly as before this observer.
template<class Call>
BOOL Observe(ChildCallEntry entry, LPPROCESS_INFORMATION information, Call&& original) noexcept {
    const DWORD incoming = GetLastError();
    auto event = BeginChildCallDiagnostic(entry);
    event.branch = ChildCallBranch::ObserveOnly;
    SetLastError(incoming);
    const BOOL result = original();
    const DWORD error = GetLastError();
    event.result = result; event.native_error = error;
    if (result && information) event.child_pid = information->dwProcessId;
    CompleteChildCallDiagnostic(event);
    SetLastError(error);
    return result;
}
}
BOOL WINAPI ObserveCreateProcessA(LPCSTR app, LPSTR command, LPSECURITY_ATTRIBUTES process_attributes,
    LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit, DWORD flags, LPVOID environment,
    LPCSTR directory, LPSTARTUPINFOA startup, LPPROCESS_INFORMATION information) noexcept {
    return Observe(ChildCallEntry::CreateProcessA, information, [&] {
        return original_observed_create_a(app, command, process_attributes, thread_attributes,
            inherit, flags, environment, directory, startup, information);
    });
}
BOOL WINAPI ObserveCreateProcessAsUserW(HANDLE token, LPCWSTR app, LPWSTR command,
    LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) noexcept {
    struct Arguments {
        HANDLE token; LPCWSTR app; LPWSTR command; LPSECURITY_ATTRIBUTES process_attributes, thread_attributes;
        BOOL inherit; LPCWSTR directory; LPSTARTUPINFOW startup; LPPROCESS_INFORMATION information;
    } args{token, app, command, process_attributes, thread_attributes, inherit, directory, startup, information};
    const auto invoke = [](void* opaque, DWORD call_flags, LPVOID call_environment) noexcept -> BOOL {
        const auto& a = *static_cast<Arguments*>(opaque);
        return original_observed_as_user(a.token, a.app, a.command, a.process_attributes, a.thread_attributes,
            a.inherit, call_flags, call_environment, a.directory, a.startup, a.information);
    };
    const auto eligible = [](void* opaque, HANDLE child) noexcept -> ChildEligibilityDiagnostic {
        if (!child) return EquivalentToken(static_cast<Arguments*>(opaque)->token);
        TokenHandle actual;
        if (!OpenProcessToken(child, TOKEN_QUERY, &actual.value)) return TokenFailure(AsUserTokenReason::Query, TokenError());
        return EquivalentToken(actual.value);
    };
    return RouteChildCreation(ChildCallEntry::CreateProcessAsUserW, &args, invoke, eligible,
        flags, environment, startup, information);
}
BOOL WINAPI ObserveCreateProcessWithTokenW(HANDLE token, DWORD logon_flags, LPCWSTR app,
    LPWSTR command, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) noexcept {
    return Observe(ChildCallEntry::CreateProcessWithTokenW, information, [&] {
        return original_observed_with_token(token, logon_flags, app, command, flags,
            environment, directory, startup, information);
    });
}
BOOL WINAPI ObserveCreateProcessWithLogonW(LPCWSTR user, LPCWSTR domain, LPCWSTR password,
    DWORD logon_flags, LPCWSTR app, LPWSTR command, DWORD flags, LPVOID environment,
    LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) noexcept {
    return Observe(ChildCallEntry::CreateProcessWithLogonW, information, [&] {
        return original_observed_with_logon(user, domain, password, logon_flags, app, command,
            flags, environment, directory, startup, information);
    });
}
static_assert(std::is_convertible_v<decltype(&ObserveCreateProcessA), decltype(&::CreateProcessA)>);
static_assert(std::is_convertible_v<decltype(&ObserveCreateProcessAsUserW), decltype(&::CreateProcessAsUserW)>);
static_assert(std::is_convertible_v<decltype(&ObserveCreateProcessWithTokenW), decltype(&::CreateProcessWithTokenW)>);
static_assert(std::is_convertible_v<decltype(&ObserveCreateProcessWithLogonW), decltype(&::CreateProcessWithLogonW)>);
}
#endif
