#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <type_traits>

namespace pulse::dialog_auto_binding {
inline constexpr uint32_t kMagic = 0x50414231;
inline constexpr uint32_t kVersion = 1;
struct ProcessIdentityV1 {
    DWORD pid;
    FILETIME created;
    DWORD session_id;
    DWORD sid_size;
    BYTE user_sid[SECURITY_MAX_SID_SIZE];
};
// Fixed wire POD, no paths, pointers or implicit padding. SID tail/reserved bytes
// must be zero. This record is identity evidence, not an authorization source.
struct BindingV1 {
    uint32_t magic, version, size, reserved;
    ProcessIdentityV1 target;
    uint64_t generation;
    DWORD helper_pid;
    FILETIME helper_created;
    GUID nonce;
    DWORD reserved_tail;
};
static_assert(std::is_trivially_copyable_v<BindingV1> && std::is_standard_layout_v<BindingV1>);
static_assert(sizeof(ProcessIdentityV1) == 88 && sizeof(BindingV1) == 144);
DWORD ValidateFormat(const BindingV1&) noexcept;
DWORD MatchTarget(const BindingV1&, const ProcessIdentityV1& current) noexcept;
// observed must come from current handle/token queries, never from the message
// or stored record. helper_alive/enabled/generation are live caller observations.
DWORD ValidateLive(const BindingV1& bound, const BindingV1& observed,
    bool helper_alive, bool enabled, uint64_t current_generation) noexcept;
DWORD LocatorBasename(const ProcessIdentityV1&, std::wstring& output) noexcept;
// Caller supplies a real helper handle with QUERY_LIMITED_INFORMATION|SYNCHRONIZE.
// Both process tokens must be elevated and unrestricted, same full SID/session.
// Thread impersonation, AppContainer, dead helper and every query error reject.
// No process is opened by PID; output is all-zero on failure.
DWORD CaptureCurrentWithHelper(HANDLE helper, uint64_t generation, const GUID& nonce, BindingV1& output) noexcept;
// Publisher form: caller, target and helper must share elevated SID/session.
DWORD CaptureProcessWithHelper(HANDLE target, HANDLE helper, uint64_t generation,
    const GUID& nonce, BindingV1& output) noexcept;
}
