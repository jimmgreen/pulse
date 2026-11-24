#pragma once
#include "FluentTokens.h"
#include <d2d1_1.h>
#include <wrl/client.h>
#include <algorithm>

namespace pulse::ui {
// Shared by the main file pane and file picker. Brushes live for one draw pass.
class FileItemSelectionPainter {
public:
    FileItemSelectionPainter(ID2D1DeviceContext* dc, const Theme& theme, float scale,
                             bool active, bool high_contrast, bool outline,
                             bool split_mode = false)
        : dc_(dc), theme_(theme), scale_(scale), active_(active),
          outline_(split_mode ? (active || high_contrast) : outline),
          split_mode_(split_mode) {
        const bool dark = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
        const D2D1_COLOR_F accent_ink = (!high_contrast && dark)
            ? MixColor(theme.accent, HexColor(0xFFFFFF), 0.28f) : theme.accent;
        const D2D1_COLOR_F fill_color = split_mode && !high_contrast
            ? (active ? WithAlpha(accent_ink, dark ? 0.22f : 0.13f)
                      : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.075f)
                              : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f)))
            : (active ? theme.fill_selected : theme.fill_selected_inactive);
        const D2D1_COLOR_F stroke_color = split_mode && !high_contrast
            ? WithAlpha(accent_ink, dark ? 0.38f : 0.28f)
            : (active ? theme.list_selected_outline : WithAlpha(theme.text, 0.30f));
        dc_->CreateSolidColorBrush(fill_color, &fill_);
        dc_->CreateSolidColorBrush(split_mode ? accent_ink : theme.accent, &accent_);
        dc_->CreateSolidColorBrush(stroke_color, &outline_brush_);
        dc_->CreateSolidColorBrush(theme.fill_hover, &hover_);
        if (active && !high_contrast && !split_mode) {
            const D2D1_GRADIENT_STOP stops[] = {{0, theme.list_selected_start}, {1, theme.list_selected_end}};
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
            if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, 2, &collection)))
                dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {1, 0}), collection.Get(), &gradient_);
        }
    }
    static D2D1_RECT_F Bounds(D2D1_RECT_F cell, float scale) {
        return {cell.left + 4 * scale, cell.top + scale,
                (std::max)(cell.left + 4 * scale, cell.right - 4 * scale),
                (std::max)(cell.top + scale, cell.bottom - scale)};
    }
    void Hover(D2D1_RECT_F cell) const {
        const float radius = (split_mode_ ? 6.0f : theme_.radius_control) * scale_;
        if (hover_) dc_->FillRoundedRectangle(D2D1::RoundedRect(Bounds(cell, scale_),
            radius, radius), hover_.Get());
    }
    // bar: the accent bar at the left edge (off where an item check box marks the row).
    void Selected(D2D1_RECT_F cell, bool bar = true) const {
        const auto sel = Bounds(cell, scale_);
        const float radius = (split_mode_ ? 6.0f : theme_.radius_control) * scale_;
        const auto rr = D2D1::RoundedRect(sel, radius, radius);
        if (gradient_) {
            gradient_->SetStartPoint({sel.left, 0}); gradient_->SetEndPoint({sel.right, 0});
            dc_->FillRoundedRectangle(rr, gradient_.Get());
        } else if (fill_) dc_->FillRoundedRectangle(rr, fill_.Get());
        if (outline_ && outline_brush_ && sel.right - sel.left > 2 * scale_) {
            const float half = 0.5f * scale_;
            const float in_r = (std::max)(0.0f, radius - half);
            dc_->DrawRoundedRectangle(D2D1::RoundedRect({sel.left + half, sel.top + half,
                sel.right - half, sel.bottom - half}, in_r, in_r), outline_brush_.Get(), scale_);
        }
        if (bar && active_ && accent_) {
            if (split_mode_) {
                const float height = (std::max)(8.0f * scale_, sel.bottom - sel.top - 12.0f * scale_);
                const float y = (sel.top + sel.bottom - height) * 0.5f;
                dc_->FillRoundedRectangle(D2D1::RoundedRect({sel.left + 0.5f * scale_, y,
                    sel.left + 3.5f * scale_, y + height}, 1.5f * scale_, 1.5f * scale_), accent_.Get());
            } else {
                const float height = (std::min)(18 * scale_, (std::max)(0.0f, sel.bottom - sel.top - 8 * scale_));
                const float y = (sel.top + sel.bottom - height) * 0.5f;
                dc_->FillRoundedRectangle(D2D1::RoundedRect({sel.left + scale_, y,
                    sel.left + 3 * scale_, y + height}, scale_, scale_), accent_.Get());
            }
        }
    }
private:
    ID2D1DeviceContext* dc_;
    const Theme& theme_;
    float scale_;
    bool active_, outline_, split_mode_ = false;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill_, accent_, outline_brush_, hover_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradient_;
};
}
