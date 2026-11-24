#include "extension_tree_layout.h"
#include "extension_tree_extra_controls.h"
#include <algorithm>
#include <numeric>
namespace pulse::ui {
using extensions::UiNode;
using extensions::UiNodeType;
namespace {
D2D1_RECT_F Intersect(D2D1_RECT_F a, D2D1_RECT_F b) {
    return {std::max(a.left, b.left), std::max(a.top, b.top),
            std::max(std::max(a.left, b.left), std::min(a.right, b.right)),
            std::max(std::max(a.top, b.top), std::min(a.bottom, b.bottom))};
}
bool Contains(D2D1_RECT_F rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}
constexpr float kCardPadX = 16, kCardPadY = 12;
bool Card(const UiNode &node) { return node.type == UiNodeType::Group && node.bordered; }
// Inner inset (DIP): explicit padding wins; a bordered group keeps its card insets by default.
float PadX(const UiNode &node) { return node.padding >= 0 ? static_cast<float>(node.padding) : Card(node) ? kCardPadX : 0.0f; }
float PadY(const UiNode &node) { return node.padding >= 0 ? static_cast<float>(node.padding) : Card(node) ? kCardPadY : 0.0f; }
bool Container(UiNodeType type) {
    return type == UiNodeType::Stack || type == UiNodeType::Grid || type == UiNodeType::Group ||
           type == UiNodeType::Tabs || type == UiNodeType::Tab || type == UiNodeType::ScrollView ||
           type == UiNodeType::SplitPane || type == UiNodeType::SidebarSection ||
           type == UiNodeType::StagingTray || type == UiNodeType::SettingsRow;
}
void Distribute(std::vector<float> &sizes, const std::vector<float> &limits, const std::vector<int> &weights,
                float available) {
    float remaining = available - std::accumulate(sizes.begin(), sizes.end(), 0.0f);
    while (remaining > 0.01f) {
        int weight = 0;
        for (size_t i = 0; i < sizes.size(); ++i)
            if (sizes[i] + 0.01f < limits[i])
                weight += weights[i];
        if (!weight)
            break;
        float distributed = 0;
        for (size_t i = 0; i < sizes.size(); ++i) {
            if (!weights[i] || sizes[i] + 0.01f >= limits[i])
                continue;
            float grow = std::min(limits[i] - sizes[i], remaining * weights[i] / weight);
            sizes[i] += grow;
            distributed += grow;
        }
        if (distributed <= 0.01f)
            break;
        remaining -= distributed;
    }
}
} // namespace
float ExtensionTreeLayout::ConstrainWidth(const UiNode &node, float width) const {
    width = std::max(1.0f, width);
    return node.max_width ? std::min(width, node.max_width * scale_) : width;
}
float ExtensionTreeLayout::ConstrainHeight(const UiNode &node, float height) const {
    height = std::max(node.min_height * scale_, height);
    return node.max_height ? std::min(height, node.max_height * scale_) : height;
}
bool ExtensionTreeLayout::ChildVisible(const UiNode &node, size_t index,
                                       const ExtensionLayoutCallbacks &callbacks) const {
    return ((node.type != UiNodeType::SidebarSection && node.type != UiNodeType::StagingTray) ||
            callbacks.selected(node) != 0) &&
           (node.type != UiNodeType::Tabs || static_cast<int>(index) == callbacks.selected(node)) &&
           callbacks.visible(node.children[index]);
}
ExtensionTreeLayout::Flow ExtensionTreeLayout::ChildFlow(const UiNode &node, float width,
                                                         const ExtensionLayoutCallbacks &callbacks) const {
    Flow flow;
    const float gap = node.gap * scale_;
    const auto intrinsic = [&](const UiNode &child) {
        return !child.min_width && !child.max_width && !child.flex && callbacks.intrinsic_width
                   ? callbacks.intrinsic_width(child)
                   : 0.0f;
    };
    bool constrained = node.type == UiNodeType::Stack && node.orientation == "horizontal" &&
                       std::any_of(node.children.begin(), node.children.end(), [&](const auto &child) {
                           return child.min_width || child.max_width || child.flex || intrinsic(child) > 0;
                       });
    if (constrained) {
        std::vector<float> limits;
        std::vector<int> weights;
        size_t count = 0;
        for (size_t index = 0; index < node.children.size(); ++index) {
            const auto &child = node.children[index];
            bool visible = ChildVisible(node, index, callbacks);
            const float natural = intrinsic(child);
            float limit = natural > 0 ? natural : child.max_width ? child.max_width * scale_ : 1.0e9f;
            float base = natural > 0 ? natural : std::min(limit, (child.min_width ? child.min_width : 160) * scale_);
            flow.widths.push_back(visible ? base : 0);
            limits.push_back(visible ? limit : 0);
            weights.push_back(visible ? child.flex : 0);
            if (visible)
                ++count;
        }
        float gaps = count ? gap * static_cast<float>(count - 1) : 0;
        if (std::accumulate(flow.widths.begin(), flow.widths.end(), 0.0f) + gaps <= width) {
            flow.columns = std::max(1, static_cast<int>(count));
            Distribute(flow.widths, limits, weights, width - gaps);
            return flow;
        }
        // A narrow viewport wins over a minimum width: keep every child reachable in one column.
        flow.widths.assign(node.children.size(), width);
        return flow;
    }
    int requested = node.type == UiNodeType::Grid      ? node.columns
                    : node.orientation == "horizontal" ? static_cast<int>(node.children.size())
                                                       : 1;
    flow.columns = std::max(1, std::min(requested, static_cast<int>(width / std::max(1.0f, 160 * scale_))));
    float minimum = 0;
    for (size_t index = 0; index < node.children.size(); ++index)
        if (ChildVisible(node, index, callbacks))
            minimum = std::max(minimum, node.children[index].min_width * scale_);
    while (flow.columns > 1 && (width - gap * (flow.columns - 1)) / flow.columns < minimum)
        --flow.columns;
    const float cell = std::max(1.0f, (width - gap * (flow.columns - 1)) / flow.columns);
    flow.widths.assign(node.children.size(), cell);
    return flow;
}
float ExtensionTreeLayout::Measure(const UiNode &node, float width, const ExtensionLayoutCallbacks &callbacks) const {
    if (!callbacks.visible(node))
        return 0;
    width = ConstrainWidth(node, width);
    if (!Container(node.type)) {
        float extra = callbacks.feedback_height ? callbacks.feedback_height(node, width) : 0;
        float natural = std::max(0.0f, callbacks.measure(node, width) - extra);
        return std::max(natural, ConstrainHeight(node, natural)) + extra;
    }
    if (node.type == UiNodeType::ScrollView)
        return ConstrainHeight(node, std::max(32, node.min_height ? node.min_height : 240) * scale_);
    if (node.type == UiNodeType::SplitPane)
        return ConstrainHeight(node, std::max(240, node.min_height) * scale_);
    if (node.type == UiNodeType::SettingsRow) {
        const bool bordered = node.bordered;
        const float pad_x = bordered ? 16.0f * scale_ : 0.0f;
        const float pad_y = bordered ? 12.0f * scale_ : 4.0f * scale_;
        const float inner_w = std::max(1.0f, width - 2.0f * pad_x);
        const float row_gap = node.gap * scale_;
        std::vector<float> child_widths;
        size_t vis_count = 0;
        for (size_t i = 0; i < node.children.size(); ++i) vis_count += ChildVisible(node, i, callbacks) ? 1 : 0;
        const float controls_w = ExtensionSettingsRowInlineWidths(node, inner_w, row_gap, scale_,
            [&](size_t i) { return ChildVisible(node, i, callbacks); }, child_widths);
        if (controls_w > 0.0f) {
            const float left_w = std::max(1.0f, inner_w - controls_w - 16.0f * scale_);
            float row_h = callbacks.measure(node, left_w);
            for (size_t i = 0; i < node.children.size(); ++i) {
                if (child_widths[i] > 0.0f)
                    row_h = std::max(row_h, Measure(node.children[i], child_widths[i], callbacks));
            }
            return ConstrainHeight(node, row_h + 2.0f * pad_y);
        }
        const float left_h = callbacks.measure(node, inner_w);
        float stacked_h = 0.0f;
        if (vis_count > 0) {
            auto flow = ChildFlow(node, inner_w, callbacks);
            float r = 0.0f;
            int col = 0;
            for (size_t i = 0; i < node.children.size(); ++i) {
                if (!ChildVisible(node, i, callbacks)) continue;
                r = std::max(r, Measure(node.children[i], flow.widths[i], callbacks));
                if (++col == flow.columns) {
                    stacked_h += r + row_gap;
                    r = 0.0f;
                    col = 0;
                }
            }
            if (col) stacked_h += r + row_gap;
            if (stacked_h > 0.0f) stacked_h = std::max(0.0f, stacked_h - row_gap) + 8.0f * scale_;
        }
        return ConstrainHeight(node, left_h + stacked_h + 2.0f * pad_y);
    }
    const float gap = node.gap * scale_;
    const bool card = Card(node);
    const float pad_x = PadX(node) * scale_, pad_y = PadY(node) * scale_;
    width = std::max(1.0f, width - 2 * pad_x);
    float used = 0;
    if ((node.type == UiNodeType::Group && (!card || !node.text.empty())) || node.type == UiNodeType::Tabs ||
        node.type == UiNodeType::SidebarSection || node.type == UiNodeType::StagingTray)
        used = callbacks.measure(node, width) + gap;
    auto flow = ChildFlow(node, width, callbacks);
    float row = 0;
    int column = 0;
    for (size_t index = 0; index < node.children.size(); ++index) {
        if (!ChildVisible(node, index, callbacks))
            continue;
        row = std::max(row, Measure(node.children[index], flow.widths[index], callbacks));
        if (++column == flow.columns) {
            used += row + gap;
            row = 0;
            column = 0;
        }
    }
    if (column)
        used += row + gap;
    return ConstrainHeight(node, std::max(0.0f, used - gap) + 2 * pad_y);
}
void ExtensionTreeLayout::Reset() {
    cached_root_ = nullptr;
    measure_cache_.clear(); feedback_cache_.clear();
    visible_cache_.clear(); enabled_cache_.clear(); selected_cache_.clear();
    viewports_.clear();
    placements_.clear();
}
float ExtensionTreeLayout::Arrange(const UiNode &root, D2D1_RECT_F viewport, float outer_offset, float scale,
                                   const ExtensionLayoutCallbacks &callbacks, bool scroll_only, float bleed_x) {
    const float width = viewport.right - viewport.left, height = viewport.bottom - viewport.top;
    if (!scroll_only || cached_root_ != &root || scale_ != scale || cached_width_ != width || cached_height_ != height) {
        measure_cache_.clear(); feedback_cache_.clear();
        visible_cache_.clear(); enabled_cache_.clear(); selected_cache_.clear();
    }
    cached_root_ = &root; cached_width_ = width; cached_height_ = height;
    auto cached = callbacks;
    cached.measure = [&](const UiNode& node, float w) {
        const auto key = std::make_pair(&node, w);
        auto at = measure_cache_.find(key);
        if (at != measure_cache_.end()) return at->second;
        return measure_cache_[key] = callbacks.measure(node, w);
    };
    cached.feedback_height = [&](const UiNode& node, float w) {
        const auto key = std::make_pair(&node, w);
        auto at = feedback_cache_.find(key);
        if (at != feedback_cache_.end()) return at->second;
        return feedback_cache_[key] = callbacks.feedback_height ? callbacks.feedback_height(node, w) : 0;
    };
    cached.visible = [&](const UiNode& node) {
        auto [at, inserted] = visible_cache_.try_emplace(&node);
        if (inserted) at->second = callbacks.visible(node);
        return at->second;
    };
    cached.enabled = [&](const UiNode& node) {
        auto [at, inserted] = enabled_cache_.try_emplace(&node);
        if (inserted) at->second = callbacks.enabled(node);
        return at->second;
    };
    cached.selected = [&](const UiNode& node) {
        auto [at, inserted] = selected_cache_.try_emplace(&node);
        if (inserted) at->second = callbacks.selected(node);
        return at->second;
    };
    scale_ = scale;
    placements_.clear();
    for (auto &entry : viewports_)
        entry.second.visible = false;
    std::vector<const UiNode *> ancestors;
    // Controls flush with the content edge (a checkbox's stroke, any focus ring) may
    // paint into the host's horizontal content inset; nothing scrolls sideways there.
    const D2D1_RECT_F clip{viewport.left - bleed_x, viewport.top, viewport.right + bleed_x, viewport.bottom};
    return Place(root, viewport.left, viewport.top - outer_offset, viewport.right - viewport.left,
                 viewport.bottom - viewport.top, clip, true, true, ancestors, cached, true);
}
float ExtensionTreeLayout::Place(const UiNode &node, float x, float y, float width, float root_height, D2D1_RECT_F clip,
                                 bool visible, bool enabled, std::vector<const UiNode *> &ancestors,
                                 const ExtensionLayoutCallbacks &callbacks, bool root, float allocated_height) {
    visible = visible && callbacks.visible(node);
    enabled = enabled && callbacks.enabled(node);
    width = ConstrainWidth(node, width);
    const float gap = node.gap * scale_;
    if (!Container(node.type)) {
        float extra = callbacks.feedback_height ? callbacks.feedback_height(node, width) : 0;
        float natural = std::max(0.0f, callbacks.measure(node, width) - extra);
        // A size limit constrains growth, never the host control's minimum readable geometry.
        float height = std::max(natural, ConstrainHeight(node, std::max(allocated_height - extra, natural))) + extra;
        D2D1_RECT_F bounds{x, y, x + width, y + height};
        placements_[&node] = {bounds, ancestors};
        callbacks.place(node, bounds, clip, visible, enabled);
        return visible ? height : 0;
    }
    auto own_ancestors = ancestors;
    if (node.type == UiNodeType::SplitPane) {
        float height = ConstrainHeight(node, std::max(allocated_height, std::max(240, node.min_height) * scale_));
        D2D1_RECT_F bounds{x, y, x + width, y + height};
        callbacks.place(node, bounds, clip, visible, enabled);
        placements_[&node] = {bounds, ancestors};
        bool horizontal = node.orientation == "horizontal";
        float gap_size = 8 * scale_;
        float extent = std::max(0.0f, (horizontal ? width : height) - gap_size);
        float first = extent * std::clamp(callbacks.selected(node), 0, 100) / 100;
        if (node.children.size() == 2) {
            auto first_bounds =
                horizontal ? D2D1::RectF(x, y, x + first, y + height) : D2D1::RectF(x, y, x + width, y + first);
            auto second_bounds = horizontal ? D2D1::RectF(x + first + gap_size, y, x + width, y + height)
                                            : D2D1::RectF(x, y + first + gap_size, x + width, y + height);
            Place(node.children[0], first_bounds.left, first_bounds.top, first_bounds.right - first_bounds.left,
                  first_bounds.bottom - first_bounds.top, Intersect(clip, first_bounds), visible, enabled, ancestors,
                  callbacks, node.children[0].type == UiNodeType::ScrollView, first_bounds.bottom - first_bounds.top);
            Place(node.children[1], second_bounds.left, second_bounds.top, second_bounds.right - second_bounds.left,
                  second_bounds.bottom - second_bounds.top, Intersect(clip, second_bounds), visible, enabled, ancestors,
                  callbacks, node.children[1].type == UiNodeType::ScrollView, second_bounds.bottom - second_bounds.top);
        }
        return visible ? height : 0;
    }
    if (node.type == UiNodeType::SettingsRow) {
        const bool bordered = node.bordered;
        const float pad_x = bordered ? 16.0f * scale_ : 0.0f;
        const float pad_y = bordered ? 12.0f * scale_ : 4.0f * scale_;
        const float inner_w = std::max(1.0f, width - 2.0f * pad_x);
        std::vector<float> child_widths;
        size_t vis_count = 0;
        for (size_t i = 0; i < node.children.size(); ++i) vis_count += ChildVisible(node, i, callbacks) ? 1 : 0;
        const float controls_w = ExtensionSettingsRowInlineWidths(node, inner_w, gap, scale_,
            [&](size_t i) { return ChildVisible(node, i, callbacks); }, child_widths);
        float total_h = 0.0f;
        if (controls_w > 0.0f) {
            const float left_w = std::max(1.0f, inner_w - controls_w - 16.0f * scale_);
            std::vector<float> child_heights(node.children.size(), 0.0f);
            float row_h = callbacks.measure(node, left_w);
            for (size_t i = 0; i < node.children.size(); ++i) {
                if (!ChildVisible(node, i, callbacks)) continue;
                child_heights[i] = Measure(node.children[i], child_widths[i], callbacks);
                row_h = std::max(row_h, child_heights[i]);
            }
            float cx = x + width - pad_x - controls_w;
            const float cy_base = y + pad_y;
            for (size_t i = 0; i < node.children.size(); ++i) {
                const bool child_visible = visible && ChildVisible(node, i, callbacks);
                if (!child_visible) {
                    Place(node.children[i], cx, cy_base, child_widths[i], root_height, clip, false, enabled, ancestors,
                          callbacks, false, 0.0f);
                    continue;
                }
                const float offset_y = std::max(0.0f, (row_h - child_heights[i]) * 0.5f);
                Place(node.children[i], cx, cy_base + offset_y, child_widths[i], root_height, clip, true, enabled,
                      ancestors, callbacks, false, child_heights[i]);
                cx += child_widths[i] + gap;
            }
            total_h = ConstrainHeight(node, std::max(allocated_height, row_h + 2.0f * pad_y));
        } else {
            const float left_h = callbacks.measure(node, inner_w);
            auto flow = ChildFlow(node, inner_w, callbacks);
            float used = left_h + (vis_count > 0 ? 8.0f * scale_ : 0.0f);
            float row = 0.0f, left = x + pad_x;
            int column = 0;
            for (size_t i = 0; i < node.children.size(); ++i) {
                const bool child_visible = visible && ChildVisible(node, i, callbacks);
                const float ch = Place(node.children[i], left, y + pad_y + used, flow.widths[i], root_height, clip,
                                       child_visible, enabled, ancestors, callbacks, false, 0.0f);
                if (!child_visible) continue;
                row = std::max(row, ch);
                left += flow.widths[i] + gap;
                if (++column == flow.columns) {
                    used += row + gap;
                    row = 0.0f;
                    column = 0;
                    left = x + pad_x;
                }
            }
            if (column) used += row + gap;
            const float content_h = vis_count > 0 ? std::max(left_h, used - gap) : left_h;
            total_h = ConstrainHeight(node, std::max(allocated_height, content_h + 2.0f * pad_y));
        }
        const D2D1_RECT_F bounds{x, y, x + width, y + total_h};
        placements_[&node] = {bounds, own_ancestors};
        callbacks.place(node, bounds, clip, visible, enabled);
        return visible ? total_h : 0.0f;
    }
    // A bordered group is a card: its heading and children sit inside the padding; the card's own
    // bounds (placed last) cover the whole card so it can paint the surface under them.
    const bool card = Card(node);
    const float card_x = x, card_y = y, card_width = width;
    const float pad_x = PadX(node) * scale_, pad_y = PadY(node) * scale_;
    const bool padded = pad_x > 0 || pad_y > 0;
    if (padded) {
        x += pad_x;
        y += pad_y;
        width = std::max(1.0f, width - 2 * pad_x);
    }
    float heading = 0;
    if ((node.type == UiNodeType::Group && (!card || !node.text.empty())) || node.type == UiNodeType::Tabs ||
        node.type == UiNodeType::SidebarSection || node.type == UiNodeType::StagingTray) {
        float height = callbacks.measure(node, width);
        D2D1_RECT_F bounds{x, y, x + width, y + height};
        placements_[&node] = {bounds, ancestors};
        callbacks.place(node, bounds, clip, visible, enabled);
        heading = height + gap;
    }
    Viewport *scrolling = nullptr;
    float viewport_height = 0;
    auto child_clip = clip;
    if (node.type == UiNodeType::ScrollView) {
        viewport_height =
            root ? std::max(0.0f, root_height)
                 : ConstrainHeight(node, std::max(allocated_height,
                                                  std::max(32, node.min_height ? node.min_height : 240) * scale_));
        if (root && node.max_height)
            viewport_height = std::min(viewport_height, node.max_height * scale_);
        auto &viewport = viewports_[&node];
        viewport.node = &node;
        viewport.bounds = {x, y, x + width, y + viewport_height};
        viewport.clip = Intersect(clip, root ? D2D1_RECT_F{clip.left, viewport.bounds.top, clip.right, viewport.bounds.bottom}
                                             : viewport.bounds);
        viewport.visible = visible;
        viewport.enabled = enabled;
        viewport.depth = ancestors.size();
        scrolling = &viewport;
        child_clip = viewport.clip;
        ancestors.push_back(&node);
    }
    const float content_width = std::max(1.0f, width - (scrolling ? 14 * scale_ : 0));
    auto flow = ChildFlow(node, content_width, callbacks);
    std::vector<float> heights(node.children.size(), 0);
    if (node.type == UiNodeType::Stack && node.orientation == "vertical") {
        std::vector<float> limits(node.children.size(), 0);
        std::vector<int> weights(node.children.size(), 0);
        size_t count = 0;
        for (size_t index = 0; index < node.children.size(); ++index) {
            if (!ChildVisible(node, index, callbacks))
                continue;
            const auto &child = node.children[index];
            heights[index] = Measure(child, flow.widths[index], callbacks);
            limits[index] =
                child.max_height
                    ? child.max_height * scale_ +
                          (callbacks.feedback_height ? callbacks.feedback_height(child, flow.widths[index]) : 0)
                    : 1.0e9f;
            weights[index] = child.flex;
            ++count;
        }
        float available = (allocated_height > 0 ? allocated_height : root ? root_height : node.min_height * scale_) -
                          2 * pad_y;
        Distribute(heights, limits, weights, available - (count ? gap * static_cast<float>(count - 1) : 0));
    }
    // A horizontal stack with alignment "center" centres each child on the row (a title beside a switch).
    std::vector<float> offsets(node.children.size(), 0);
    if (node.type == UiNodeType::Stack && node.orientation == "horizontal" && node.alignment == "center") {
        float tallest = 0;
        for (size_t index = 0; index < node.children.size(); ++index)
            if (ChildVisible(node, index, callbacks)) {
                offsets[index] = Measure(node.children[index], flow.widths[index], callbacks);
                tallest = std::max(tallest, offsets[index]);
            }
        for (auto &offset : offsets)
            offset = std::max(0.0f, (tallest - offset) / 2);
    }
    auto place_children = [&]() {
        float used = heading, row = 0, left = x;
        int column = 0;
        for (size_t index = 0; index < node.children.size(); ++index) {
            bool child_visible = visible && ChildVisible(node, index, callbacks);
            float height =
                Place(node.children[index], left, y + used + offsets[index] - (scrolling ? scrolling->offset : 0),
                      flow.widths[index], root_height, child_clip, child_visible, enabled, ancestors, callbacks, false,
                      heights[index]) +
                (child_visible ? offsets[index] : 0);
            if (!child_visible)
                continue;
            row = std::max(row, height);
            left += flow.widths[index] + gap;
            if (++column == flow.columns) {
                used += row + gap;
                row = 0;
                column = 0;
                left = x;
            }
        }
        if (column)
            used += row + gap;
        return std::max(0.0f, used - gap);
    };
    float content = place_children();
    if (scrolling) {
        scrolling->content = content;
        float clamped = std::clamp(scrolling->offset, 0.0f, std::max(0.0f, content - viewport_height));
        if (clamped != scrolling->offset) {
            scrolling->offset = clamped;
            place_children();
        }
        ancestors.pop_back();
        placements_[&node] = {{x, y, x + width, y + viewport_height}, own_ancestors};
        return visible ? viewport_height : 0;
    }
    if (card) {
        const float height = ConstrainHeight(node, std::max(allocated_height, content + 2 * pad_y));
        const D2D1_RECT_F bounds{card_x, card_y, card_x + card_width, card_y + height};
        placements_[&node] = {bounds, own_ancestors};
        callbacks.place(node, bounds, clip, visible, enabled);
        return visible ? height : 0;
    }
    float height = ConstrainHeight(node, std::max(allocated_height, content + 2 * pad_y));
    placements_[&node] = {{card_x, card_y, card_x + card_width, card_y + height}, own_ancestors};
    return visible ? height : 0;
}
fluent::ScrollbarSpec ExtensionTreeLayout::Scrollbar(const UiNode *node, bool expanded) const {
    fluent::ScrollbarSpec bar;
    auto entry = viewports_.find(node);
    if (entry == viewports_.end())
        return bar;
    const auto &view = entry->second;
    bar.viewport = view.bounds;
    bar.viewport.left = bar.viewport.right - 14 * scale_;
    bar.offset = view.offset;
    bar.viewport_extent = view.bounds.bottom - view.bounds.top;
    bar.content_extent = view.content;
    bar.enabled = view.visible && view.enabled;
    bar.opacity = 1;
    bar.expand_progress = expanded ? 1.0f : 0.0f;
    return bar;
}
const UiNode *ExtensionTreeLayout::HitScrollbar(float x, float y) const {
    const UiNode *found = nullptr;
    size_t depth = 0;
    for (const auto &[node, view] : viewports_) {
        if (view.visible && Contains(view.clip, x, y) && fluent::ScrollbarHitTest(Scrollbar(node), x, y) &&
            (!found || view.depth >= depth)) {
            found = node;
            depth = view.depth;
        }
    }
    return found;
}
bool ExtensionTreeLayout::ScrollAt(float x, float y, float delta) {
    std::vector<Viewport *> candidates;
    for (auto &[node, view] : viewports_)
        if (view.visible && view.enabled && Contains(view.clip, x, y))
            candidates.push_back(&view);
    std::sort(candidates.begin(), candidates.end(), [](auto a, auto b) { return a->depth > b->depth; });
    for (auto *view : candidates) {
        float next = std::clamp(view->offset + delta, 0.0f,
                                std::max(0.0f, view->content - (view->bounds.bottom - view->bounds.top)));
        if (next != view->offset) {
            view->offset = next;
            return true;
        }
    }
    return false;
}
bool ExtensionTreeLayout::Drag(const UiNode *node, float y, float grab) {
    auto entry = viewports_.find(node);
    if (entry == viewports_.end())
        return false;
    auto bar = Scrollbar(node, true);
    auto thumb = fluent::ScrollbarThumbRect(bar, scale_);
    float next = fluent::ScrollbarDragOffset(bar.viewport.top, bar.viewport_extent, thumb.bottom - thumb.top,
                                             std::max(0.0f, bar.content_extent - bar.viewport_extent), y, grab);
    bool changed = next != entry->second.offset;
    entry->second.offset = next;
    return changed;
}
std::optional<D2D1_RECT_F> ExtensionTreeLayout::Bounds(const UiNode *node) const {
    auto at = placements_.find(node);
    if (at == placements_.end())
        return std::nullopt;
    return at->second.bounds;
}
bool ExtensionTreeLayout::Reveal(const UiNode *node) {
    auto found = placements_.find(node);
    if (found == placements_.end())
        return false;
    auto bounds = found->second.bounds;
    bool changed = false;
    for (auto at = found->second.ancestors.rbegin(); at != found->second.ancestors.rend(); ++at) {
        auto &view = viewports_.at(*at);
        float delta = bounds.top < view.bounds.top ? bounds.top - view.bounds.top
                      : bounds.bottom > view.bounds.bottom
                          ? (bounds.bottom - bounds.top > view.bounds.bottom - view.bounds.top
                                 ? bounds.top - view.bounds.top
                                 : bounds.bottom - view.bounds.bottom)
                          : 0;
        float next = std::clamp(view.offset + delta, 0.0f,
                                std::max(0.0f, view.content - (view.bounds.bottom - view.bounds.top)));
        float moved = next - view.offset;
        if (moved) {
            view.offset = next;
            bounds.top -= moved;
            bounds.bottom -= moved;
            changed = true;
        }
    }
    return changed;
}
} // namespace pulse::ui