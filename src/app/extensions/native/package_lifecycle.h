#pragma once
#include <windows.h>
namespace pulse { struct AppState; }
namespace pulse::extensions::native {
bool StartPackageMonitoring(HWND window);
bool HandlePackageMonitoringTimer(HWND window, WPARAM timer, AppState& state);
void StopPackageMonitoring(HWND window);
void ReleasePackageOwner(HWND owner);
}
