#pragma once
#include "single_instance_coordinator.h"
namespace pulse {
struct AppState;
// Invalid requested fixtures fail closed before normal singleton discovery.
bool PrepareHotEntryProbe(AppState& state);
std::wstring_view HotEntryProbeMutexName();
bool HotEntryProbeAcquired(app::SingleInstanceCoordinator::AcquireResult result);
void HotEntryProbeForwarded(bool success, const std::array<unsigned char, 16>& attempted_id);
void InitializeHotEntryProbe(AppState& state);
void HotEntryProbePresented(AppState& state);
void TickHotEntryProbe(AppState& state);
}
