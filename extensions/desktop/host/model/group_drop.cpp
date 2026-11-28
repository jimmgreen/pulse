// group_drop.cpp - Where files dragged onto a group card land, and the card's order afterwards.
#include "desktop_model.h"

#include <algorithm>
#include <cwctype>

namespace pulse::desktop::model {
namespace {
bool SamePath(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return true;
}
bool Contains(std::span<const std::wstring> list, std::wstring_view path) noexcept {
    return std::any_of(list.begin(), list.end(), [&](const std::wstring& p) { return SamePath(p, path); });
}
} // namespace

std::optional<GroupDropSlot> ComputeGroupDropSlot(const GroupComputedView& view, int x, int y) noexcept {
    if (!view.tab_active || !view.expanded || view.frame.empty() || view.item_columns < 1 ||
        view.cell_width < 1 || view.cell_height < 1) return std::nullopt;
    const RectI& content = view.content_rect;
    if (content.empty() || !content.contains(x, y)) return std::nullopt;
    const size_t visible = view.visible_item_indices.size();
    const size_t page_first = static_cast<size_t>(std::max(0, view.page_index)) * view.page_capacity;
    const int top = content.y + view.details_header_height;
    GroupDropSlot slot;
    if (visible == 0) {                     // an empty card: the whole card is the target, no caret
        slot.insert_index = page_first;
        return slot;
    }
    if (view.item_columns == 1) {           // 详细信息 and single-column cards: a line between rows
        const int idx = std::clamp((y - top + view.cell_height / 2) / view.cell_height, 0, static_cast<int>(visible));
        slot.insert_index = page_first + static_cast<size_t>(idx);
        slot.caret = {content.x + 4, top + idx * view.cell_height - 1, std::max(1, content.w - 8), 3};
        return slot;
    }
    const int cols = view.item_columns;
    const int rows_used = static_cast<int>((visible + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols));
    int row = std::clamp((y - top) / view.cell_height, 0, rows_used - 1);
    int col = std::clamp((x - content.x + view.cell_width / 2) / view.cell_width, 0, cols);
    size_t idx = static_cast<size_t>(row) * static_cast<size_t>(cols) + static_cast<size_t>(col);
    if (idx > visible) {                    // past the last icon: right after it
        idx = visible;
        row = static_cast<int>(visible / static_cast<size_t>(cols));
        col = static_cast<int>(visible % static_cast<size_t>(cols));
        if (col == 0) { --row; col = cols; }
    }
    slot.insert_index = page_first + idx;
    slot.caret = {content.x + col * view.cell_width - 1, top + row * view.cell_height + 6, 3,
                  std::max(8, view.cell_height - 12)};
    return slot;
}

std::vector<std::wstring> ComposeManualOrder(std::span<const std::wstring> display_order,
                                             std::span<const std::wstring> dragged, size_t insert_index) {
    insert_index = std::min(insert_index, display_order.size());
    std::vector<std::wstring> order;
    std::vector<std::wstring> moving;
    order.reserve(display_order.size() + dragged.size());
    size_t at = insert_index;
    for (size_t i = 0; i < display_order.size(); ++i) {
        if (Contains(dragged, display_order[i])) {
            moving.push_back(display_order[i]);  // members keep their relative order
            if (i < insert_index) --at;
        } else {
            order.push_back(display_order[i]);
        }
    }
    for (const auto& p : dragged)
        if (!p.empty() && !Contains(moving, p)) moving.push_back(p);
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(std::min(at, order.size())), moving.begin(), moving.end());
    return order;
}

bool ApplyGroupManualOrder(std::vector<DesktopGroup>& groups, std::string_view group_id,
                           std::vector<std::wstring> order) {
    bool switched = false;
    for (auto& g : groups) {
        if (g.id == group_id) {
            switched = g.sort_by != GroupSortBy::Manual;
            g.sort_by = GroupSortBy::Manual;
            g.manual_order = order;
        } else if (!g.manual_order.empty()) {
            // A file is in one card: its old place elsewhere goes.
            std::erase_if(g.manual_order, [&](const std::wstring& p) { return Contains(order, p); });
        }
    }
    return switched;
}

std::wstring NextNewGroupTitle(const std::vector<DesktopGroup>& groups, std::wstring_view base) {
    const auto taken = [&](std::wstring_view title) {
        return std::any_of(groups.begin(), groups.end(), [&](const DesktopGroup& g) { return g.title == title; });
    };
    if (!taken(base)) return std::wstring(base);
    for (int n = 2; n < 1000; ++n) {
        std::wstring candidate = std::wstring(base) + L" " + std::to_wstring(n);
        if (!taken(candidate)) return candidate;
    }
    return std::wstring(base);
}

} // namespace pulse::desktop::model
