#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace pulse::shell_bridge::runtime {
inline constexpr uint32_t kConfigMagic = 0x50445243;
inline constexpr uint32_t kReadyMagic = 0x50445252;
inline constexpr uint32_t kVersion = 1;
inline constexpr size_t kPathUnits = 32768;
inline constexpr uint32_t kCoCreateInstance = 1;
inline constexpr uint32_t kCoCreateInstanceEx = 2;
inline constexpr uint32_t kCoGetClassObject = 4;
inline constexpr uint32_t kCreateProcessW = 8;
// Other standard entries are currently unconnected. Flags are coverage,
// never proof that any individual dialog passed the capability policy.
#pragma pack(push, 8)
// Session::reserved is a flags word; unknown bits always fail closed.
inline constexpr uint32_t kAdminSession = 1;
struct Session {
    uint32_t magic = kConfigMagic, version = kVersion, size = sizeof(Session), reserved = 0;
    uint32_t target_pid = 0, machine = 0;
    FILETIME target_created{};
    wchar_t nonce[40]{};
    wchar_t host_path[kPathUnits]{};
    wchar_t target_path[kPathUnits]{};
    wchar_t control_path[kPathUnits]{};
    wchar_t launcher_path[kPathUnits]{};
};
struct Ready {
    uint32_t magic = kReadyMagic, version = kVersion, size = sizeof(Ready), reserved = 0;
    uint32_t target_pid = 0, machine = 0;
    FILETIME target_created{};
    wchar_t nonce[40]{};
    HRESULT status = E_FAIL;
    uint32_t installed_entries = 0;
};
#pragma pack(pop)
inline constexpr wchar_t kSessionFile[] = L"session.bin";
inline constexpr wchar_t kReadyFile[] = L"runtime-ready.bin";
inline constexpr wchar_t kDisableFile[] = L"disable";
// Launcher publishes session.bin before resuming its CREATE_SUSPENDED child.
// Runtime validates all identity fields before publishing ready. Ready only
// acknowledges runtime initialization/entry coverage, not a displayed dialog.
// The directory is unique GUID-named with a current-user/System-only DACL.
// Disable is CREATE_NEW and retained with diagnostics until explicit cleanup;
// neither component deletes the directory or kills the user application.
}
