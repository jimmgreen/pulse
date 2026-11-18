#pragma once
#include <windows.h>
// Explicit experimental runtime only. A/WithToken/WithLogon are transparent
// observers. AsUser shares W's routing transaction after strict token eligibility;
// token/security/credentials are never replaced. Original APIs are called once.
#if defined(PULSE_DIALOG_UI_PROOF)
namespace pulse::shell_bridge::runtime {
enum class AsUserTokenReason : DWORD {
    None, Impersonation, Query, NotPrimary, User, Session, Authentication, Integrity,
    Elevated, Restricted, AppContainer, UiAccess, Groups, Privileges, Owner,
    PrimaryGroup, MandatoryPolicy, ChangedDuringQuery
};
extern decltype(&::CreateProcessA) original_observed_create_a;
extern decltype(&::CreateProcessAsUserW) original_observed_as_user;
extern decltype(&::CreateProcessWithTokenW) original_observed_with_token;
extern decltype(&::CreateProcessWithLogonW) original_observed_with_logon;
BOOL WINAPI ObserveCreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION) noexcept;
BOOL WINAPI ObserveCreateProcessAsUserW(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) noexcept;
BOOL WINAPI ObserveCreateProcessWithTokenW(HANDLE, DWORD, LPCWSTR, LPWSTR, DWORD, LPVOID,
    LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) noexcept;
BOOL WINAPI ObserveCreateProcessWithLogonW(LPCWSTR, LPCWSTR, LPCWSTR, DWORD, LPCWSTR, LPWSTR,
    DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION) noexcept;
}
#endif
