#pragma once
namespace pulse {
struct AppState;
void TickUpdates(AppState& state, unsigned long long now);
void CheckForUpdates(AppState& state);
void InstallUpdate(AppState& state);
void CompleteUpdateCheck(AppState& state);
void CompleteUpdateDownload(AppState& state);
}
