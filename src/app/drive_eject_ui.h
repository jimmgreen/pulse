#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
namespace pulse {
struct AppState;
struct DriveEjectTask;
bool StartPreparedDriveEject(AppState& state, const std::wstring& root, uint8_t type);
void ApplyDriveEjectReady(AppState* state, LPARAM payload);
void InheritDriveEjectSuspension(AppState& state);
void CompletePreparedDriveEject(AppState& state, const std::wstring& root);
}
