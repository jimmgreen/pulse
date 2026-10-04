// type_ahead.h — Keyboard type-ahead for the file list (#93).
#pragma once
#include "app_runtime.h"

namespace pulse::app {

// Consumes one WM_CHAR: selects (and scrolls to) the next entry in the
// current view order whose name starts with `ch`, wrapping at the end.
// Returns true when the character was consumed. Even a character with no
// match is consumed so DefWindowProcW cannot beep at it.
bool HandleTypeAheadChar(AppState& s, wchar_t ch);

} // namespace pulse::app
