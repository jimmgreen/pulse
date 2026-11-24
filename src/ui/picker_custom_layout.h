#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {
constexpr uint32_t kPickerCustomSeparator = 9;
constexpr uint32_t kPickerCustomText = 10;

inline bool PickerCustomInteractive(uint32_t kind) {
    return kind != 1 && kind != 5 && kind != kPickerCustomSeparator && kind != kPickerCustomText;
}

// Formats Win32 IFileDialogCustomize labels for Fluent DirectWrite display:
// unescapes "&&" -> "&", strips CJK/Win32 mnemonic suffixes like "(&C)" / "（&T）",
// and strips bare accelerator markers "&X" -> "X". Raw labels stay intact for callbacks.
inline std::wstring FormatCustomControlCaption(std::wstring_view raw) {
    auto is_mnemonic_key = [](wchar_t ch) noexcept {
        return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9');
    };
    std::wstring out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        if (i + 3 < raw.size() &&
            ((raw[i] == L'(' && raw[i + 1] == L'&' && is_mnemonic_key(raw[i + 2]) && raw[i + 3] == L')') ||
             (raw[i] == L'\xFF08' && raw[i + 1] == L'&' && is_mnemonic_key(raw[i + 2]) && raw[i + 3] == L'\xFF09'))) {
            i += 4;
            continue;
        }
        if (raw[i] == L'&') {
            if (i + 1 < raw.size() && raw[i + 1] == L'&') {
                out.push_back(L'&');
                i += 2;
                continue;
            }
            if (i + 1 < raw.size() && raw[i + 1] != L' ' && raw[i + 1] != L'\t') {
                ++i;
                continue;
            }
        }
        out.push_back(raw[i]);
        ++i;
    }
    return out;
}

inline bool IsInlineColonCaption(std::wstring_view display_text) noexcept {
    while (!display_text.empty() && (display_text.back() == L' ' || display_text.back() == L'\t')) {
        display_text.remove_suffix(1);
    }
    while (!display_text.empty() && (display_text.front() == L' ' || display_text.front() == L'\t')) {
        display_text.remove_prefix(1);
    }
    if (display_text.size() < 2 || display_text.size() > 24) return false;
    return display_text.back() == L':' || display_text.back() == L'\xFF1A';
}

struct PickerCustomSections {
    float prominent_top = 0;
    float content_height = 0;
};
inline PickerCustomSections LayoutPickerCustomSections(float normal_height, float prominent_height, float viewport_height) {
    const float gap = normal_height > 0 && prominent_height > 0 ? 12.0f : 0.0f;
    const float top = prominent_height > 0 ? std::max(4.0f + normal_height + gap, viewport_height - 4.0f - prominent_height) : 0.0f;
    return {top, prominent_height > 0 ? top + prominent_height + 4.0f : normal_height + 8.0f};
}
inline float PickerProminentLeft(float width) { return std::max(16.0f, width - 28.0f - 360.0f); }

struct PickerFlowBox {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    bool visible = false;
    bool vertical_separator = false;
    bool inline_caption = false;
    bool segmented_radio = false;
};

struct PickerCustomFlowItem {
    uint32_t kind = 0;
    uint32_t id = 0;
    bool visible = false;
    bool prominent = false;
    bool has_parent = false;
    uint32_t parent_id = 0;
    bool has_caption = false;
    bool inline_colon_label = false;
    float natural_w = 0.0f;
    std::vector<float> radio_item_widths;   // negative => hidden radio item
    std::vector<float> segment_item_widths; // negative => hidden segment item (optional; defaults from radio_item_widths)
};

struct PickerCustomFlowLayout {
    std::vector<PickerFlowBox> boxes;
    std::vector<std::vector<PickerFlowBox>> radio_boxes;
    float content_height = 0.0f;
    float viewport_height = 0.0f;
    bool has_scrollbar = false;
};

namespace detail {
inline PickerCustomFlowLayout SolveCustomFlow(float width_dip, float max_viewport_dip, float reserve_right,
                                              const std::vector<PickerCustomFlowItem>& items) {
    constexpr float kPadX = 16.0f;
    constexpr float kPadTop = 8.0f;
    constexpr float kPadBottom = 8.0f;
    constexpr float kItemGapX = 14.0f;
    constexpr float kLabelGapX = 6.0f;
    constexpr float kRowGapY = 6.0f;
    constexpr float kHeadingH = 20.0f;
    constexpr float kControlH = 30.0f;
    constexpr float kRadioH = 26.0f;
    constexpr float kRadioGapX = 10.0f;
    constexpr float kSepH = 11.0f;
    constexpr float kVertSepW = 13.0f;

    PickerCustomFlowLayout out;
    out.boxes.resize(items.size());
    out.radio_boxes.resize(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        out.radio_boxes[i].resize(std::max(items[i].radio_item_widths.size(), items[i].segment_item_widths.size()));
    }

    const float avail_w = std::max(120.0f, width_dip - kPadX * 2.0f - reserve_right);
    const float right_edge = kPadX + avail_w;

    auto segment_width_for = [&](const PickerCustomFlowItem& item, size_t j) -> float {
        if (j < item.segment_item_widths.size()) return item.segment_item_widths[j];
        if (j >= item.radio_item_widths.size() || item.radio_item_widths[j] < 0.0f) return -1.0f;
        return std::max(44.0f, item.radio_item_widths[j] - 8.0f);
    };

    auto segmented_total_width = [&](const PickerCustomFlowItem& item, size_t& visible_count) -> float {
        visible_count = 0;
        float w = 0.0f;
        const size_t count = std::max(item.radio_item_widths.size(), item.segment_item_widths.size());
        for (size_t j = 0; j < count; ++j) {
            const float sw = segment_width_for(item, j);
            if (sw < 0.0f) continue;
            ++visible_count;
            w += std::max(44.0f, sw);
        }
        return w;
    };

    auto can_use_segmented = [&](const PickerCustomFlowItem& item, float max_w) -> bool {
        size_t visible_count = 0;
        const float total_w = segmented_total_width(item, visible_count);
        return visible_count >= 2 && total_w > 0.0f && total_w <= max_w + 0.5f;
    };

    auto radio_single_line_width = [&](const PickerCustomFlowItem& item) -> float {
        if (can_use_segmented(item, avail_w)) {
            size_t visible_count = 0;
            return segmented_total_width(item, visible_count);
        }
        float w = 0.0f;
        for (float rw : item.radio_item_widths) {
            if (rw < 0.0f) continue;
            if (w > 0.0f) w += kRadioGapX;
            w += std::min(rw, avail_w);
        }
        return w;
    };

    auto next_visible_index = [&](size_t start, bool prominent) -> size_t {
        for (size_t k = start; k < items.size(); ++k) {
            if (!items[k].visible || items[k].prominent != prominent) continue;
            if ((items[k].kind == 1 || items[k].kind == kPickerCustomText) && !items[k].has_caption) continue;
            return k;
        }
        return items.size();
    };

    auto can_pair_inline = [&](size_t idx, size_t next_idx) -> bool {
        if (next_idx >= items.size()) return false;
        const auto& cur = items[idx];
        const auto& nxt = items[next_idx];
        if (!cur.inline_colon_label || !PickerCustomInteractive(nxt.kind)) return false;
        if (cur.kind == 1) return nxt.has_parent && nxt.parent_id == cur.id;
        if (cur.kind == kPickerCustomText) {
            return nxt.has_parent == cur.has_parent && (!nxt.has_parent || nxt.parent_id == cur.parent_id);
        }
        return false;
    };

    auto unit_width = [&](size_t idx, bool prominent) -> float {
        const auto& item = items[idx];
        if ((item.kind == 1 || item.kind == kPickerCustomText) && !item.inline_colon_label) return avail_w;
        if (item.inline_colon_label) {
            const size_t nxt = next_visible_index(idx + 1, prominent);
            if (can_pair_inline(idx, nxt)) {
                const float lw = std::min(item.natural_w, avail_w * 0.35f);
                const float cw = items[nxt].kind == 6 ? std::min(radio_single_line_width(items[nxt]), avail_w)
                                                      : std::min(items[nxt].natural_w, avail_w);
                return std::min(avail_w, lw + kLabelGapX + cw);
            }
        }
        if (item.kind == 6) return std::min(radio_single_line_width(item), avail_w);
        return std::min(std::max(32.0f, item.natural_w), avail_w);
    };

    float y = kPadTop;
    bool any_emitted = false;

    auto layout_pass = [&](bool prominent) {
        float x = kPadX;
        float row_h = 0.0f;
        bool row_has_items = false;
        bool just_emitted_sep = false;
        bool paired_next = false;
        size_t pending_sep = items.size();

        auto flush_row = [&]() {
            if (!row_has_items) return;
            y += row_h + kRowGapY;
            x = kPadX;
            row_h = 0.0f;
            row_has_items = false;
        };

        auto resolve_pending_sep = [&](bool force_horizontal, float next_w) {
            if (pending_sep >= items.size()) return;
            const size_t sep_idx = pending_sep;
            pending_sep = items.size();
            if (!force_horizontal && row_has_items &&
                x + kVertSepW + kItemGapX * 0.5f + next_w <= right_edge + 0.5f) {
                const float sx = x - kItemGapX * 0.5f;
                const float sep_h = std::max(kControlH, row_h);
                out.boxes[sep_idx] = {sx, y, sx + kVertSepW, y + sep_h, true, true, false};
                x = sx + kVertSepW + kItemGapX * 0.5f;
                any_emitted = true;
            } else {
                flush_row();
                if (any_emitted) {
                    out.boxes[sep_idx] = {kPadX, y, right_edge, y + kSepH, true, false, false};
                    y += kSepH + 2.0f;
                    just_emitted_sep = true;
                }
            }
        };

        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            if (!item.visible || item.prominent != prominent) continue;
            if ((item.kind == 1 || item.kind == kPickerCustomText) && !item.has_caption) continue;

            if (item.kind == kPickerCustomSeparator) {
                if (any_emitted || row_has_items) pending_sep = i;
                continue;
            }

            if (item.kind == 1 || item.kind == kPickerCustomText) {
                const size_t nxt = next_visible_index(i + 1, prominent);
                if (can_pair_inline(i, nxt)) {
                    const float pw = unit_width(i, prominent);
                    resolve_pending_sep(false, pw);
                    if (row_has_items && x + pw > right_edge + 0.5f) flush_row();
                    const float lw = std::min(item.natural_w, std::max(32.0f, right_edge - x) * 0.45f);
                    const bool nxt_segmented = items[nxt].kind == 6 &&
                        can_use_segmented(items[nxt], std::max(0.0f, right_edge - (x + lw + kLabelGapX)));
                    const float lh = (items[nxt].kind == 6 && !nxt_segmented) ? kRadioH : kControlH;
                    out.boxes[i] = {x, y, x + lw, y + lh, true, false, true, false};
                    x += lw + kLabelGapX;
                    row_h = std::max(row_h, lh);
                    row_has_items = true;
                    paired_next = true;
                    any_emitted = true;
                    just_emitted_sep = false;
                    continue;
                }
                // Standalone section heading or descriptive text.
                resolve_pending_sep(true, avail_w);
                flush_row();
                if (any_emitted && !just_emitted_sep) y += 2.0f;
                out.boxes[i] = {kPadX, y, right_edge, y + kHeadingH, true, false, false, false};
                y += kHeadingH + 4.0f;
                any_emitted = true;
                just_emitted_sep = false;
                paired_next = false;
                continue;
            }

            if (item.kind == 6) {
                const bool use_segmented = paired_next
                    ? can_use_segmented(item, std::max(0.0f, right_edge - x))
                    : can_use_segmented(item, avail_w);
                if (use_segmented) {
                    size_t visible_count = 0;
                    const float total_sw = segmented_total_width(item, visible_count);
                    if (!paired_next) {
                        resolve_pending_sep(false, total_sw);
                        if (row_has_items && x + total_sw > right_edge + 0.5f) flush_row();
                    }
                    const float start_x = x;
                    float rx = start_x;
                    const size_t count = out.radio_boxes[i].size();
                    for (size_t j = 0; j < count; ++j) {
                        const float sw = segment_width_for(item, j);
                        if (sw < 0.0f) continue;
                        const float w = std::max(44.0f, sw);
                        out.radio_boxes[i][j] = {rx, y, std::min(right_edge, rx + w), y + kControlH, true, false, false, true};
                        rx = out.radio_boxes[i][j].right;
                    }
                    if (rx > start_x) {
                        out.boxes[i] = {start_x, y, rx, y + kControlH, true, false, false, true};
                        x = rx + kItemGapX;
                        row_h = std::max(row_h, kControlH);
                        row_has_items = true;
                        any_emitted = true;
                        just_emitted_sep = false;
                    }
                    paired_next = false;
                    continue;
                }
                const float total_rw = radio_single_line_width(item);
                if (!paired_next) {
                    resolve_pending_sep(false, std::min(total_rw, avail_w));
                    if (row_has_items && x + total_rw > right_edge + 0.5f) flush_row();
                }
                const float start_x = x;
                float rx = start_x;
                float ry = y;
                float max_right = start_x;
                bool any_radio = false;
                for (size_t j = 0; j < item.radio_item_widths.size(); ++j) {
                    const float rw = item.radio_item_widths[j];
                    if (rw < 0.0f) continue;
                    const float w = std::min(std::max(44.0f, rw), std::max(44.0f, right_edge - start_x));
                    if (any_radio && rx + w > right_edge + 0.5f) {
                        rx = start_x;
                        ry += kRadioH + 2.0f;
                    }
                    out.radio_boxes[i][j] = {rx, ry, std::min(right_edge, rx + w), ry + kRadioH, true, false, false, false};
                    max_right = std::max(max_right, out.radio_boxes[i][j].right);
                    rx += w + kRadioGapX;
                    any_radio = true;
                }
                if (any_radio) {
                    const float total_h = (ry + kRadioH) - y;
                    out.boxes[i] = {start_x, y, max_right, ry + kRadioH, true, false, false, false};
                    if (ry > y) {
                        // Multi-line radio group consumes the rest of its vertical span.
                        y = ry + kRadioH + kRowGapY;
                        x = kPadX;
                        row_h = 0.0f;
                        row_has_items = false;
                    } else {
                        x = max_right + kItemGapX;
                        row_h = std::max(row_h, total_h);
                        row_has_items = true;
                    }
                    any_emitted = true;
                    just_emitted_sep = false;
                }
                paired_next = false;
                continue;
            }

            // PushButton (2), ComboBox (3), Menu (4), CheckButton (7), EditBox (8).
            float cw = std::min(std::max(32.0f, item.natural_w), avail_w);
            if (!paired_next) {
                resolve_pending_sep(false, cw);
                if (row_has_items && x + cw > right_edge + 0.5f) flush_row();
            } else {
                cw = std::min(cw, std::max(48.0f, right_edge - x));
            }
            out.boxes[i] = {x, y, x + cw, y + kControlH, true, false, false};
            x += cw + kItemGapX;
            row_h = std::max(row_h, kControlH);
            row_has_items = true;
            any_emitted = true;
            just_emitted_sep = false;
            paired_next = false;
        }
        flush_row();
    };

    layout_pass(false);

    bool has_prominent = false;
    for (const auto& item : items) {
        if (item.visible && item.prominent) { has_prominent = true; break; }
    }
    if (has_prominent) {
        if (any_emitted) y += 2.0f;
        const float prom_start_y = y;
        layout_pass(true);
        // Right-align single-row prominent controls when they fit cleanly in the right action area.
        float min_l = right_edge, max_r = kPadX;
        bool multi_row = false;
        float first_top = -1.0f;
        for (size_t i = 0; i < items.size(); ++i) {
            if (!items[i].prominent || !out.boxes[i].visible) continue;
            if (first_top < 0.0f) first_top = out.boxes[i].top;
            else if (std::abs(out.boxes[i].top - first_top) > 1.0f) multi_row = true;
            min_l = std::min(min_l, out.boxes[i].left);
            max_r = std::max(max_r, out.boxes[i].right);
        }
        if (!multi_row && max_r > min_l && max_r < right_edge) {
            const float shift = right_edge - max_r;
            for (size_t i = 0; i < items.size(); ++i) {
                if (!items[i].prominent || !out.boxes[i].visible) continue;
                out.boxes[i].left += shift;
                out.boxes[i].right += shift;
                for (auto& rb : out.radio_boxes[i]) {
                    if (rb.visible) { rb.left += shift; rb.right += shift; }
                }
            }
        }
        (void)prom_start_y;
    }

    if (!any_emitted) {
        out.content_height = 0.0f;
        out.viewport_height = 0.0f;
        return out;
    }
    // Remove trailing kRowGapY from the final flushed row and add bottom padding.
    out.content_height = std::max(kControlH + kPadTop + kPadBottom, y - kRowGapY + kPadBottom);
    out.viewport_height = std::min(out.content_height, std::max(48.0f, max_viewport_dip));
    out.has_scrollbar = out.content_height > out.viewport_height + 0.5f;
    return out;
}
} // namespace detail

inline PickerCustomFlowLayout LayoutPickerCustomFlow(float width_dip, float max_viewport_dip,
                                                     const std::vector<PickerCustomFlowItem>& items) {
    auto layout = detail::SolveCustomFlow(width_dip, max_viewport_dip, 0.0f, items);
    if (layout.has_scrollbar) {
        layout = detail::SolveCustomFlow(width_dip, max_viewport_dip, 14.0f, items);
        layout.has_scrollbar = layout.content_height > layout.viewport_height + 0.5f;
    }
    return layout;
}
} // namespace pulse::ui
