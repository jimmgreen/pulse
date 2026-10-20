#pragma once

#include "view_layout.h"
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <algorithm>

namespace pulse::ui {

// Touch density choices (corresponding to touch_ui_mode preference)
enum class TouchDensity {
    Compact,
    Touch,
};

// Item checkbox display policy (corresponding to item_checkboxes preference)
enum class CheckboxVisibility {
    Auto,
    Always,
    Never,
};

struct ItemSelectionGeometry {
    D2D1_RECT_F visual_rect{};       // Visual 18-20 DIP checkbox bounding box
    D2D1_RECT_F hit_rect{};          // Interactive hit box (>= 44x44 DIP in touch density)
    float leading_slot_width = 0.0f; // Leading slot width reserved for the checkbox
    bool visible = false;            // Whether the checkbox should be drawn
};

struct HeaderSelectionGeometry {
    D2D1_RECT_F visual_rect{};       // Visual checkbox box inside column header
    D2D1_RECT_F hit_rect{};          // Interactive hit box (prioritizes over sorting click)
    float leading_slot_width = 0.0f; // Slot occupied in the header
};

// Calculates item selection geometry for any of the 8 view modes.
// item_rect: full bounding box of the item in client/viewport pixel space.
// scale: DPI scale factor (e.g. 1.0, 1.5, 2.0).
// density: Compact or Touch density.
// is_selected: whether this item is currently selected.
// is_hovered: whether pointer is currently over this item.
// has_touchscreen_ready: whether a touchscreen digitizer is detected.
// policy: Auto, Always, or Never.
inline ItemSelectionGeometry ComputeItemSelectionGeometry(
    ViewMode mode,
    const D2D1_RECT_F& item_rect,
    float scale,
    TouchDensity density,
    bool is_selected,
    bool is_hovered,
    bool has_touchscreen_ready,
    CheckboxVisibility policy = CheckboxVisibility::Auto) noexcept {

    ItemSelectionGeometry geom;
    scale = (std::max)(0.5f, scale);

    // Determine visibility based on policy:
    // - Never: never visible
    // - Always: always visible
    // - Auto: if touchscreen is ready, always visible; otherwise visible on hover or when selected
    if (policy == CheckboxVisibility::Never) {
        geom.visible = false;
    } else if (policy == CheckboxVisibility::Always) {
        geom.visible = true;
    } else {
        geom.visible = has_touchscreen_ready || is_selected || is_hovered;
    }

    const float item_h = (std::max)(0.0f, item_rect.bottom - item_rect.top);
    const bool is_touch = (density == TouchDensity::Touch);

    const float box_size_dip = 18.0f;
    const float box_size_px = box_size_dip * scale;
    const float hit_min_dip = is_touch ? 44.0f : 24.0f;
    const float hit_min_px = hit_min_dip * scale;

    const bool is_grid = (mode == ViewMode::ExtraLargeIcons ||
                          mode == ViewMode::LargeIcons ||
                          mode == ViewMode::MediumIcons);

    if (is_grid) {
        // Icon grid: checkbox rests at top-left corner of the item cell.
        // It does not displace the thumbnail/icon horizontally, but overlays the top-left card corner.
        const float pad_px = 6.0f * scale;
        geom.visual_rect.left = item_rect.left + pad_px;
        geom.visual_rect.top = item_rect.top + pad_px;
        geom.visual_rect.right = geom.visual_rect.left + box_size_px;
        geom.visual_rect.bottom = geom.visual_rect.top + box_size_px;

        // Hit box centered on visual box, clamped to item bounds
        const float center_x = (geom.visual_rect.left + geom.visual_rect.right) * 0.5f;
        const float center_y = (geom.visual_rect.top + geom.visual_rect.bottom) * 0.5f;
        const float half_hit = hit_min_px * 0.5f;

        geom.hit_rect.left = (std::max)(item_rect.left, center_x - half_hit);
        geom.hit_rect.top = (std::max)(item_rect.top, center_y - half_hit);
        geom.hit_rect.right = (std::min)(item_rect.right, center_x + half_hit);
        geom.hit_rect.bottom = (std::min)(item_rect.bottom, center_y + half_hit);

        geom.leading_slot_width = 0.0f; // Overlay, does not shift name below
    } else {
        // Linear rows: Details, List, SmallIcons, Tiles, Content.
        // Checkbox is aligned to the leading edge (left) and vertically centered in the row.
        const float pad_x_px = 4.0f * scale;
        const float slot_dip = is_touch ? 40.0f : 26.0f;
        const float slot_px = slot_dip * scale;

        geom.leading_slot_width = slot_px;

        geom.visual_rect.left = item_rect.left + pad_x_px + (slot_px - pad_x_px - box_size_px) * 0.5f;
        geom.visual_rect.right = geom.visual_rect.left + box_size_px;
        geom.visual_rect.top = item_rect.top + (item_h - box_size_px) * 0.5f;
        geom.visual_rect.bottom = geom.visual_rect.top + box_size_px;

        // Hit box covers the leading slot and full row height (or at least hit_min_px)
        geom.hit_rect.left = item_rect.left;
        geom.hit_rect.right = item_rect.left + (std::max)(slot_px, hit_min_px);
        const float center_y = item_rect.top + item_h * 0.5f;
        geom.hit_rect.top = (std::max)(item_rect.top, center_y - hit_min_px * 0.5f);
        geom.hit_rect.bottom = (std::min)(item_rect.bottom, center_y + hit_min_px * 0.5f);
    }

    return geom;
}

// Calculates header selection geometry (for Details view Name column).
inline HeaderSelectionGeometry ComputeHeaderSelectionGeometry(
    const D2D1_RECT_F& name_col_rect,
    float scale,
    TouchDensity density) noexcept {

    HeaderSelectionGeometry geom;
    scale = (std::max)(0.5f, scale);
    const bool is_touch = (density == TouchDensity::Touch);

    const float col_h = (std::max)(0.0f, name_col_rect.bottom - name_col_rect.top);
    const float box_size_px = 18.0f * scale;
    const float slot_dip = is_touch ? 40.0f : 26.0f;
    const float slot_px = slot_dip * scale;
    const float pad_x_px = 4.0f * scale;

    geom.leading_slot_width = slot_px;
    geom.visual_rect.left = name_col_rect.left + pad_x_px + (slot_px - pad_x_px - box_size_px) * 0.5f;
    geom.visual_rect.right = geom.visual_rect.left + box_size_px;
    geom.visual_rect.top = name_col_rect.top + (col_h - box_size_px) * 0.5f;
    geom.visual_rect.bottom = geom.visual_rect.top + box_size_px;

    // Hit box prioritizes over column sort click, but avoids touching the right column divider
    const float hit_w = (std::max)(slot_px, is_touch ? 44.0f * scale : 26.0f * scale);
    geom.hit_rect.left = name_col_rect.left;
    geom.hit_rect.right = (std::min)(name_col_rect.left + hit_w, name_col_rect.right - 8.0f * scale);
    geom.hit_rect.top = name_col_rect.top;
    geom.hit_rect.bottom = name_col_rect.bottom;

    return geom;
}

// Effective touch row height helper
inline float EffectiveRowHeightDip(float base_row_height_dip, TouchDensity density) noexcept {
    if (density == TouchDensity::Touch) {
        return (std::max)(44.0f, base_row_height_dip);
    }
    return base_row_height_dip > 0.0f ? base_row_height_dip : 28.0f;
}

// Touch digitizer detection helper
inline bool DetectTouchscreenReady() noexcept {
    const int digitizer = GetSystemMetrics(SM_DIGITIZER);
    // NID_READY (0x80) indicates digitizer is ready and input is enabled
    // NID_INTEGRATED_TOUCH (0x01) or NID_EXTERNAL_TOUCH (0x02) indicates touch
    constexpr int kReadyTouch = 0x80 | 0x01;
    constexpr int kReadyExternalTouch = 0x80 | 0x02;
    if ((digitizer & kReadyTouch) == kReadyTouch || (digitizer & kReadyExternalTouch) == kReadyExternalTouch) {
        return GetSystemMetrics(SM_MAXIMUMTOUCHES) > 0;
    }
    return false;
}

// Resolves effective touch density from preference and hardware capability
inline TouchDensity ResolveTouchDensity(const std::wstring& touch_ui_mode_pref,
                                        bool touchscreen_ready) noexcept {
    if (touch_ui_mode_pref == L"touch") return TouchDensity::Touch;
    if (touch_ui_mode_pref == L"compact") return TouchDensity::Compact;
    // "auto": touch if touchscreen is ready, compact otherwise
    return touchscreen_ready ? TouchDensity::Touch : TouchDensity::Compact;
}

// Resolves checkbox visibility policy from preference
inline CheckboxVisibility ResolveCheckboxVisibility(const std::wstring& pref) noexcept {
    if (pref == L"always") return CheckboxVisibility::Always;
    if (pref == L"never") return CheckboxVisibility::Never;
    return CheckboxVisibility::Auto;
}

} // namespace pulse::ui
