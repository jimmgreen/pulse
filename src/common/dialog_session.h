#pragma once
#include "../shell_bridge/runtime_config.h"
#include <filesystem>
#include <string>
#include <vector>
namespace pulse::dialog_session {
namespace cfg = shell_bridge::runtime;
struct Status {
    DWORD pid = 0;
    bool process_alive = false, historical_handshake = false, local_disabled = false;
    HRESULT initialization = E_PENDING;
    uint32_t entry_coverage = 0;
    std::wstring application;
};
std::wstring Canonical(const std::filesystem::path&, bool directory);
bool AllowedProcess(HANDLE process, bool allow_elevated = false);
bool TokenData(HANDLE, TOKEN_INFORMATION_CLASS, std::vector<BYTE>&);
bool Machine(const std::filesystem::path&, WORD&);
bool ProcessIdentity(HANDLE, const cfg::Session&);
bool Ready(const std::filesystem::path&, const cfg::Session&, cfg::Ready&);
// expected_launcher must come from a verified bundle/profile, never session.bin.
bool LoadSession(const std::filesystem::path&, const std::wstring& expected_launcher, cfg::Session&);
// Worker only. Exact PID/create-time/image/permission validation before mutation.
// Never kills processes. Handshake is historical; current routing is not queried.
DWORD Inspect(const std::wstring& directory, const std::wstring& expected_launcher,
              bool disable, Status&) noexcept;
}
