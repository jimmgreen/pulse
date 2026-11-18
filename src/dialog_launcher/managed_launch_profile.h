#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <array>
#include <span>
#include <string>
#include <vector>

namespace pulse::dialog_launcher {
struct ComponentIdentity {
    uint32_t volume = 0;
    uint64_t file = 0, size = 0, modified = 0;
    bool operator==(const ComponentIdentity&) const = default;
};
struct ManagedLaunchProfile {
    uint32_t format_version = 1;
    uint32_t machine = 0;
    std::wstring bundle_root;
    std::string manifest_digest;
    std::wstring component_root, host, runtime, launcher, target, cwd, control_root;
    std::array<ComponentIdentity, 3> components{}; // host, runtime, launcher
    std::vector<std::wstring> arguments;
};
inline constexpr size_t kProfileMaximumBytes = 1024 * 1024;
DWORD EncodeProfile(const ManagedLaunchProfile&, std::vector<unsigned char>&) noexcept;
DWORD DecodeProfile(std::span<const unsigned char>, ManagedLaunchProfile&) noexcept;
// Resolves local absolute non-reparse paths and captures the exact component
// v1 host/runtime/launcher set only. v2 additionally binds and verifies the complete
// experimental manifest (including UI); neither format authenticates a publisher.
// No process creation, DLL loading, policy changes or file replacement.
DWORD CaptureProfile(ManagedLaunchProfile&, const std::wstring& actual_launcher) noexcept;
// Rechecks the same version directory and component identities on every use.
// This is validation, not a signature/trust assertion or an image TOCTOU lock.
DWORD ValidateProfile(const ManagedLaunchProfile&, const std::wstring& actual_launcher) noexcept;
DWORD SaveProfile(const std::wstring& absolute_file, const ManagedLaunchProfile&) noexcept;
// Local marker only; does not certify ancestor sessions or current runtime policy.
DWORD ReadDisableMarker(const std::wstring& control_root, bool& disabled) noexcept;
DWORD LoadProfile(const std::wstring& absolute_file, ManagedLaunchProfile&) noexcept;
}
