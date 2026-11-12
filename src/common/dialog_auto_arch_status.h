#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace pulse::dialog_auto {
enum class ArchComponentState { Unknown, Missing, Present };
enum class ArchRegistrationState { Unregistered, Registered, DisabledMatching, Conflict, ReadError };
struct ArchStatus {
    bool x86 = false;
    ArchComponentState component = ArchComponentState::Unknown;
    ArchRegistrationState registration = ArchRegistrationState::Unregistered;
    DWORD registry_error = 0, component_error = 0;
    std::wstring dll_path;
    bool operator==(const ArchStatus&) const = default;
};
// Read-only. Run on the management worker; no activation, helper or mutation.
// An explicit root lets tests use an isolated hive without remapping HKCU.
ArchStatus InspectArchStatus(HKEY root, bool x86, const std::wstring& component_directory = {}) noexcept;
std::wstring FormatArchStatus(std::wstring_view format, std::wstring_view architecture,
    std::wstring_view component, std::wstring_view registration);
}
