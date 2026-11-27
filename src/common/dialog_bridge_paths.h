#pragma once

#include <windows.h>
#include <string>

namespace pulse::dialog_bridge_paths {

inline constexpr wchar_t kCommonRelativeSubdir[] = L"Pulse\\DialogBridge";
inline constexpr wchar_t kActivePointerFile[] = L"active-bridge.json";
inline constexpr wchar_t kStubDllName64[] = L"PulseDialogStub64.dll";
inline constexpr wchar_t kStubDllName32[] = L"PulseDialogStub32.dll";
inline constexpr wchar_t kCoreDllName64[] = L"PulseDialogCore64.dll";
inline constexpr wchar_t kCoreDllName32[] = L"PulseDialogCore32.dll";
inline constexpr wchar_t kBrokerExeName[] = L"PulseDialogBroker.exe";
inline constexpr wchar_t kInject32ExeName[] = L"PulseDialogInject32.exe";
inline constexpr wchar_t kOverlayExeName[] = L"PulseDialogOverlay.exe";
inline constexpr wchar_t kExplorerBridgeExeName[] = L"pulse_explorer_bridge.exe";

struct ActiveBridgePointer {
    uint32_t schema = 1;
    std::wstring core_dir_x64;
    std::wstring core_dir_x86;
    std::string core_digest_x64;
    std::string core_digest_x86;
    std::string explorer_bridge_sha256_x64;
    std::string explorer_bridge_sha256_x86;
    std::wstring host_exe;
    std::wstring install_root;
    uint64_t generation = 0;
    bool disabled = false;
};

// Returns C:\Program Files\Common Files\Pulse\DialogBridge (or override_root for tests).
std::wstring CommonDialogBridgeRoot(const std::wstring& override_root = {}) noexcept;

// Returns the SDDL used for the protected shared bridge tree:
// SYSTEM/Admins full control, Users & ALL APPLICATION PACKAGES (S-1-15-2-1) read+execute.
const wchar_t* ProtectedBridgeSddl(bool include_app_container = true) noexcept;

// Creates (if absent) and hardens a protected bridge directory with PROTECTED_DACL.
// Caller must be elevated unless operating inside a test-owned directory with write DAC rights.
DWORD EnsureProtectedBridgeDirectory(const std::wstring& directory, bool include_app_container = true) noexcept;

// Atomically persists or loads active-bridge.json under bridge_root.
DWORD WriteActiveBridgePointer(const std::wstring& bridge_root, const ActiveBridgePointer& pointer) noexcept;
DWORD ReadActiveBridgePointer(const std::wstring& bridge_root, ActiveBridgePointer& output) noexcept;

// Prefers a verified CommonDialogBridgeRoot() when present (decoupling non-C: installs
// from elevated trust checks); falls back to install_root for legacy/unmigrated layouts.
std::wstring ResolveActiveBridgeRoot(const std::wstring& install_root,
                                     const std::wstring& override_common_root = {}) noexcept;

// Returns true when `path` is under Program Files or Program Files\Common Files.
bool IsUnderProgramFiles(const std::wstring& path) noexcept;

} // namespace pulse::dialog_bridge_paths
