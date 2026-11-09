#include "extension_data_view.h"
#include "FluentTokens.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace pulse::ui {
namespace {
std::wstring WideData(const std::string &text) {
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(count), 0);
    if (count)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
                            count);
    return result;
}
bool Contains(D2D1_RECT_F rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}
} // namespace
void ExtensionDataView::Configure(const extensions::UiNode &node) {
    node_ = node;
    collapsed_.clear();
    std::function<void(const std::vector<extensions::UiDataRow> &)> initial;
    initial = [&](const auto &rows) {
        for (const auto &row : rows) {
            if (!row.expanded)
                collapsed_.insert(row.id);
            initial(row.children);
        }
    };
    initial(node_.rows);
    selected_.clear();
    scroll_ = horizontal_ = 0;
    drag_axis_ = 0;
    sort_column_ = -1;
    Rebuild();
}
bool ExtensionDataView::ContainsRow(const std::string &id) const {
    std::function<bool(const std::vector<extensions::UiDataRow> &)> contains = [&](const auto &rows) {
        for (const auto &row : rows)
            if (row.id == id || contains(row.children))
                return true;
        return false;
    };
    return id.empty() || contains(node_.rows);
}
extensions::UiNode ExtensionDataView::SnapshotNode() const {
    auto node = node_;
    std::function<void(std::vector<extensions::UiDataRow> &)> expand = [&](auto &rows) {
        for (auto &row : rows) {
            row.expanded = !collapsed_.contains(row.id);
            expand(row.children);
        }
    };
    expand(node.rows);
    return node;
}
void ExtensionDataView::CopyViewState(const ExtensionDataView &source) {
    collapsed_ = source.collapsed_;
    selected_ = source.selected_;
    sort_column_ = source.sort_column_;
    sort_descending_ = source.sort_descending_;
    scroll_ = source.scroll_ * scale_ / source.scale_;
    horizontal_ = source.horizontal_ * scale_ / source.scale_;
    Rebuild();
}
void ExtensionDataView::UpdateNode(const extensions::UiNode &node) {
    auto rows = node_.rows;
    node_ = node;
    if (node.data_source)
        node_.rows = std::move(rows);
    if (!ContainsRow(selected_))
        selected_.clear();
    if (sort_column_ >= static_cast<int>(node_.data_columns.size()))
        sort_column_ = -1;
    Rebuild();
}
void ExtensionDataView::UpdateRows(const std::vector<extensions::UiDataRow> &rows) {
    node_.rows = rows;
    if (!ContainsRow(selected_))
        selected_.clear();
    Rebuild();
}
float ExtensionDataView::RowHeight() const { return 32 * scale_; }
float ExtensionDataView::HeaderHeight() const {
    return node_.type == extensions::UiNodeType::DataTable ? 36 * scale_ : 0;
}
float ExtensionDataView::ContentWidth() const {
    float width = 0;
    for (const auto &column : node_.data_columns)
        width += column.width * scale_;
    return width;
}
D2D1_RECT_F ExtensionDataView::Body() const {
    auto body = bounds_;
    body.top = (std::min)(body.bottom, body.top + HeaderHeight());
    body.right = (std::max)(body.left, body.right - 14 * scale_);
    if (ContentWidth() > body.right - body.left)
        body.bottom = (std::max)(body.top, body.bottom - 14 * scale_);
    return body;
}
fluent::ScrollbarSpec ExtensionDataView::Bar(bool horizontal) const {
    const auto body = Body();
    fluent::ScrollbarSpec bar;
    bar.horizontal = horizontal;
    bar.expand_progress = 1;
    bar.viewport = horizontal ? D2D1::RectF(body.left, body.bottom, body.right, bounds_.bottom)
                              : D2D1::RectF(body.right, body.top, bounds_.right, body.bottom);
    bar.viewport_extent = horizontal ? body.right - body.left : body.bottom - body.top;
    bar.content_extent = horizontal ? ContentWidth() : static_cast<float>(rows_.size()) * RowHeight();
    bar.offset = horizontal ? horizontal_ : scroll_;
    return bar;
}
void ExtensionDataView::Clamp() {
    const auto body = Body();
    scroll_ = std::clamp(scroll_, 0.0f,
                         (std::max)(0.0f, static_cast<float>(rows_.size()) * RowHeight() - (body.bottom - body.top)));
    horizontal_ = std::clamp(horizontal_, 0.0f, (std::max)(0.0f, ContentWidth() - (body.right - body.left)));
}
void ExtensionDataView::SetViewport(D2D1_RECT_F bounds, float scale) {
    if (!(scale > 0) || !std::isfinite(scale))
        return;
    scroll_ *= scale / scale_;
    horizontal_ *= scale / scale_;
    bounds_ = bounds;
    scale_ = scale;
    Clamp();
}
void ExtensionDataView::Rebuild() {
    rows_.clear();
    std::function<void(const std::vector<extensions::UiDataRow> &, size_t)> append;
    append = [&](const auto &source, size_t depth) {
        std::vector<const extensions::UiDataRow *> ordered;
        for (const auto &row : source)
            ordered.push_back(&row);
        if (sort_column_ >= 0) {
            std::stable_sort(ordered.begin(), ordered.end(), [&](const auto *a, const auto *b) {
                const auto cell = [&](const auto *row) -> std::string_view {
                    return static_cast<size_t>(sort_column_) < row->cells.size()
                               ? std::string_view(row->cells[sort_column_])
                               : std::string_view(row->text);
                };
                return sort_descending_ ? cell(a) > cell(b) : cell(a) < cell(b);
            });
        }
        for (const auto *row : ordered) {
            Row visible{row,
                        depth,
                        WideData(row->text.empty() && !row->cells.empty() ? row->cells.front() : row->text),
                        WideData(row->glyph),
                        {}};
            for (const auto &cell : row->cells)
                visible.cells.push_back(WideData(cell));
            rows_.push_back(std::move(visible));
            if (!collapsed_.contains(row->id))
                append(row->children, depth + 1);
        }
    };
    append(node_.rows, 0);
    Clamp();
}
void ExtensionDataView::EnsureVisible() {
    const auto it =
        std::find_if(rows_.begin(), rows_.end(), [&](const auto &row) { return row.data->id == selected_; });
    if (it == rows_.end())
        return;
    const float top = static_cast<float>(it - rows_.begin()) * RowHeight();
    const auto body = Body();
    if (top < scroll_)
        scroll_ = top;
    else if (top + RowHeight() > scroll_ + body.bottom - body.top)
        scroll_ = top + RowHeight() - (body.bottom - body.top);
    Clamp();
}
bool ExtensionDataView::SetSelection(const std::string &id) {
    if (id == selected_)
        return true;
    if (id.empty()) {
        selected_.clear();
        return true;
    }
    std::vector<std::string> ancestors;
    std::function<bool(const std::vector<extensions::UiDataRow> &)> find;
    find = [&](const auto &rows) {
        for (const auto &row : rows) {
            if (row.id == id)
                return row.enabled;
            ancestors.push_back(row.id);
            if (find(row.children))
                return true;
            ancestors.pop_back();
        }
        return false;
    };
    if (!find(node_.rows))
        return false;
    bool expanded = false;
    for (const auto &parent : ancestors)
        expanded = collapsed_.erase(parent) != 0 || expanded;
    if (expanded)
        Rebuild();
    selected_ = id;
    EnsureVisible();
    return true;
}
std::wstring ExtensionDataView::SelectedText() const {
    for (const auto &row : rows_)
        if (row.data->id == selected_)
            return row.text;
    return {};
}
std::vector<DataAccessibleRow> ExtensionDataView::AccessibilityRows() const {
    std::vector<DataAccessibleRow> result;
    result.reserve(rows_.size());
    const auto body = Body();
    for (size_t index = 0; index < rows_.size(); ++index) {
        const auto &row = rows_[index];
        const float top = body.top + static_cast<float>(index) * RowHeight() - scroll_;
        DataAccessibleRow accessible;
        accessible.id = row.data->id;
        accessible.name = row.text;
        accessible.screen_bounds = {static_cast<LONG>(body.left), static_cast<LONG>(top), static_cast<LONG>(body.right),
                                    static_cast<LONG>(top + RowHeight())};
        accessible.enabled = row.data->enabled;
        accessible.selected = selected_ == row.data->id;
        accessible.offscreen = top + RowHeight() <= body.top || top >= body.bottom || body.right <= body.left;
        accessible.has_children = !row.data->children.empty();
        accessible.expanded = !collapsed_.contains(row.data->id);
        result.push_back(std::move(accessible));
    }
    return result;
}
int ExtensionDataView::HitRow(float x, float y) const {
    const auto body = Body();
    if (!Contains(body, x, y))
        return -1;
    const int index = static_cast<int>((y - body.top + scroll_) / RowHeight());
    return index >= 0 && static_cast<size_t>(index) < rows_.size() ? index : -1;
}
ExtensionDataView::Event ExtensionDataView::SelectIndex(int index) {
    if (index < 0 || static_cast<size_t>(index) >= rows_.size() || !rows_[index].data->enabled)
        return {true};
    const auto id = rows_[index].data->id;
    const bool changed = id != selected_;
    selected_ = id;
    EnsureVisible();
    return {true, changed, {}};
}
ExtensionDataView::Event ExtensionDataView::Activate(int index) {
    if (index < 0 || static_cast<size_t>(index) >= rows_.size() || !rows_[index].data->enabled)
        return {true};
    auto event = SelectIndex(index);
    const auto *row = rows_[index].data;
    if (!row->children.empty()) {
        if (!collapsed_.erase(row->id))
            collapsed_.insert(row->id);
        Rebuild();
        EnsureVisible();
    } else
        event.command = row->command;
    return event;
}
ExtensionDataView::Event ExtensionDataView::PointerDown(float x, float y) {
    if (!Contains(bounds_, x, y))
        return {};
    for (bool horizontal : {false, true}) {
        const auto bar = Bar(horizontal);
        if (bar.content_extent > bar.viewport_extent && fluent::ScrollbarHitTest(bar, x, y)) {
            const auto thumb = fluent::ScrollbarThumbRect(bar, scale_);
            drag_axis_ = horizontal ? 2 : 1;
            grab_ = fluent::ScrollbarGrabOffset(horizontal ? thumb.left : thumb.top,
                                                horizontal ? thumb.right - thumb.left : thumb.bottom - thumb.top,
                                                horizontal ? x : y);
            return PointerMove(x, y);
        }
    }
    if (y < bounds_.top + HeaderHeight()) {
        float left = bounds_.left - horizontal_;
        for (size_t i = 0; i < node_.data_columns.size(); ++i) {
            const float right = left + node_.data_columns[i].width * scale_;
            if (x >= left && x < right) {
                sort_descending_ = sort_column_ == static_cast<int>(i) && !sort_descending_;
                sort_column_ = static_cast<int>(i);
                Rebuild();
                EnsureVisible();
                return {true};
            }
            left = right;
        }
        return {true};
    }
    const int index = HitRow(x, y);
    if (index >= 0 && !rows_[index].data->children.empty() &&
        x < Body().left + (static_cast<float>(rows_[index].depth) * 20 + 24) * scale_)
        return Activate(index);
    return SelectIndex(index);
}
ExtensionDataView::Event ExtensionDataView::PointerMove(float x, float y) {
    if (!drag_axis_)
        return {};
    const bool horizontal = drag_axis_ == 2;
    const auto bar = Bar(horizontal);
    const auto thumb = fluent::ScrollbarThumbRect(bar, scale_);
    const auto offset = fluent::ScrollbarDragOffset(
        horizontal ? bar.viewport.left : bar.viewport.top,
        horizontal ? bar.viewport.right - bar.viewport.left : bar.viewport.bottom - bar.viewport.top,
        horizontal ? thumb.right - thumb.left : thumb.bottom - thumb.top,
        (std::max)(0.0f, bar.content_extent - bar.viewport_extent), horizontal ? x : y, grab_);
    (horizontal ? horizontal_ : scroll_) = offset;
    Clamp();
    return {true};
}
ExtensionDataView::Event ExtensionDataView::PointerUp() {
    const bool handled = drag_axis_ != 0;
    drag_axis_ = 0;
    return {handled};
}
ExtensionDataView::Event ExtensionDataView::DoubleClick(float x, float y) { return Activate(HitRow(x, y)); }
bool ExtensionDataView::Wheel(int delta, bool horizontal) {
    const float before = horizontal ? horizontal_ : scroll_;
    (horizontal ? horizontal_ : scroll_) -= static_cast<float>(delta) / WHEEL_DELTA * RowHeight() * 3;
    Clamp();
    return before != (horizontal ? horizontal_ : scroll_);
}
ExtensionDataView::Event ExtensionDataView::KeyDown(WPARAM key) {
    int index = -1;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].data->id == selected_)
            index = static_cast<int>(i);
    if (key == VK_RETURN || key == VK_SPACE)
        return Activate(index);
    if ((key == VK_LEFT || key == VK_RIGHT) && node_.type == extensions::UiNodeType::Tree && index >= 0) {
        const auto &row = rows_[index];
        if (key == VK_RIGHT && !row.data->children.empty()) {
            if (collapsed_.erase(row.data->id)) {
                Rebuild();
                return {true};
            }
            for (int i = index + 1; static_cast<size_t>(i) < rows_.size() && rows_[i].depth > row.depth; ++i)
                if (rows_[i].data->enabled)
                    return SelectIndex(i);
            return {true};
        }
        if (key == VK_LEFT) {
            if (!row.data->children.empty() && !collapsed_.contains(row.data->id)) {
                collapsed_.insert(row.data->id);
                Rebuild();
                return {true};
            }
            for (int i = index - 1; i >= 0; --i)
                if (rows_[i].depth < row.depth)
                    return SelectIndex(i);
        }
        return {true};
    }
    const int page = (std::max)(1, static_cast<int>((Body().bottom - Body().top) / RowHeight()));
    int next = index;
    int direction = 1;
    switch (key) {
    case VK_HOME:
        next = 0;
        break;
    case VK_END:
        next = static_cast<int>(rows_.size()) - 1;
        direction = -1;
        break;
    case VK_UP:
        next = index - 1;
        direction = -1;
        break;
    case VK_DOWN:
        next = index + 1;
        break;
    case VK_PRIOR:
        next = index - page;
        direction = -1;
        break;
    case VK_NEXT:
        next = index + page;
        break;
    default:
        return {};
    }
    if (rows_.empty())
        return {true};
    next = std::clamp(next, 0, static_cast<int>(rows_.size()) - 1);
    while (next >= 0 && static_cast<size_t>(next) < rows_.size() && !rows_[next].data->enabled)
        next += direction;
    return SelectIndex(next);
}
void ExtensionDataView::Draw(fluent::Painter &painter, Compositor &compositor, const Theme &theme,
                             const fluent::ControlState &state) {
    drawn_rows_ = 0;
    auto *dc = compositor.Dc();
    if (!dc || bounds_.right <= bounds_.left || bounds_.bottom <= bounds_.top)
        return;
    const auto body = Body();
    dc->PushAxisAlignedClip(body, D2D1_ANTIALIAS_MODE_ALIASED);
    const size_t first = static_cast<size_t>(scroll_ / RowHeight());
    for (size_t index = first; index < rows_.size(); ++index) {
        const float top = body.top + static_cast<float>(index) * RowHeight() - scroll_;
        if (top >= body.bottom)
            break;
        ++drawn_rows_;
        const auto &row = rows_[index];
        fluent::ListRowSpec spec;
        spec.bounds = D2D1::RectF(body.left, top, body.right, top + RowHeight());
        spec.state = state;
        spec.state.enabled = state.enabled && row.data->enabled;
        spec.state.selected = selected_ == row.data->id;
        spec.state.keyboard_focus = state.keyboard_focus && spec.state.selected;
        spec.state.hovered = false;
        painter.DrawListRowBackground(spec);
        const auto color = spec.state.enabled ? theme.text : theme.text_secondary;
        if (node_.type == extensions::UiNodeType::DataTable) {
            float left = body.left - horizontal_;
            for (size_t column = 0; column < node_.data_columns.size(); ++column) {
                const float right = left + node_.data_columns[column].width * scale_;
                if (right > body.left && left < body.right) {
                    const auto rect = D2D1::RectF(left + 8 * scale_, top, right - 8 * scale_, top + RowHeight());
                    painter.DrawText(column < row.cells.size() ? row.cells[column] : L"", rect, compositor.TextFormat(),
                                     color);
                }
                left = right;
            }
        } else {
            float left = body.left + (8 + static_cast<float>(row.depth) * 20) * scale_;
            if (node_.type == extensions::UiNodeType::Tree) {
                if (!row.data->children.empty())
                    painter.DrawGlyph(collapsed_.contains(row.data->id) ? L"\xE76C" : L"\xE70D",
                                      D2D1::RectF(left, top, left + 16 * scale_, top + RowHeight()), color);
                left += 20 * scale_;
            }
            if (!row.glyph.empty()) {
                painter.DrawGlyph(row.glyph, D2D1::RectF(left, top, left + 20 * scale_, top + RowHeight()), color);
                left += 28 * scale_;
            }
            painter.DrawText(row.text, D2D1::RectF(left, top, body.right - 8 * scale_, top + RowHeight()),
                             compositor.TextFormat(), color);
        }
    }
    dc->PopAxisAlignedClip();
    if (HeaderHeight() > 0) {
        dc->PushAxisAlignedClip(D2D1::RectF(bounds_.left, bounds_.top, body.right, body.top),
                                D2D1_ANTIALIAS_MODE_ALIASED);
        float left = bounds_.left - horizontal_;
        for (size_t i = 0; i < node_.data_columns.size(); ++i) {
            const auto &column = node_.data_columns[i];
            const auto text = WideData(column.text);
            fluent::ColumnHeaderSpec spec;
            spec.bounds = D2D1::RectF(left, bounds_.top, left + column.width * scale_, body.top);
            spec.text = text;
            spec.state.enabled = state.enabled;
            if (sort_column_ == static_cast<int>(i))
                spec.sort = sort_descending_ ? fluent::SortDirection::Descending : fluent::SortDirection::Ascending;
            painter.DrawColumnHeader(spec);
            left = spec.bounds.right;
        }
        dc->PopAxisAlignedClip();
    }
    for (bool horizontal : {false, true}) {
        auto bar = Bar(horizontal);
        bar.enabled = state.enabled;
        if (bar.content_extent > bar.viewport_extent)
            painter.DrawScrollbar(bar);
    }
}
} // namespace pulse::ui
