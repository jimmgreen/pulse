#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace pulse::ui {

struct TabRowItem {
    float width = 120.0f;
    bool pinned = false;
    bool hidden = false;
};

struct TabRowSlot {
    float x = 0.0f;
    float width = 0.0f;
    int row = -1;
};

struct TabRowsLayout {
    std::vector<TabRowSlot> slots;
    int rows = 1;
    int first_row = 0;
    int visible_rows = 1;

    // The title row stays fixed while only the rows beneath it scroll.
    int VisibleRow(int row) const {
        if (row <= 0) return row;
        const int visible = row - first_row;
        return visible >= 1 && visible < visible_rows ? visible : -1;
    }
};

// Coordinates are DIP. The first visible row reserves the window controls;
// subsequent rows use the complete strip. Hidden group members take no space.
inline TabRowsLayout ComputeTabRows(const std::vector<TabRowItem>& items,
    float width, float first_left, float first_right, bool aligned,
    int max_rows, int requested_first, int active, bool reveal_active) {
    TabRowsLayout result;
    result.slots.resize(items.size());
    width = (std::max)(1.0f, width);
    first_left = std::clamp(first_left, 0.0f, width - 1.0f);
    first_right = std::clamp(first_right, first_left + 1.0f, width);
    max_rows = std::clamp(max_rows, 2, 4);
    const float padding=(std::min)({8.0f,width*0.25f,(width-1.0f)*0.5f});
    const int columns = (std::max)(1, static_cast<int>((width-2*padding) / 120.0f));
    const float column_width = (std::min)(240.0f, (width-2*padding) / static_cast<float>(columns));
    int row = 0;
    float x = first_left;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        if (item.hidden) continue;
        float right = row == 0 ? first_right : width-padding;
        float desired = item.pinned ? item.width : aligned ? column_width
            : std::clamp(item.width, 120.0f, 240.0f);
        if (aligned && !item.pinned) x = padding + std::ceil((x-padding) / column_width - 0.0001f) * column_width;
        const float left = row == 0 ? first_left : padding;
        if (x > left && x + desired > right) {
            ++row;
            x = padding;
            right = width-padding;
        }
        desired = std::clamp(desired, 1.0f, (std::max)(1.0f, right - x));
        result.slots[i] = {x, desired, row};
        x += desired;
    }
    // Fit-title rows fill their available width, except for the final row.
    if (!aligned) for(int line=0;line<row;++line) {
        float used=0;int stretch=0;
        for(size_t i=0;i<items.size();++i) if(result.slots[i].row==line) {
            used+=result.slots[i].width;
            if(!items[i].pinned) ++stretch;
        }
        const float available=(line==0 ? first_right-first_left : width-2*padding);
        const float extra=stretch ? (std::max)(0.0f,available-used)/stretch : 0;
        float left=line==0 ? first_left : padding;
        for(size_t i=0;i<items.size();++i) if(result.slots[i].row==line) {
            auto& slot=result.slots[i];slot.x=left;
            if(!items[i].pinned) slot.width+=extra;
            left+=slot.width;
        }
    }
    result.rows = row + 1;
    result.visible_rows = (std::min)(max_rows, result.rows);
    result.first_row = std::clamp(requested_first, 0, result.rows - result.visible_rows);
    if (reveal_active && active >= 0 && active < static_cast<int>(items.size())) {
        const int active_row = result.slots[static_cast<size_t>(active)].row;
        if (active_row >= 0) {
            if (active_row > 0 && active_row <= result.first_row)
                result.first_row = active_row - 1;
            if (active_row > 0 && active_row >= result.first_row + result.visible_rows)
                result.first_row = active_row - result.visible_rows + 1;
        }
    }
    return result;
}

} // namespace pulse::ui
