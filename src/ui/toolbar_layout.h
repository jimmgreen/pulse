#pragma once
#include "responsive_chrome_layout.h"
#include <d2d1.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace pulse::ui {
inline float NavigationStep(float available, float scale) {
    return (available < 500 * scale ? 26.0f : 34.0f) * scale;
}
inline D2D1_RECT_F NavigationButtonAt(float x, float y, float scale, float step) {
    return {x, y, x + step - 4 * scale, y + 32 * scale};
}
// Toolbar pins: the pinned commands (自定义工具栏), then the pinned apps.
constexpr int kToolbarCommandPins = 34;   // = app::kMaxToolbarTools (every tool)
constexpr int kToolbarAppPins = 8;
// Glyphs of the split and column-view tools (no icon-font meaning; mapped in
// command_icons.h). The toolbar draws the split pin as the current layout.
inline constexpr const wchar_t* kToolbarSplitGlyph = L"\xF5B0";
inline constexpr const wchar_t* kToolbarColumnsGlyph = L"\xF5B1";
constexpr int kToolbarPinSlots = kToolbarCommandPins + kToolbarAppPins;
struct ToolbarPinOptions {
    std::array<int,3> view_slots{-1,-1,-1}; // view controls inserted among left pins
    int right_from = -1;        // first pin in the right-aligned tools group
    int app_from = -1;          // index of the first app pin (a short gap goes before it)
    bool drop_mode = false;     // a file drag is over the window: pins also in the narrow tier
    float label_width = 0.0f;   // drop mode: "拖到软件上打开" / "固定到工具栏" ahead of the pins
    uint64_t separators = 0;    // bit i: the user's separator before pin i (app::ToolbarSeparatorMask)
    std::array<int, 3> view_order{0, 1, 2}; // sort, filter, group identities
};
struct ToolbarLayout {
    std::array<D2D1_RECT_F, 4> navigation{};
    D2D1_RECT_F address{}, search{}, create{};
    D2D1_RECT_F sort{}, filter{}, overflow{};
    D2D1_RECT_F group{};   // "Group" button / active chip (empty when not offered)
    std::array<D2D1_RECT_F, 8> commands{};
    std::array<D2D1_RECT_F, kToolbarPinSlots> pins{};   // pinned commands, then pinned apps
    D2D1_RECT_F pin_label{};   // drop mode only (ToolbarPinOptions::label_width)
    uint64_t pin_dividers = 0; // bit i: a separator is drawn before pins[i] (both pins shown)
    bool narrow = false;       // the narrow tier: every pin waits in More
    D2D1_RECT_F address_text{};
    NavigationTier navigation_tier = NavigationTier::Full;
    bool refresh_inside = false;
    bool merged_search = false;
};

inline float SelectionChipSpan(float scale, float label_width, int n);

// search_min_dip widens the search field while a search is active or being
// edited, borrowing from the address bar but leaving it at least 120 DIP.
inline ToolbarLayout MakeToolbarLayout(float width, float scale, float top, float margin, float create_width,
                                      float left, float filter_expand = 0.0f, float search_min_dip = 0.0f,
                                      float group_width = 0.0f, float sort_width = 0.0f,
                                      float filter_width = 0.0f, int pin_count = 0,
                                      float selection_span = 0.0f, const ToolbarPinOptions& pin_options = {},
                                      bool can_forward = true) {
    ToolbarLayout out;
    const float available = width - left;
    const auto navigation = MakeResponsiveNavigation(width, left, top, margin, scale, can_forward, search_min_dip);
    out.navigation = navigation.navigation;
    out.address = navigation.address;
    out.search = navigation.search;
    out.address_text = navigation.address_text;
    out.navigation_tier = navigation.tier;
    out.refresh_inside = navigation.refresh_inside;
    out.merged_search = navigation.merged;
    const bool overflow = available < 540*scale;
    float x = left + margin;
    out.create = {left+margin,top+50*scale,left+margin+create_width,top+82*scale};
    out.narrow = overflow;
    // Toolbar picker T3d: cut .. delete and split / details / columns are
    // tools like any other now (toolbar_tools), drawn as pins; `commands`
    // stays empty.
    x = out.create.right;
    // Left cluster ends here: 新建 + file commands ("act on files"), plus the
    // pinned commands placed below.
    float left_end = x + 8*scale;
    // Right cluster ("how to look at files"), right-aligned like the prototype:
    // [排序][筛选][分组] | [layout][preview][columns] [more]; narrow: ... [more].
    // More is always there (prototype note 5): select all, terminal,
    // properties, 自定义工具栏, plus the folded commands when narrow.
    out.overflow = {width-margin-32*scale,top+50*scale,width-margin,top+82*scale};
    const float view_end = out.overflow.left-8*scale;
    // Pinned commands and apps (自定义工具栏) follow the file commands after a
    // divider, as many as leave the view group its icon-only minimum. The rest,
    // and all of them in the narrow tier, wait in the More menu. While files are
    // dragged over the window the toolbar holds drop targets only (the caller
    // passes the apps alone): they show in the narrow tier too, after a label
    // when it fits together with every one of them.
    // An open filter (filter_expand > 0) asks for 200 DIP to type in: the
    // pinned commands and apps fold into More, the view labels to icons and
    // the selection chip to its smallest form (more, clear) first. That
    // happens as the field starts to open, not midway through the animation.
    const bool filter_open = filter_expand > 0.0f;
    // 自定义工具栏 → 排序与筛选: a negative sort / filter width hides that
    // button (an open filter shows anyway); a hidden group passes 0.
    const std::array<bool,3> left_view{
        !overflow && pin_options.view_slots[0]>=0 && sort_width>=0,
        !overflow && !filter_open && pin_options.view_slots[1]>=0 && filter_width>=0,
        !overflow && pin_options.view_slots[2]>=0 && group_width>0};
    const std::array<float,3> left_width{
        available>=700*scale ? (sort_width>0?sort_width:88*scale) : 32*scale,
        available>=700*scale ? (filter_width>0?std::max(filter_width,64*scale):88*scale) : 32*scale,
        group_width};
    std::array<D2D1_RECT_F,3> left_rects{};
    const bool sort_shown = sort_width >= 0.0f && !left_view[0];
    const bool filter_shown = (filter_width >= 0.0f || filter_open) && !left_view[1];
    if(left_view[2])group_width=0;
    const float filter_open_w = 200*scale;
    const float chip_span = filter_open ? std::min(selection_span, SelectionChipSpan(scale, 0.0f, 0)) : selection_span;
    const bool drop_mode = pin_options.drop_mode;
    if ((!overflow || drop_mode) && (pin_count > 0 || left_view[0] || left_view[1] || left_view[2] || (drop_mode && pin_options.label_width > 0.0f))) {
        const float view_min = (sort_shown ? 38*scale : 0.0f) + (filter_shown ? 32*scale : 0.0f) +
                               (group_width > 0.0f ? 38*scale : 0.0f) +
                               (filter_open ? filter_open_w - 32*scale + chip_span : 0.0f);
        const int count = std::min(pin_count, kToolbarPinSlots);
        const bool gap = pin_options.app_from > 0 && pin_options.app_from < count;
        float px = x + 13.0f*scale;   // 13 DIP after 新建 (a divider at 6), like the selection chip
        if (drop_mode && pin_options.label_width > 0.0f) {
            const float all = pin_options.label_width + 4*scale + static_cast<float>(count)*34*scale;
            if (px+all+8*scale+view_min <= view_end) {
                out.pin_label = {px,top+50*scale,px+pin_options.label_width,top+82*scale};
                px = out.pin_label.right + 4*scale;
                left_end = px + 4*scale;
            }
        }
        auto insert_views = [&](int slot) {
            for(int view:pin_options.view_order) {
                if(view<0 || view>=3 || !left_view[view] || std::min(pin_options.view_slots[view],count)!=slot)continue;
                const float w=left_width[view];
                if(px+w+8*scale+view_min>view_end)continue;
                left_rects[view]={px,top+50*scale,px+w,top+82*scale};
                px+=w+6*scale;left_end=px+8*scale;
            }
        };
        for (int i=0;i<count;++i) {
            insert_views(i);
            // The user's separators take the same 9 DIP as the gap before the apps.
            const bool divider = i > 0 && i < 64 && (pin_options.separators >> i & 1);
            const float lead = (gap && i == pin_options.app_from) || divider ? 9*scale : 0.0f;
            if (px+lead+30*scale+8*scale+view_min > view_end) break;
            px += lead;
            if (divider) out.pin_dividers |= uint64_t{1} << i;
            out.pins[i] = {px,top+50*scale,px+30*scale,top+82*scale};
            px += 34*scale;
            left_end = px + 8*scale;
        }
        insert_views(count);
    }
    // sort_width: measured "icon + label + chevron" width (0 = legacy 88 DIP).
    float sort_w = !sort_shown ? 0.0f
        : available >= 700*scale ? (sort_width > 0.0f ? sort_width : 88*scale) : 32*scale;
    // filter_width: measured "icon + label" width (0 = legacy 88 DIP); kept above the
    // 60 DIP icon-only threshold. Expanding grows the field to the left.
    float filter_base = !filter_shown ? 0.0f : available >= 700*scale
        ? (filter_width > 0.0f ? std::max(filter_width, 64*scale) : 88*scale) : 32*scale;
    float group_w = group_width;
    // While something is selected the selection chip (prototype note 4) comes
    // before the view labels: they fold to icons first so the chip keeps its
    // quick actions. The chip itself shrinks into whatever is left.
    const float room = view_end - left_end - (overflow ? 0.0f : chip_span);
    // The shown buttons with 6 DIP between them (all three: sort+6+filter+6+group).
    auto block_of = [&](float filter) {
        const int n = (sort_shown ? 1 : 0) + (filter_shown ? 1 : 0) + (group_w > 0.0f ? 1 : 0);
        return (sort_shown ? sort_w : 0.0f) + (filter_shown ? filter : 0.0f) + (group_w > 0.0f ? group_w : 0.0f) +
               (n > 1 ? static_cast<float>(n-1)*6*scale : 0.0f);
    };
    // Priority collapse when the labels do not fit: icon-only sort / filter
    // first, then a narrower group chip (never below its icon).
    if (block_of(filter_base) > room) {
        if (sort_shown) sort_w = 32*scale;
        if (filter_shown) filter_base = 32*scale;
    }
    const float filter_goal = !filter_shown ? 0.0f : filter_open ? std::max(filter_base, filter_open_w) : filter_base;
    if (sort_shown && block_of(filter_goal) > room) sort_w = 32*scale;
    if (group_w > 0.0f && block_of(filter_goal) > room)
        group_w = std::max(32*scale, group_w-(block_of(filter_goal)-room));
    const float filter_max = std::clamp(room-block_of(0.0f),filter_base,220*scale);
    const float filter_w = !filter_shown ? 0.0f
        : filter_base + (filter_max-filter_base)*std::clamp(filter_expand,0.0f,1.0f);
    // Still too narrow to right-align: start right after the left cluster instead.
    x = std::max(left_end, view_end-block_of(filter_w));
    // Keep semantic rects for painting/hit testing while placing controls in
    // the saved order. Hidden controls take no space or separator.
    const std::array<float, 3> widths{sort_shown ? sort_w : 0.0f,
        filter_shown ? filter_w : 0.0f, group_width > 0.0f ? group_w : 0.0f};
    std::array<D2D1_RECT_F*, 3> rects{&out.sort, &out.filter, &out.group};
    std::array<bool, 3> placed{};
    bool shown = false;
    const auto place = [&](int index) {
        if (index < 0 || index >= 3 || placed[index]) return;
        placed[index] = true;
        const float w = widths[index];
        if (w > 0.0f && shown) x += 6*scale;
        *rects[index] = {x, top+50*scale, x+w, top+82*scale};
        x += w;
        shown = shown || w > 0.0f;
    };
    for (int index : pin_options.view_order) place(index);
    for (int index = 0; index < 3; ++index) place(index);
    for(int i=0;i<3;++i) if(left_view[i]) *rects[i]=left_rects[i];
    if (!drop_mode && pin_options.right_from >= 0 && pin_options.right_from < kToolbarPinSlots) {
        int last = pin_options.right_from;
        for (int i = last; i < kToolbarPinSlots; ++i)
            if (out.pins[i].right > out.pins[i].left) last = i;
        const auto& last_pin = out.pins[last];
        float view_left = view_end;
        for(int i=0;i<3;++i) if(!left_view[i] && rects[i]->right>rects[i]->left) view_left=std::min(view_left,rects[i]->left);
        const float shift = std::max(0.0f, view_left - 8*scale - chip_span - last_pin.right);
        for (int i = pin_options.right_from; i <= last; ++i) {
            if (out.pins[i].right <= out.pins[i].left) continue;
            out.pins[i].left += shift; out.pins[i].right += shift;
        }
    }
    return out;
}

// Selection context (prototype note 4): up to kSelChipMaxActions commands of
// the Pulse item menu in prototype priority (标签, 复制路径, ...; not filtered
// by the right-click menu's visibility settings), then "more" (the full item
// menu) and "clear".
// Priority collapse: the label goes first, then trailing quick actions;
// "more" and "clear" stay. An empty chip means it does not fit at all.
struct SelectionChipAction {
    int command = 0;
    std::wstring glyph;
    std::wstring label;   // tooltip, with the shortcut
    bool enabled = true;
    bool checked = false; // toolbar toggle tools: the setting is on
    bool situational = false; // offered because of the moment (split, two folders, ...): accent underline
};
// Four parts (toolbar redesign §5): the count (a click opens the selection
// menu), up to 2 situational actions, up to 4 of the user's actions, then
// 固定到…, more and clear.
constexpr int kSelChipSituational = 2;
constexpr int kSelChipCommon = 4;
constexpr int kSelChipMaxActions = kSelChipSituational + kSelChipCommon;
constexpr int kSelChipPin = kSelChipMaxActions;
constexpr int kSelChipMore = kSelChipMaxActions + 1;
constexpr int kSelChipClear = kSelChipMaxActions + 2;
constexpr int kSelChipSlots = kSelChipMaxActions + 3;
constexpr int kSelChipLabel = kSelChipSlots;   // hit index of the count
struct ToolbarSelectionChip {
    D2D1_RECT_F chip{}, label{};
    std::array<D2D1_RECT_F, kSelChipSlots> buttons{};   // quick actions, pin to, more, clear
};
// Chip width with `n` quick actions (label_width 0 = no label).
inline float SelectionChipWidth(float scale, float label_width, int n) {
    const float buttons = static_cast<float>((n + 3)*30 - 2)*scale + (n > 0 ? 7*scale : 0.0f);
    return (label_width > 0.0f ? 10*scale + label_width + 4*scale : 4*scale) + buttons + 4*scale;
}
// Room the chip asks of the toolbar row: the 13 DIP lead (divider) and the
// 10 DIP gap before the view group included.
inline float SelectionChipSpan(float scale, float label_width, int n) {
    return 13*scale + SelectionChipWidth(scale, label_width, std::clamp(n, 0, kSelChipMaxActions)) + 10*scale;
}
inline ToolbarSelectionChip MakeToolbarSelectionChip(const ToolbarLayout& l, float scale, float label_width,
                                                     int action_count) {
    ToolbarSelectionChip out;
    D2D1_RECT_F anchor = l.commands[4].right > l.commands[4].left ? l.commands[4] : l.create;
    for (const D2D1_RECT_F& pin : l.pins) if (pin.right > pin.left) anchor = pin;
    // A moved view on the left participates in the same occupied span.
    float limit = l.overflow.left - 10*scale;
    for(const auto& view : {l.sort,l.filter,l.group}) {
        if(view.right<=view.left)continue;
        if(view.left>=anchor.right)limit=std::min(limit,view.left-10*scale);
        else if(view.right>anchor.right)anchor=view;
    }
    const float chip_x = anchor.right + 13*scale;
    int count = std::clamp(action_count, 0, kSelChipMaxActions);
    const auto width = [&](float label, int n) { return SelectionChipWidth(scale, label, n); };
    if (label_width > 0.0f && chip_x + width(label_width, count) > limit) label_width = 0.0f;
    while (count > 0 && chip_x + width(label_width, count) > limit) --count;
    if (chip_x + width(label_width, count) > limit) return out;
    out.chip = {chip_x, l.create.top, chip_x + width(label_width, count), l.create.bottom};
    float x = chip_x + 4*scale;
    if (label_width > 0.0f) {
        out.label = {chip_x + 10*scale, l.create.top, chip_x + 10*scale + label_width, l.create.bottom};
        x = out.label.right + 4*scale;
    }
    const auto place = [&](int slot) {
        out.buttons[static_cast<size_t>(slot)] = {x, l.create.top + 2*scale, x + 28*scale, l.create.bottom - 2*scale};
        x += 30*scale;
    };
    for (int i = 0; i < count; ++i) place(i);
    if (count > 0) x += 7*scale;   // room for a divider before pin to / more / clear
    place(kSelChipPin);
    place(kSelChipMore);
    place(kSelChipClear);
    return out;
}

// Trailing "x" of the active group chip (removes grouping).
inline D2D1_RECT_F ToolbarGroupClearRect(const D2D1_RECT_F& chip, float scale) {
    return {chip.right-28*scale,chip.top+5*scale,chip.right-6*scale,chip.bottom-5*scale};
}
}
