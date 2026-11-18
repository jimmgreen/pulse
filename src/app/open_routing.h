// open_routing.h - Opens folders that come from outside Pulse (a second
// launch, Win+E, another program's "show in folder", Explorer takeover) where
// plan §6 says: multi-window M7a. The rules are external_open.h; this applies
// them to the windows of the process.
#pragma once
#include <windows.h>

#include <string>

namespace pulse {

struct AppState;

// `raw` as a launch passes it: a folder, a file (its folder opens with it
// selected) or This PC. `mru` is the window that received the request (the
// most recently used one). Dispatched means navigation was issued, not that
// its contents have been presented. Deferred/Failed never carry a target.
enum class ExternalOpenStatus { Dispatched, Deferred, Failed };
struct ExternalOpenResult { ExternalOpenStatus status = ExternalOpenStatus::Failed; AppState* target = nullptr; };
ExternalOpenResult OpenExternalFolder(AppState& mru, const std::wstring& raw, bool reveal = false);
bool ExternalOpenWindowBusy(const AppState& state);

// WM_CONVERT_EXTERNAL_OPEN, the toast after such an open was clicked (M7b,
// plan §6 rule 7): a folder opened in window `s` goes back as a tab (with
// every tab of `s`, which then closes), or its tab moves to a new window.
// After 3 conversions the same way, a toast offers that as the default. May
// destroy `s`: touch nothing of it afterwards.
void ConvertExternalOpen(AppState& s);
// WM_ADOPT_OPEN_POLICY: that toast was clicked; the setting changes and saves.
void AdoptSuggestedOpenPolicy(AppState& s);

// The message loop: records key presses, clicks and wheel turns in Pulse
// windows (child controls included) as AppState::last_input_tick.
void NoteUserInput(AppState& any, const MSG& msg);

}  // namespace pulse
