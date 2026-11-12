#pragma once
#include "../common/runtime_log.h"
#include <windows.h>

namespace pulse::shell_bridge::token_diagnostics {
enum class Stage { Initialize, Activation, Resolve, Validation };
struct Evidence {
    unsigned reason = 0;
    DWORD error = 0;
    bool elevated_known = false, elevated = false;
    DWORD elevation_type = 0, token_type = 0;
    DWORD elevation_type_error = 0, token_type_error = 0;
    bool trust_checked = false;
    DWORD trust_error = 0;
};
inline const wchar_t* ReasonName(unsigned reason) noexcept {
    constexpr const wchar_t* names[]{L"TOKEN_ACCEPTED", L"THREAD_IMPERSONATION", L"THREAD_TOKEN_QUERY_FAILED",
        L"PROCESS_TOKEN_QUERY_FAILED", L"ELEVATION_QUERY_FAILED", L"ELEVATED_BUNDLE_UNTRUSTED", L"RESTRICTED_TOKEN",
        L"APPCONTAINER_QUERY_FAILED", L"APPCONTAINER", L"INTEGRITY_QUERY_FAILED", L"INVALID_INTEGRITY_SID", L"BELOW_MEDIUM_INTEGRITY"};
    return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : L"UNKNOWN_TOKEN_REASON";
}
inline const wchar_t* StageName(Stage stage) noexcept {
    switch (stage) {
    case Stage::Initialize: return L"initialize";
    case Stage::Activation: return L"activation";
    case Stage::Resolve: return L"resolve";
    case Stage::Validation: return L"validation";
    }
    return L"unknown";
}
inline const wchar_t* ElevationTypeName(DWORD type) noexcept {
    return type == TokenElevationTypeDefault ? L"default" : type == TokenElevationTypeFull ? L"full" : type == TokenElevationTypeLimited ? L"limited" : L"unknown";
}
inline const wchar_t* TokenTypeName(DWORD type) noexcept {
    return type == TokenPrimary ? L"primary" : type == TokenImpersonation ? L"impersonation" : L"unknown";
}
// Classification only. None of these observations authorizes a gate.
inline const wchar_t* GateReason(bool token_safe, const Evidence& token, bool enabled, bool conflict, DWORD error, bool path_match) noexcept {
    if (!token_safe) return ReasonName(token.reason);
    if (error) return L"REGISTRATION_QUERY_FAILED";
    if (conflict) return L"REGISTRATION_CONFLICT";
    if (!enabled) return L"AUTOMATIC_DISABLED";
    if (!path_match) return L"REGISTERED_MODULE_MISMATCH";
    return L"ELIGIBLE";
}
inline void LogGate(const char* event, Stage stage, const wchar_t* reason,
    std::initializer_list<diagnostics::runtime::Field> fields) noexcept {
    const DWORD saved = GetLastError();
    diagnostics::runtime::TextEvent(event, {{"reason_name", reason}, {"stage", StageName(stage)}}, fields);
    SetLastError(saved);
}
inline void LogToken(const char* event, Stage stage, const Evidence& value,
    diagnostics::runtime::Level level = diagnostics::runtime::Level::Info, const wchar_t* reason_override = nullptr) noexcept {
    const DWORD saved = GetLastError();
    // runtime_log permits twelve numeric and four text fields per record.
    diagnostics::runtime::TextEvent(event,
        {{"reason_name", reason_override ? reason_override : ReasonName(value.reason)}, {"stage", StageName(stage)},
         {"elevation_type_name", ElevationTypeName(value.elevation_type)}, {"token_type_name", TokenTypeName(value.token_type)}},
        {{"reason", value.reason}, {"error", value.error}, {"process_arch_bits", sizeof(void*) * 8},
         {"process_elevated_known", value.elevated_known}, {"process_elevated", value.elevated},
         {"process_elevation_type", value.elevation_type}, {"process_token_type", value.token_type},
         {"elevation_type_error", value.elevation_type_error}, {"token_type_error", value.token_type_error},
         {"admin_trust_checked", value.trust_checked}, {"admin_trust_error", value.trust_error},
         {"admin_trust_accepted", value.trust_checked && value.trust_error == ERROR_SUCCESS}}, level);
    SetLastError(saved);
}
}
