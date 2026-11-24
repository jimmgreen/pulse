#pragma once
#include <windows.h>
#include <string>
#include <string_view>
namespace pulse::app {
// UI-safe: starts one worker; all process/bundle work stays off the caller.
void ConfigureDirectRevealBootstrap(bool enabled) noexcept;
namespace direct_reveal_bootstrap_detail {
bool RevealTargetName(std::wstring_view name) noexcept;
bool AllowedImage(std::wstring_view path) noexcept;
bool EligibleIntegrity(DWORD current_il, DWORD target_il) noexcept;
bool GeneralRevealCandidateImage(std::wstring_view path, std::wstring_view windows_dir, std::wstring_view install_root) noexcept;
std::wstring InstallRootFromExecutable(std::wstring_view executable);
#ifdef PULSE_DIRECT_REVEAL_BOOTSTRAP_TEST
// Test-only entry, absent from the production binary; caller owns the fixture process.
DWORD AttachFixture(DWORD pid, const wchar_t* dll) noexcept;
#endif
}
}
