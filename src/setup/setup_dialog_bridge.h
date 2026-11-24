#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::setup {

struct BridgeStageRequest {
    std::wstring app_dir;              // User's chosen Pulse install dir (may be on D:\ / E:\ / F:\).
    std::wstring stage_source_dir;     // Directory containing built Stub/Core/Broker/Inject32/Overlay files.
    std::wstring host_exe;             // Pulse host executable path (e.g. D:\Pulse\pulse.exe).
    std::wstring bridge_root_override; // Empty => C:\Program Files\Common Files\Pulse\DialogBridge.
    bool harden_dacl = true;
    bool configure_service = true;
    bool start_service = true;
    bool register_hklm_com = false;
};

struct BridgeStageResult {
    DWORD error = ERROR_SUCCESS;
    std::wstring bridge_root;
    std::wstring core_dir_x64;
    std::wstring core_dir_x86;
    uint64_t generation = 0;
    bool stub64_staged = false;
    bool stub32_staged = false;
    bool core64_staged = false;
    bool core32_staged = false;
    bool broker_staged = false;
    bool inject32_staged = false;
    bool overlay_staged = false;
    bool service_configured = false;
    bool service_running = false;
};

// Provisions the protected shared bridge root (C:\Program Files\Common Files\Pulse\DialogBridge),
// stages fixed-path Stub/Broker/Inject32/Overlay binaries (lock-safe rename if running),
// stages hash-addressed Core64/Core32 DLLs under v\<sha256>\, atomically updates active-bridge.json,
// and optionally installs/updates/starts the PulseDialogBroker SYSTEM service.
BridgeStageResult ProvisionDialogBridge(const BridgeStageRequest& request) noexcept;

// Installs or updates and optionally starts the PulseDialogBroker SYSTEM service pointing to `broker_exe`.
DWORD InstallOrUpdateDialogBrokerService(const std::wstring& broker_exe, bool start_now,
                                         bool& running_out) noexcept;

} // namespace pulse::setup
