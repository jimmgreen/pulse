#pragma once
#include <windows.h>
namespace pulse::app {
// Endpoint samples, not a claim that every intermediate frame was observed.
struct ExplorerSourceObservation {
    bool alive = false, visible = false, enabled = false, alpha_read = false;
    LONG_PTR style = 0;
    BYTE alpha = 0;
    DWORD flags = 0;
    COLORREF color = 0;
};
struct ExplorerSourceSamples {
    bool unchanged = true;
    unsigned before_close = 0, transition = 0, authorized = 0;
    void Record(bool authorized_before, bool authorized_after, bool available_unchanged) {
        if (!authorized_before && !authorized_after) {
            unchanged = unchanged && available_unchanged;
            ++before_close;
        } else if (!authorized_before) ++transition;
        else ++authorized;
    }
};
inline ExplorerSourceObservation ObserveExplorerSource(HWND window) {
    ExplorerSourceObservation value;
    value.alive = IsWindow(window) != FALSE;
    value.visible = IsWindowVisible(window) != FALSE;
    value.enabled = IsWindowEnabled(window) != FALSE;
    value.style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    value.alpha_read = !(value.style & WS_EX_LAYERED) ||
        GetLayeredWindowAttributes(window, &value.color, &value.alpha, &value.flags) != FALSE;
    return value;
}
inline bool UnchangedAvailableSource(const ExplorerSourceObservation& before, const ExplorerSourceObservation& after) {
    return before.alive && before.visible && before.enabled && before.alpha_read &&
        after.alive && after.visible && after.enabled && after.alpha_read &&
        before.style == after.style && before.alpha == after.alpha && before.flags == after.flags && before.color == after.color;
}
}
