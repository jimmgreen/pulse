#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include "../../../../sdk/native/include/pulse/abi.h"
#include "../../../common/json_value.h"
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
class UiAdapter;
class SettingsPagesAdapter;

enum class PackageRuntimeState : uint8_t {
    Inactive,
    Loaded,
    Active,
    Draining,
    Failed,
};

struct PackageRuntimeSnapshot {
    PackageRuntimeState state = PackageRuntimeState::Inactive;
    uint64_t draining_ms = 0;
    bool callbacks_active = false;
    bool restart_required = false;
    PulseStatus last_status = PULSE_OK;
};

SettingsPagesAdapter* PackageSettings(HWND owner, size_t package);
// Explicit trusted developer DLL; manifest supplies the host resource identity.
bool StartFromArguments(AppState& owner, int argc, wchar_t** argv);
bool StartPackage(AppState& owner, size_t package);
void PrepareCommandPackages(AppState& owner);
bool PackageSessionLoaded(HWND owner, size_t package);
bool EnsureCommandPackageStarted(AppState& owner, size_t package);
PackageRuntimeSnapshot QueryPackageRuntime(HWND owner, size_t package);
PackageRuntimeSnapshot QueryPackageRuntime(size_t package);
void StopPackage(HWND owner, size_t package);
void StopPackage(size_t package);
UiAdapter* PackageUi(HWND owner, size_t package);
bool PackageActive(HWND owner, size_t package);
PulseStatus PackageLastStatus(HWND owner, size_t package);
void StopOwner(HWND owner);
void StopAll();
UiAdapter* OwnerUi(HWND owner);
bool OwnerActive(HWND owner);
json::Value OwnerMetrics(HWND owner);
}
