#include "last_tab_close.h"

namespace pulse::app {

bool LastTabClosesWindow(size_t tab_count, bool tab_pinned, bool enabled) noexcept {
    return enabled && tab_count == 1 && !tab_pinned;
}

bool TabDoubleClickCloses(bool enabled, const void* pressed_tab, const void* double_clicked_tab,
                          unsigned long elapsed_ms, unsigned long double_click_ms) noexcept {
    return enabled && pressed_tab && pressed_tab == double_clicked_tab && elapsed_ms <= double_click_ms;
}

} // namespace pulse::app
