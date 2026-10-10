#pragma once

#include <cstddef>

namespace pulse::app {

// Settings > Startup and close: closing the only tab closes the window, like a
// browser. Off by default; pinned tabs never close this way. The window then
// follows the normal close path (it hides to the tray when Pulse keeps running).
bool LastTabClosesWindow(size_t tab_count, bool tab_pinned, bool enabled) noexcept;

// Settings > Startup and close: double-clicking a tab closes it, like its close
// button. Off by default. Both presses must land on the same tab itself: when
// the first one hits a close button, the next tab slides under the pointer and
// must not close too. pressed_tab is the tab the first press landed on (null
// when it was not a tab), elapsed_ms the time since that press.
bool TabDoubleClickCloses(bool enabled, const void* pressed_tab, const void* double_clicked_tab,
                          unsigned long elapsed_ms, unsigned long double_click_ms) noexcept;

} // namespace pulse::app
