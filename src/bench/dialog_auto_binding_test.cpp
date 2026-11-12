#include "../common/dialog_auto_binding.h"
#include <cstdio>
#include <cstring>
#include <string_view>

namespace binding = pulse::dialog_auto_binding;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* label) { ++checks; if (!value) ++failures; std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); }
binding::BindingV1 Fixture() {
    binding::BindingV1 result{};
    result.magic = binding::kMagic; result.version = binding::kVersion; result.size = sizeof(result);
    result.target.pid = 100; result.target.created = {200, 300}; result.target.session_id = 2;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    InitializeSid(result.target.user_sid, &authority, 5);
    const DWORD parts[]{21, 11, 22, 33, 1001};
    for (DWORD i = 0; i < 5; ++i) *GetSidSubAuthority(result.target.user_sid, i) = parts[i];
    result.target.sid_size = GetLengthSid(result.target.user_sid);
    result.generation = 8; result.helper_pid = 400; result.helper_created = {500, 600};
    result.nonce = {0x12345678, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
    return result;
}
bool Zero(const binding::BindingV1& value) { const binding::BindingV1 zero{}; return std::memcmp(&value, &zero, sizeof(value)) == 0; }
}
int main() {
    const auto original = Fixture();
    Check(binding::ValidateFormat(original) == ERROR_SUCCESS, "fixed POD format accepts valid full SID binding");
    Check(binding::ValidateLive(original, original, true, true, 8) == ERROR_SUCCESS, "same live identity and generation admitted");
    auto observed = original; ++observed.target.created.dwLowDateTime;
    Check(binding::ValidateLive(original, observed, true, true, 8) == ERROR_INVALID_DATA, "target PID reuse rejected by creation time");
    observed = original; ++observed.target.pid;
    Check(binding::MatchTarget(original, observed.target) == ERROR_INVALID_DATA, "different target PID rejected");
    observed = original; ++observed.target.session_id;
    Check(binding::ValidateLive(original, observed, true, true, 8) == ERROR_ACCESS_DENIED, "same user in different session rejected");
    observed = original; ++*GetSidSubAuthority(observed.target.user_sid, 4);
    Check(binding::ValidateLive(original, observed, true, true, 8) == ERROR_ACCESS_DENIED, "complete SID compared through last subauthority");
    observed = original; ++observed.helper_created.dwHighDateTime;
    Check(binding::ValidateLive(original, observed, true, true, 8) == ERROR_INVALID_DATA, "helper PID reuse rejected by creation time");
    observed = original; ++observed.helper_pid;
    Check(binding::ValidateLive(original, observed, true, true, 8) == ERROR_INVALID_DATA, "helper PID mismatch rejected");
    Check(binding::ValidateLive(original, original, false, true, 8) == ERROR_PROCESS_ABORTED, "dead helper fails closed");
    Check(binding::ValidateLive(original, original, true, false, 8) == ERROR_ACCESS_DISABLED_BY_POLICY, "disabled switch fails closed");
    Check(binding::ValidateLive(original, original, true, true, 9) == ERROR_REVISION_MISMATCH, "stale generation rejected");
    Check(binding::ValidateLive(original, original, true, true, 0) == ERROR_REVISION_MISMATCH, "unknown current generation rejected");
    observed = original; ++observed.generation;
    Check(binding::ValidateLive(original, observed, true, true, 8) == ERROR_REVISION_MISMATCH, "observed generation mismatch rejected");
    observed = original; ++observed.nonce.Data4[7];
    Check(binding::ValidateLive(original, observed, true, true, 8) == ERROR_INVALID_DATA, "nonce mismatch rejected");
    auto bad = original; ++bad.version;
    Check(binding::ValidateFormat(bad) == ERROR_INVALID_DATA, "unknown format version rejected");
    bad = original; --bad.size;
    Check(binding::ValidateFormat(bad) == ERROR_INVALID_DATA, "wrong wire size rejected");
    bad = original; bad.reserved_tail = 1;
    Check(binding::ValidateFormat(bad) == ERROR_INVALID_DATA, "reserved bytes rejected");
    bad = original; bad.nonce = {};
    Check(binding::ValidateFormat(bad) == ERROR_INVALID_DATA, "empty GUID rejected");
    bad = original; bad.target.user_sid[1] = 255;
    Check(binding::ValidateFormat(bad) == ERROR_INVALID_DATA, "oversized SID rejected before OS SID parsing");
    bad = original; --bad.target.sid_size;
    Check(binding::ValidateFormat(bad) == ERROR_INVALID_DATA, "truncated SID length rejected");
    bad = original; bad.target.user_sid[bad.target.sid_size] = 1;
    Check(binding::ValidateFormat(bad) == ERROR_INVALID_DATA, "SID tail bytes must be zero");
    bad = original; bad.target.created = {};
    Check(binding::ValidateFormat(bad) == ERROR_INVALID_DATA, "unknown creation time rejected");
    std::wstring locator;
    Check(binding::LocatorBasename(original.target, locator) == ERROR_SUCCESS && locator == L"2-100-0000012C000000C8", "locator is deterministic numeric identity only");
    Check(locator.find_first_not_of(L"0123456789ABCDEF-") == std::wstring::npos, "locator contains no path syntax");
    auto reused = original.target; ++reused.created.dwLowDateTime; std::wstring other;
    Check(binding::LocatorBasename(reused, other) == ERROR_SUCCESS && other != locator, "PID reuse receives distinct locator basename");
    reused.pid = 0;
    Check(binding::LocatorBasename(reused, other) == ERROR_INVALID_DATA && other.empty(), "invalid locator clears output");
    binding::BindingV1 captured = original;
    Check(binding::CaptureCurrentWithHelper(nullptr, 8, original.nonce, captured) == ERROR_INVALID_HANDLE && Zero(captured), "invalid helper handle reports Win32 error and clears output");
    captured = original;
    Check(binding::CaptureProcessWithHelper(nullptr, GetCurrentProcess(), 8, original.nonce, captured) == ERROR_INVALID_HANDLE && Zero(captured), "invalid publisher target handle rejected");
    captured = original;
    Check(binding::CaptureCurrentWithHelper(GetCurrentProcess(), 0, original.nonce, captured) == ERROR_INVALID_PARAMETER && Zero(captured), "invalid capture generation rejected");
    const DWORD capture = binding::CaptureCurrentWithHelper(GetCurrentProcess(), 8, original.nonce, captured);
    HANDLE token = nullptr; TOKEN_ELEVATION elevation{}; DWORD returned = 0; BOOL restricted = TRUE;
    const bool token_opened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE;
    bool token_queried = false;
    if (token_opened) {
        token_queried = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned) != FALSE;
        restricted = IsTokenRestricted(token); CloseHandle(token);
    }
    Check(token_opened && token_queried, "current token evidence collected");
    if (restricted || !elevation.TokenIsElevated) {
        const DWORD expected = restricted ? ERROR_ACCESS_DENIED : ERROR_ELEVATION_REQUIRED;
        Check(capture == expected && Zero(captured), "actual ordinary or restricted token rejected without UAC");
    } else {
        Check(capture == ERROR_SUCCESS && binding::ValidateFormat(captured) == ERROR_SUCCESS && captured.target.pid == GetCurrentProcessId(), "already elevated current token captures real identity without starting UAC");
    }
    std::printf("capture_win32=%lu elevated=%lu restricted=%d\n", capture, elevation.TokenIsElevated, restricted);
    std::printf("Checks: %u, failures: %u\n", checks, failures);
    return failures ? 1 : 0;
}
