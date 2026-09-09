#pragma once

#include "FluentTokens.h"
#include "fluent_components.h"
#include "ui_compositor.h"

#include <d2d1.h>
#include <windows.h>

namespace pulse::ui {

float ScaleDip(float scale, float value);
D2D1_RECT_F DipRect(float scale, float x, float y, float width, float height);
bool ContainsRect(const D2D1_RECT_F& rect, float x, float y);

LRESULT BorderlessHitTest(HWND hwnd, LPARAM lparam, float title_height,
                          const D2D1_RECT_F& client_buttons);
bool ApplyBackdrop(HWND hwnd, bool dark);
void CenterOwnedWindow(HWND hwnd, HWND owner, int width, int height,
                       bool clamp_to_work_area = true);

void BeginSurface(Compositor& compositor, fluent::Painter& painter,
                  const Theme& theme, bool dark, bool high_contrast,
                  bool backdrop_enabled, float scale);
void EndSurface(Compositor& compositor);

} // namespace pulse::ui
