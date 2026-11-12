#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace pulse::dialog_launcher::admin_auto {
inline constexpr uint32_t kStatusMagic = 0x50414131;
struct HelperStatusV1 {
    uint32_t magic = kStatusMagic, version = 1, size = sizeof(HelperStatusV1), reserved = 0;
    DWORD pid = 0;
    FILETIME created{};
    DWORD session_id = 0, machine = 0;
    uint64_t generation = 0;
    DWORD ready = 0, error = ERROR_IO_PENDING;
};
static_assert(sizeof(HelperStatusV1) == 56);
struct Event {
    const char* stage;
    DWORD pid = 0, error = 0, entries = 0;
    FILETIME created{};
    uint64_t app_instance_id = 0;
    std::wstring app_basename;
};
struct Options {
    std::wstring installed_root;
    bool companion = false;
    bool probe_only = false;
    DWORD duration_ms = 0;
    HANDLE stop = nullptr; // Borrowed; optional manual-reset event.
    std::function<void(const Event&)> log;
};
// Pure policy model; identity comparisons come from real handles in Run.
struct Eligibility {
    bool same_user_session = false, elevated = false, unrestricted = false;
    bool same_architecture = false, unprotected_process = false, excluded = true;
};
DWORD CheckEligibility(const Eligibility&) noexcept;
std::wstring StatusBasename(DWORD session_id, WORD machine);
// Long-lived same-user/session elevated helper. No registration, token changes,
// process suspension or forced target termination. Injection is never retried for
// the same retained PID+creation identity. Completion requires bound ready data.
DWORD Run(const Options&) noexcept;
}
