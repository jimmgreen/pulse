// drag_ghost.h - the tab card that follows the cursor while a tab is dragged
// over another Pulse window.
//
// The window that owns the tab cannot draw outside its own client area (the
// floating block inside the strip covers that case), so a cross-window drag
// needs a window of its own: a layered, topmost, never-activated popup that is
// transparent to the mouse. Explorer draws the same thing, from a snapshot of
// the dragged tab.
#pragma once

#include <windows.h>
#include <string>

namespace pulse::ui {

class TabDragGhost {
public:
    TabDragGhost() = default;
    ~TabDragGhost() {
        // DestroyWindow is a user32 call, so the destructor stays inline: a panel
        // that merely owns an AppState never needs this file's implementation.
        if (hwnd_) DestroyWindow(hwnd_);
    }
    TabDragGhost(const TabDragGhost&) = delete;
    TabDragGhost& operator=(const TabDragGhost&) = delete;

    // Paints the card for one dragged tab. `title` is the label under the
    // cursor's tab, `dark` picks the palette the window is running in, and
    // (anchor_x, anchor_y) is where inside the original tab it was grabbed: the
    // card hangs by that offset, so it lands where the tab used to be instead of
    // jumping away from the pointer.
    void Show(float scale, const std::wstring& title, bool dark,
              int anchor_x, int anchor_y);
    // Anchors the card by the grab offset. Moves only: the z-order is set once
    // when the window is created (re-asserting it per frame is what makes a
    // preview trail its owner).
    void Follow(POINT screen_point);
    void Hide();
    bool visible() const { return hwnd_ != nullptr && visible_; }

private:
    bool EnsureWindow();

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int anchor_x_ = 0;
    int anchor_y_ = 0;
    bool visible_ = false;
};

} // namespace pulse::ui
