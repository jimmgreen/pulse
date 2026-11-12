// settings_dialog_records.h — geometry of the dialog takeover records in
// Settings (dialog_records_mockup.html), shared by layout, drawing and tests:
// icon, name with the executable name, a status chip and one summary line;
// a box with what happened and what to do; a later-success note.
#pragma once
#include "fluent_components.h"
#include "ui_view_model.h"
#include <algorithm>
#include <string>

namespace pulse::ui::dialog_records {

constexpr float kPad = 12.0f;      // above and below a row's content
constexpr float kSide = 20.0f;     // row edges to content
constexpr float kIcon = 32.0f;     // program icon
constexpr float kGap = 12.0f;      // icon to text
constexpr float kHead = 40.0f;     // name line + summary line
constexpr float kTitleH = 22.0f;   // name line
constexpr float kBoxTop = 8.0f;    // summary to the detail box
constexpr float kBoxPadX = 10.0f, kBoxPadY = 8.0f;
constexpr float kLaterTop = 6.0f;
constexpr float kLaterIcon = 18.0f;  // check mark slot before the later note
constexpr float kGroupH = 30.0f;     // group caption row

inline float TextLeft() { return kSide + kIcon + kGap; }

// The detail's first line says what happened; the rest says what to do.
inline std::pair<std::wstring, std::wstring> SplitDetail(const std::wstring& detail) {
    const auto at = detail.find(L'\n');
    return at == std::wstring::npos ? std::pair{detail, std::wstring()} : std::pair{detail.substr(0, at), detail.substr(at + 1)};
}

// Wrap widths in pixels for a row `row_width` pixels wide.
inline float BoxTextWidth(float row_width, float scale) {
    return (std::max)(40.0f * scale, row_width - (TextLeft() + kSide + 2.0f * kBoxPadX) * scale);
}
inline float LaterTextWidth(float row_width, float scale) {
    return (std::max)(40.0f * scale, row_width - (TextLeft() + kSide + kLaterIcon) * scale);
}

inline float BoxHeight(const fluent::Painter& painter, const WindowViewModel::DialogTakeoverRow& row, float row_width, float scale) {
    if (row.detail.empty()) return 0.0f;
    const auto [what, todo] = SplitDetail(row.detail);
    const float width = BoxTextWidth(row_width, scale);
    float h = 2.0f * kBoxPadY * scale + painter.MeasureWrappedCaptionHeight(what, width);
    if (!todo.empty()) h += 2.0f * scale + painter.MeasureWrappedCaptionHeight(todo, width);
    return h;
}

// Pixel height of `row` in a row `row_width` pixels wide.
inline float RowHeight(const fluent::Painter& painter, const WindowViewModel::DialogTakeoverRow& row, float row_width, float scale) {
    float h = (kPad + kHead + kPad) * scale;
    if (const float box = BoxHeight(painter, row, row_width, scale); box > 0.0f) h += kBoxTop * scale + box;
    if (!row.later.empty()) h += kLaterTop * scale + painter.MeasureWrappedCaptionHeight(row.later, LaterTextWidth(row_width, scale));
    return h;
}

} // namespace pulse::ui::dialog_records
