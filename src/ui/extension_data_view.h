#pragma once
#include "../app/extensions/ui_tree.h"
#include "extension_data_accessibility.h"
#include "fluent_components.h"
#include <set>
#include <string>
#include <vector>

namespace pulse::ui {
// Virtualized rows use the containing tree's compositor and input proxy.
class ExtensionDataView {
  public:
    struct Event {
        bool handled = false;
        bool selection_changed = false;
        std::string command;
    };
    void Configure(const extensions::UiNode &node);
    extensions::UiNode SnapshotNode() const;
    void CopyViewState(const ExtensionDataView &source);
    void UpdateNode(const extensions::UiNode &node);
    void UpdateRows(const std::vector<extensions::UiDataRow> &rows);
    bool ContainsRow(const std::string &id) const;
    void SetViewport(D2D1_RECT_F bounds, float scale);
    bool SetSelection(const std::string &id);
    const std::string &Selection() const { return selected_; }
    std::wstring SelectedText() const;
    std::vector<DataAccessibleRow> AccessibilityRows() const;
    Event PointerDown(float x, float y);
    Event PointerMove(float x, float y);
    Event PointerUp();
    Event DoubleClick(float x, float y);
    Event KeyDown(WPARAM key);
    bool Wheel(int delta, bool horizontal = false);
    void Draw(fluent::Painter &painter, Compositor &compositor, const Theme &theme, const fluent::ControlState &state);
    size_t VisibleRowCount() const { return rows_.size(); }
    size_t LastDrawnRowCount() const { return drawn_rows_; }
    float ScrollOffset() const { return scroll_; }
    bool Dragging() const { return drag_axis_ != 0; }

  private:
    struct Row {
        const extensions::UiDataRow *data = nullptr;
        size_t depth = 0;
        std::wstring text, glyph;
        std::vector<std::wstring> cells;
    };
    extensions::UiNode node_;
    std::vector<Row> rows_;
    std::set<std::string> collapsed_;
    std::string selected_;
    D2D1_RECT_F bounds_{};
    float scale_ = 1, scroll_ = 0, horizontal_ = 0, grab_ = 0;
    int drag_axis_ = 0, sort_column_ = -1;
    bool sort_descending_ = false;
    size_t drawn_rows_ = 0;
    void Rebuild();
    void Clamp();
    void EnsureVisible();
    float RowHeight() const;
    float HeaderHeight() const;
    float ContentWidth() const;
    D2D1_RECT_F Body() const;
    fluent::ScrollbarSpec Bar(bool horizontal) const;
    int HitRow(float x, float y) const;
    Event SelectIndex(int index);
    Event Activate(int index);
};
} // namespace pulse::ui
