// fluent_color_svg.h — the color Fluent artwork (assets/fluent/*_color.svg)
// shared by MainRenderer and the flyouts that list the same rows (编辑快速访问).
#pragma once

#include "ui_compositor.h"

#include <d2d1svg.h>
#include <string_view>

namespace pulse::ui {

// Resource id of the color artwork for a sidebar glyph, 0 when there is none.
int FluentSvgIdForGlyph(std::wstring_view glyph);

// Parses the RCDATA resource; when Direct2D rejects the markup, retries with
// solid fills from the first gradient stops. False when nothing parses.
bool LoadFluentSvg(ID2D1DeviceContext5* dc, int resource_id, ComPtr<ID2D1SvgDocument>& out, HMODULE module = nullptr);

// Draws a 24-unit document as the largest square centred in bounds.
bool DrawFluentSvgFitted(ID2D1DeviceContext5* dc, ID2D1SvgDocument* svg,
                         const D2D1_RECT_F& bounds, float opacity);

} // namespace pulse::ui
