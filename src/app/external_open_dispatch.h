#pragma once
namespace pulse {
struct AppState;
void DispatchQueuedExternalOpen(AppState& state);
void TickExternalOpen(AppState& state);
}
