#pragma once
#include <windows.h>
namespace pulse::security {
// IsTokenRestricted checks restricting SIDs, not whether UAC ever filtered a token.
// A query error never grants eligibility.
inline DWORD CheckUnrestrictedToken(HANDLE token) noexcept {
    SetLastError(ERROR_SUCCESS);
    const BOOL restricted = IsTokenRestricted(token);
    const DWORD error = GetLastError();
    if (restricted) return ERROR_ACCESS_DENIED;
    return error;
}
}
