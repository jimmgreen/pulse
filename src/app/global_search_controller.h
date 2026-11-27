#pragma once
#include <string>
#include <windows.h>
namespace pulse {
struct AppState;
struct GlobalSearchEvents;
GlobalSearchEvents MakeGlobalSearchEvents(AppState& s);
void ApplyGlobalSearchSettings(AppState& s);
// double_ctrl: summoned by two Ctrl taps rather than the registered hotkey.
void ToggleGlobalSearch(AppState& s, bool double_ctrl = false, WPARAM trigger_token = 0);
// Opens quick search with text typed elsewhere (the desktop pack). Independent of the summon hotkey;
// false when search is turned off or the window cannot open.
bool ShowGlobalSearchWithQuery(AppState& s, const std::wstring& query);
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
