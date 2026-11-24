#pragma once

#include <d2d1.h>

namespace pulse::ui {

class QuickPreviewWindow;

// Subtract both parent overlays from the native child, not the space between them.
void ClipEmbeddedPreview(QuickPreviewWindow& preview, const D2D1_RECT_F& first, const D2D1_RECT_F& second);

} // namespace pulse::ui
