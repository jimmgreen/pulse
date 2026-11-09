#include "ui_renderer_internal.h"
#include "pane_header_icons.h"
#include "command_icons.h"

namespace pulse::ui {

void MainRenderer::DrawPaneHeaderIcon(const D2D1_RECT_F& rc, PaneHeaderIcon icon,
                                      const D2D1_COLOR_F& color) {
    auto* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc) return;
    if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
    using I = command_icons::Icon;
    I command = I::None;
    switch (icon) {
    case PaneHeaderIcon::Back: command = I::Back; break;
    case PaneHeaderIcon::Forward: command = I::Forward; break;
    case PaneHeaderIcon::Up: command = I::Up; break;
    case PaneHeaderIcon::Columns: command = I::Columns; break;
    case PaneHeaderIcon::MediumIcons: command = I::Grid; break;
    case PaneHeaderIcon::More: command = I::More; break;
    case PaneHeaderIcon::Split: command = I::Split; break;
    case PaneHeaderIcon::SplitSingle: command = I::SplitSingle; break;
    case PaneHeaderIcon::SplitStacked: command = I::SplitStacked; break;
    case PaneHeaderIcon::SplitThree: command = I::SplitThree; break;
    case PaneHeaderIcon::SplitFour: command = I::SplitFour; break;
    case PaneHeaderIcon::View: command = I::List; break;
    case PaneHeaderIcon::Filter: command = I::Filter; break;
    case PaneHeaderIcon::SwapHorizontal: command = I::SwapHorizontal; break;
    case PaneHeaderIcon::SwapVertical: command = I::SwapVertical; break;
    case PaneHeaderIcon::Size: command = I::Sliders; break;
    case PaneHeaderIcon::Expand: command = I::PaneExpand; break;
    case PaneHeaderIcon::Restore: command = I::PaneRestore; break;
    case PaneHeaderIcon::Close: command = I::Close; break;
    case PaneHeaderIcon::Wall: break;  // drawn below
    }
    MakeBrush(dc, color, brText_);
    // In the toolbar the layout icons stand with the command icons.
    const float box_dip = icon_stroke_px_ > 0.0f ? icon_box_dip_ : 18.0f;
    D2D1_RECT_F bounds = command_icons::CenteredBounds(rc, box_dip * scale_);
    if (icon_stroke_px_ > 0.0f) {
        const float side = bounds.right - bounds.left;
        bounds.left = std::round(bounds.left);
        bounds.top = std::round(bounds.top);
        bounds.right = bounds.left + side;
        bounds.bottom = bounds.top + side;
    }
    if (icon == PaneHeaderIcon::Wall) {
        // Photo wall: two justified rows (the prototype's glyph) on the command
        // icons' canvas and stroke; one button needs no shared Icon entry.
        const float k = (bounds.right - bounds.left) / command_icons::kCanvas;
        D2D1_MATRIX_3X2_F previous{};
        dc->GetTransform(&previous);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k) *
                         D2D1::Matrix3x2F::Translation(bounds.left, bounds.top) * previous);
        for (const D2D1_RECT_F& box : {D2D1::RectF(3, 3, 13, 10), D2D1::RectF(15, 3, 21, 10),
                                       D2D1::RectF(3, 14, 9, 21), D2D1::RectF(11, 14, 21, 21)})
            dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 1.5f, 1.5f), brText_.get(),
                                     command_icons::kStroke, paneHeaderStroke_.get());
        dc->SetTransform(previous);
        return;
    }
    const float side = bounds.right - bounds.left;
    const float width = icon_stroke_px_ > 0.0f && side > 0.0f
        ? icon_stroke_px_ * command_icons::kCanvas / side : command_icons::kStroke;
    // The toolbar's split button at rest takes its accent part like the pins.
    const auto tone = icon_tones_ && !IsHighContrast() ? command_icons::ToneOf(command)
                                                       : command_icons::Tone::None;
    ID2D1Brush* accent = nullptr;
    if (tone != command_icons::Tone::None) {
        MakeBrush(dc, icon_tone_colors_[static_cast<size_t>(tone)], brIconTone_);
        accent = brIconTone_.get();
    }
    command_icons::DrawTwoTone(dc, brText_.get(), accent, paneHeaderStroke_.get(), command, bounds, 1.0f, width);
}

} // namespace pulse::ui
