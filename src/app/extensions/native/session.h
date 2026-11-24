#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>
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
bool BackgroundPackageEnabled();
void StartBackgroundPackages(AppState& owner);
void PrepareCommandPackages(AppState& owner);
bool PackageSessionLoaded(HWND owner, size_t package);
// Activates the package (OnStart) for a contributed command, a settings page or a
// menu scan. Gives up per package build after a failed start.
bool EnsureCommandPackageStarted(AppState& owner, size_t package);
// Packages whose manifest StartsWithWindow() (status items, columns, hooks) are
// activated shortly after each main window is created: ScheduleStartupPackages in
// WM_CREATE, HandleStartupPackagesTimer in WM_TIMER.
void ScheduleStartupPackages(HWND owner);
bool HandleStartupPackagesTimer(HWND window, WPARAM timer, AppState& owner);
void ActivateStartupPackages(AppState& owner);
// Preview providers register in the SDK's OnStart, so previews need the package
// activated, not just loaded. Gives up per package build after a failed start.
bool EnsurePreviewPackageActive(AppState& owner, size_t package);
// Every enabled package that may preview `extension` (".ext", lower case ASCII).
void ActivatePreviewPackages(AppState& owner, std::string_view extension);
// Grid thumbnails. Any thread (the decode lanes): true when an enabled package that
// may serve `extension` has not activated yet; the first window is then asked
// (WM_PLUGIN_THUMBNAILS) to activate it and the caller should retry shortly.
bool RequestThumbnailPackages(std::string_view extension);
// UI thread: on WM_PLUGIN_THUMBNAILS.
void ActivateThumbnailPackages(AppState& owner);
// UI thread: recompute what RequestThumbnailPackages sees after packages changed.
void RefreshThumbnailPackages(HWND owner);
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