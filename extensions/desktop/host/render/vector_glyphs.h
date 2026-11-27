// vector_glyphs.h - The desktop layer's icons, drawn as vector outlines instead of Segoe Fluent font glyphs.
// Call sites keep the font's code points as names; DrawGlyph has DrawText's shape so they swap one for one.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

namespace pulse::desktop::render {

// Draws the outline icon for `glyph` as a size x size square placed in `box` (centred). False when the
// glyph has no drawn icon.
bool DrawVectorGlyph(ID2D1DeviceContext* dc, wchar_t glyph, const D2D1_RECT_F& box, float size, ID2D1Brush* brush);

// DrawText's signature for a one-character icon: placed by the format's alignment, sized by its font size.
// Falls back to the font when the glyph has no drawn icon.
void DrawGlyph(ID2D1DeviceContext* dc, const wchar_t* text, UINT32 length, IDWriteTextFormat* format,
               const D2D1_RECT_F& rect, ID2D1Brush* brush,
               D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE);

// SVG path data drawn into `box` from a view x view square (centred, aspect kept). `path` must be a string
// literal: the geometry is cached per pointer. Filled, or stroked `stroke_view` view units wide with round ends.
void DrawSvgPath(ID2D1DeviceContext* dc, const char* path, const D2D1_RECT_F& box, float view, ID2D1Brush* brush,
                 bool fill, float stroke_view = 1.7f);

} // namespace pulse::desktop::render
