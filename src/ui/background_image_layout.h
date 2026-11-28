// background_image_layout.h - pure geometry for the background image layer.
//
// Deliberately independent of the compositor so the wallpaper placement can be
// checked without a D3D device.
#pragma once

#include <d2d1.h>

namespace pulse::ui {

// Stored as an int in the preferences (background_fit).
enum class ImageFit : int { Contain = 0, Cover = 1, Stretch = 2, Original = 3 };

// The part of the window the image may paint in. `position` is row-major
// 0..8 (0 top-left ... 8 bottom-right); 4 is the whole area and also the
// fallback for an out-of-range value. The other cells take `ratio_percent`
// (clamped to 0..100) of each axis and stick to their edge or corner. A
// degenerate area is returned unchanged.
D2D1_RECT_F ImagePlacementRect(const D2D1_RECT_F& area, int position, int ratio_percent) noexcept;

// Where the image lands inside that region, always centred. Stretch fills the
// region, Contain and Cover keep the aspect ratio, Original stays at 1:1 (a
// larger image overflows and the caller clips it). A non-positive image size
// or an empty area returns the placement instead of NaN.
D2D1_RECT_F ImageDrawRect(float image_w, float image_h, const D2D1_RECT_F& area,
                          int position, int ratio_percent, ImageFit fit) noexcept;

} // namespace pulse::ui
