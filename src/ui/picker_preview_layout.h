#pragma once
#include <d2d1.h>
#include <algorithm>
#include <array>

namespace pulse::ui {
inline constexpr bool PickerSpaceTogglesPreview(bool list_focus, bool ctrl, bool shift, bool alt) {
    return list_focus && !ctrl && !shift && !alt;
}

struct PickerPreviewLayout {
    D2D1_RECT_F preview{}, name{}, subtitle{}, modified{}, location{}, enter_archive{}, copy_path{}, reveal{}, footer_hint{};
};

// Keep actions inside the panel even in a short Save dialog: give up preview
// height first, then optional metadata. Draw and hit testing share these bounds.
inline PickerPreviewLayout LayoutPickerPreview(D2D1_RECT_F panel, float scale, float desired_preview, bool is_archive = false) {
    PickerPreviewLayout out;
    if (scale <= 0.0f) return out;
    const float pad = 12.0f * scale, gap = 6.0f * scale, button = 32.0f * scale;
    const float left = panel.left + pad, right = panel.right - pad;
    const float top = panel.top + pad, bottom = panel.bottom - pad;
    const float width = right - left;
    const float hint_h = 20.0f * scale;
    const bool show_hint = bottom - top >= button * 2 + hint_h + gap * 2;
    const float actions_bottom = show_hint ? bottom - hint_h - gap : bottom;
    if (show_hint) out.footer_hint = {left, bottom - hint_h, right, bottom};
    const bool stacked = width < 260.0f * scale;
    const float pair_h = stacked ? button * 2 + gap : button;
    const float actions = pair_h + (is_archive ? button + gap : 0.0f);
    if (width < 64.0f * scale || actions_bottom - top < actions) return out;
    if (stacked) {
        out.copy_path = {left, actions_bottom - pair_h, right, actions_bottom - button - gap};
        out.reveal = {left, actions_bottom - button, right, actions_bottom};
    } else {
        const float middle = (left + right) / 2.0f;
        out.copy_path = {left, actions_bottom - button, middle - gap / 2, actions_bottom};
        out.reveal = {middle + gap / 2, actions_bottom - button, right, actions_bottom};
    }
    if (is_archive) {
        out.enter_archive = {left, actions_bottom - actions, right, actions_bottom - pair_h - gap};
    }
    const float metadata_bottom = actions_bottom - actions - gap;
    const float reserved_metadata = (24.0f + 20.0f + 20.0f + 32.0f + 3.0f * 4.0f) * scale;
    float preview = (std::min)((std::max)(0.0f, desired_preview), (std::max)(0.0f, metadata_bottom - top - reserved_metadata - gap));
    if (preview < 48.0f * scale) preview = 0.0f;
    float y = top;
    if (preview > 0.0f) { out.preview = {left, y, right, y + preview}; y += preview + gap; }
    const auto row = [&](D2D1_RECT_F& target, float height) {
        const float pixels = height * scale;
        if (y + pixels <= metadata_bottom) {
            target = {left, y, right, y + pixels};
            y += pixels + 4.0f * scale;
        }
    };
    row(out.name, 24.0f);
    row(out.subtitle, 20.0f);
    row(out.modified, 20.0f);
    row(out.location, 32.0f);
    return out;
}

struct PickerSavePreviewLayout {
    D2D1_RECT_F heading{};
    std::array<D2D1_RECT_F, 5> kv_rows{};
    D2D1_RECT_F banner{};
    D2D1_RECT_F divider{};
    D2D1_RECT_F subheading{};
    D2D1_RECT_F after_show{};
    D2D1_RECT_F after_copy{};
    std::array<D2D1_RECT_F, 3> tray_bullets{};
};

// Shared geometry for the Save destination preview panel ("保存到这里" / "先放进暂存托盘").
// Reserves the bottom "保存后" checkboxes first so narrow/short dialogs keep them clickable.
inline PickerSavePreviewLayout LayoutPickerSavePreview(D2D1_RECT_F panel, float scale, bool to_tray) {
    PickerSavePreviewLayout out;
    if (scale <= 0.0f) return out;
    const float pad = 12.0f * scale;
    const float left = panel.left + pad, right = panel.right - pad;
    const float top = panel.top + pad, bottom = panel.bottom - pad;
    if (right - left < 80.0f * scale || bottom - top < 80.0f * scale) return out;
    float y = top;
    const auto take = [&](D2D1_RECT_F& target, float h_dip, float gap_dip, float limit) {
        const float h = h_dip * scale;
        if (y + h <= limit) {
            target = {left, y, right, y + h};
            y += h + gap_dip * scale;
            return true;
        }
        return false;
    };
    if (!to_tray) {
        // Reserve room for "保存后" heading + 2 checkboxes so they never clip even on short dialogs.
        const float after_h = (1.0f + 10.0f + 20.0f + 6.0f + 26.0f + 4.0f + 26.0f) * scale;
        const float upper_limit = (std::max)(top + 24.0f * scale, bottom - after_h - 6.0f * scale);
        take(out.heading, 24.0f, 8.0f, upper_limit);
        for (int i = 0; i < 5; ++i) take(out.kv_rows[static_cast<size_t>(i)], 21.0f, 3.0f, upper_limit);
        const float banner_h = (std::min)(54.0f * scale, (std::max)(0.0f, upper_limit - y - 4.0f * scale));
        if (banner_h >= 28.0f * scale) {
            out.banner = {left, y + 4.0f * scale, right, y + 4.0f * scale + banner_h};
            y = out.banner.bottom + 8.0f * scale;
        }
        float ay = (std::min)(y, bottom - after_h);
        if (ay < top) ay = top;
        out.divider = {left, ay, right, ay + (std::max)(1.0f, scale)};
        ay += 10.0f * scale;
        if (ay + 20.0f * scale <= bottom) {
            out.subheading = {left, ay, right, ay + 20.0f * scale};
            ay += 24.0f * scale;
        }
        if (ay + 26.0f * scale <= bottom) {
            out.after_show = {left, ay, right, ay + 26.0f * scale};
            ay += 30.0f * scale;
        }
        if (ay + 26.0f * scale <= bottom) {
            out.after_copy = {left, ay, right, ay + 26.0f * scale};
        }
        return out;
    }
    take(out.heading, 24.0f, 8.0f, bottom);
    for (int i = 0; i < 3; ++i) take(out.kv_rows[static_cast<size_t>(i)], 21.0f, 3.0f, bottom);
    if (y + 68.0f * scale <= bottom) {
        out.banner = {left, y + 6.0f * scale, right, y + 74.0f * scale};
        y = out.banner.bottom + 10.0f * scale;
    }
    if (y + 12.0f * scale <= bottom) {
        out.divider = {left, y, right, y + (std::max)(1.0f, scale)};
        y += 10.0f * scale;
    }
    take(out.subheading, 20.0f, 6.0f, bottom);
    for (int i = 0; i < 3; ++i) take(out.tray_bullets[static_cast<size_t>(i)], 34.0f, 4.0f, bottom);
    return out;
}
} // namespace pulse::ui
