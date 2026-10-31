#include "../common/process_token_restriction.h"
#include <cstdio>
int main() {
    HANDLE token = nullptr, restricted = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token)) return 2;
    int failures = 0;
    auto check = [&](bool pass, const char* label) { std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label); if (!pass) ++failures; };
    check(pulse::security::CheckUnrestrictedToken(token) == ERROR_SUCCESS, "ordinary token allowed");
    check(pulse::security::CheckUnrestrictedToken(INVALID_HANDLE_VALUE) != ERROR_SUCCESS, "invalid token rejected");
    BYTE sid[SECURITY_MAX_SID_SIZE]{}; DWORD bytes = sizeof(sid);
    if (!CreateWellKnownSid(WinWorldSid, nullptr, sid, &bytes)) { CloseHandle(token); return 2; }
    SID_AND_ATTRIBUTES restriction{sid, 0};
    const BOOL created = CreateRestrictedToken(token, 0, 0, nullptr, 0, nullptr, 1, &restriction, &restricted);
    check(created != FALSE, "create restricting SID token");
    if (created) { check(pulse::security::CheckUnrestrictedToken(restricted) == ERROR_ACCESS_DENIED, "restricting SID token rejected"); CloseHandle(restricted); }
    CloseHandle(token); return failures ? 1 : 0;
}
