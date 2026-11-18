#pragma once
namespace pulse {
struct AppState;
// Initialization belongs after preferences load and before startup services.
void InitializeEntryCommandProbe(AppState& state);
// Call only immediately after an actual, non-hidden compositor Present.
void EntryCommandProbePresented(AppState& state);
// UI timer provides bounded failure/exit when presentation never succeeds.
void TickEntryCommandProbe(AppState& state);
}
