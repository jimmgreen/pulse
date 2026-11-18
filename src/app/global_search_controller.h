#pragma once
namespace pulse {
struct AppState;
struct GlobalSearchEvents;
GlobalSearchEvents MakeGlobalSearchEvents(AppState& s);
void ApplyGlobalSearchSettings(AppState& s);
// double_ctrl: summoned by two Ctrl taps rather than the registered hotkey.
void ToggleGlobalSearch(AppState& s, bool double_ctrl = false);
// Settings > Search master switch: asks before turning it off (and whether to
// delete the index), saves the preference and applies it.
void ToggleSearchSwitch(AppState& s);
void ApplySearchSwitch(AppState& s, bool purge_index);
void ShutdownGlobalSearch(AppState& s);
// Answers the Open / Save dialog jump bar: the folders open in Pulse and around.
void SendDialogJumpContext(AppState& s);
void PublishPickerPlaces(AppState& s, bool force = false);
bool HandleGlobalSearchHotkeyCapture(AppState& s, unsigned key);
}
