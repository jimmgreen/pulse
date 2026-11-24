#pragma once
// Pure placement math shared by CenterOwnedWindow and the dialogs. Kept free
// of window calls so pulse_dialog_dpi_placement_test can drive it with simulated
// monitors of mixed scale (a 100% and a 150% screen side by side).
#include <windows.h>

#include <algorithm>

namespace pulse::ui::placement {

// Pixel length of `dip` on a monitor of `dpi`. Same rounding as the dialogs'
// historical `static_cast<int>(dip * scale)` so sizes do not shift by a pixel.
inline int DipToPx(float dip, UINT dpi) noexcept {
    return static_cast<int>(dip * (static_cast<float>(dpi ? dpi : 96) / 96.0f));
}

// A width x height rect centred on `anchor`, optionally pushed inside `work`.
inline RECT CenteredRect(const RECT& anchor, const RECT* work, int width, int height) noexcept {
    int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
    if (work) {
        x = std::clamp(x, static_cast<int>(work->left),
                       (std::max)(static_cast<int>(work->left), static_cast<int>(work->right) - width));
        y = std::clamp(y, static_cast<int>(work->top),
                       (std::max)(static_cast<int>(work->top), static_cast<int>(work->bottom) - height));
    }
    return RECT{ x, y, x + width, y + height };
}

// Where a window should sit after WM_DPICHANGED: the size always comes from
// the dialog's own DIP layout at the new DPI. Windows' suggested rect only
// rescales the current pixel size, so a window whose pixels were sized for a
// different DPI than the one it was born with (CW_USEDEFAULT lands on the
// primary monitor) would keep the mismatch and draw past its edges.
inline RECT DpiChangedRect(const RECT& suggested, float width_dip, float height_dip, UINT dpi) noexcept {
    const int width = DipToPx(width_dip, dpi);
    const int height = DipToPx(height_dip, dpi);
    return RECT{ suggested.left, suggested.top, suggested.left + width, suggested.top + height };
}

// What a window reports after a placement step.
struct Placed {
    UINT dpi = 96;
    int width = 0;
    int height = 0;
};

// Centre-at-DIP-size loop behind CenterOwnedWindowDip. `place(width, height)`
// centres the window at that pixel size; `observe()` reports the DPI and size
// it ended with - the move may cross onto a screen of another scale and run
// WM_DPICHANGED in between. Repeats until size and DPI agree; returns the DPI.
template <class Place, class Observe>
UINT SettleDipSize(UINT dpi, float width_dip, float height_dip, Place&& place, Observe&& observe) {
    for (int pass = 0; pass < 3; ++pass) {
        const int width = DipToPx(width_dip, dpi);
        const int height = DipToPx(height_dip, dpi);
        place(width, height);
        const Placed now = observe();
        if (now.dpi == dpi && now.width == width && now.height == height) break;
        dpi = now.dpi;
    }
    return dpi;
}

// True when a dialog's scale no longer matches the DPI of its monitor (a
// hidden window can be moved across screens without seeing WM_DPICHANGED).
inline bool NeedsDpiSync(float scale, UINT dpi) noexcept {
    return dpi != 0 && dpi != static_cast<UINT>(scale * 96.0f + 0.5f);
}

} // namespace pulse::ui::placement
