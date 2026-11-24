#pragma once

#include <d2d1_1.h>
#include <dwrite.h>

#include "host/model/desktop_model.h"

namespace pulse::desktop::overlays {

// The 添加小组件 gallery's icon for a widget kind: a small drawn scene (sun over a cloud, a calendar page,
// a clock face…) on a lit two-tone squircle, in the manner of a phone's widget gallery. `number_format`
// draws the calendar's date; `rect` is the badge (34×34 DIP in the gallery).
void DrawWidgetIcon(ID2D1DeviceContext* dc, model::TileKind kind, const D2D1_RECT_F& rect, float radius,
                    IDWriteTextFormat* number_format);

} // namespace pulse::desktop::overlays
