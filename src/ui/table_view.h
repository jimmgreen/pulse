// table_view.h — Quick Look grid for CSV/TSV/XLSX: the preview host's table
// payload (preview_host/table_document.h) drawn as a virtualized grid with a
// pinned header row and row-number column, cell selection, find highlights
// and, for workbooks, sheet tabs.
#pragma once
#include "search_highlights.h"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <memory>
#include <vector>

#include "FluentTokens.h"

namespace pulse::ui {

class TableView {
    friend struct PreviewBufferLifetimeTest;
public:
    // Parses the payload; a repeat of the same payload keeps sheet, scroll and
    // selection. Returns false for anything that is not a table payload.
    bool SetPayload(const std::shared_ptr<const std::wstring>& payload);
    bool SetPayload(const std::wstring& payload);
    void Clear();
    bool HasData() const noexcept { return !sheets_.empty(); }
    bool IsSpreadsheet() const noexcept { return spreadsheet_; }
    size_t SelectedSheetIndex() const noexcept { return sheet_; }
    bool IsSheetRequestPending() const noexcept { return awaiting_sheet_; }
    // A sheet switch queues at most one request; rapid switches replace it.
    // The owner sends this index to the preview host and applies its response
    // with SetPayload. Clear() must be called when changing the source file.
    bool TakePendingSheetRequest(uint32_t& index);
    void FailPendingSheetRequest(uint32_t index);
    const std::wstring& Source() const noexcept { return source_; }
    // Cells of the current sheet joined by tabs, rows by newlines: what find
    // works on. Offsets below index this text.
    uint64_t TextRevision() const noexcept { return text_revision_; }
    const std::wstring& PlainText() const noexcept { return plain_; }
    // CSV status pill segments ("CSV", "逗号 · UTF-8", "N 行 × M 列").
    std::vector<std::wstring> StatusParts(uint32_t encoding) const;

    using Highlight = SearchHighlight;
    void Draw(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& rect,
              const Theme& theme, bool dark, float scale, const D2D1_COLOR_F& background,
              SearchHighlights matches);

    bool Scroll(float wheel_steps, bool horizontal);
    // Arrows move the selected cell (Shift extends); paging keys scroll.
    // Returns false for keys the window should handle (Left/Right with no
    // selection step through files).
    bool Key(UINT vk, bool shift, bool ctrl);
    void Reveal(uint32_t offset);

    bool MouseDown(float x, float y, bool shift);  // true when handled
    bool MouseMove(float x, float y);              // while dragging; true to repaint
    void MouseUp();
    bool Dragging() const noexcept { return drag_ != Drag::None; }
    bool Hover(float x, float y);                  // true to repaint
    bool Leave();
    // Hovering a clipped cell whose full text is not shown yet.
    bool TipArmed() const noexcept { return tip_row_ >= 0 && !tip_shown_; }
    bool ShowTip();

    // Workbook tabs drawn by the window in its footer instead of under the grid.
    void SetFooterTabs(bool on) noexcept { footer_tabs_ = on; }
    // Draws the tabs into rect; returns the right edge actually used.
    float DrawTabsIn(ID2D1DeviceContext* dc, IDWriteFactory2* factory, const D2D1_RECT_F& rect,
                     const Theme& theme, bool dark, bool status);
    bool TabClick(float x, float y);  // ‹ › and tabs; true when it hit one

    bool HasSelection() const noexcept { return sel_valid_; }
    void SelectAll();
    bool ClearSelection();
    // Selection as TSV (the whole sheet when nothing is selected).
    std::wstring SelectionText() const;

private:
    friend struct TableViewTestAccess;
    struct Sheet {
        std::wstring name, detail;
        bool truncated = false, header = false;
        size_t cols = 0;
        std::vector<std::vector<std::wstring>> cells;  // display text, header row included
        std::vector<std::vector<unsigned>> fills;
        std::vector<std::vector<uint8_t>> flags;       // kBold / kNumeric per cell
        std::vector<float> col_x;                      // prefix sums (device px), cols + 1
        float num_w = 0.0f;                            // row-number column width
        float measured_scale = 0.0f;
        float tab_width = -1.0f, selected_tab_width = -1.0f;
        float sx = 0.0f, sy = 0.0f;                    // scroll
        std::vector<uint8_t> col_types;                // inferred per column (ColumnType)
    };
    struct Geometry {
        D2D1_RECT_F grid{}, body{}, tabs{};
        float row_h = 0, head_h = 0, pad = 0, content_w = 0, content_h = 0;
        float max_sx = 0, max_sy = 0;
    };
    enum class Drag { None, Cells, VThumb, HThumb };

    Sheet* Current() noexcept { return sheets_.empty() ? nullptr : &sheets_[sheet_]; }
    const Sheet* Current() const noexcept { return sheets_.empty() ? nullptr : &sheets_[sheet_]; }
    size_t HeaderRows() const noexcept;  // 1 when row 0 is the pinned header
    size_t BodyRows() const noexcept;
    void BuildPlain();
    bool EnsureFormats(IDWriteFactory2* factory, float scale);
    void EnsureWidths(Sheet& sheet);
    float Estimate(const std::wstring& text) const;
    Geometry Measure() const;
    void Clamp();
    void SwitchSheet(size_t index);
    // Body cell under a point (-1 when outside); row in body rows.
    bool CellAt(float x, float y, int& row, int& col, bool clamp) const;
    void EnsureVisible(int row, int col);
    bool OffsetToCell(uint32_t offset, size_t& row, size_t& col, uint32_t& in_cell) const;
    bool Clipped(int row, int col);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> CellLayout(const std::wstring& text, bool bold,
                                                         bool right, float width) const;

    uint64_t text_revision_ = 0;
    bool SetPayloadImpl(const std::wstring& payload,
                        std::shared_ptr<const std::wstring> owner);
    const std::wstring& Payload() const noexcept { return shared_payload_ ? *shared_payload_ : payload_; }
    void StorePayload(const std::wstring& value, std::shared_ptr<const std::wstring> owner) {
        if (owner) {
            shared_payload_ = std::move(owner);
            std::wstring{}.swap(payload_);
        } else {
            payload_ = value;
            shared_payload_.reset();
        }
    }
    std::shared_ptr<const std::wstring> shared_payload_;
    std::weak_ptr<const std::wstring> payload_identity_;
    std::wstring payload_, source_, plain_;
    std::vector<Sheet> sheets_;
    std::vector<uint32_t> row_offsets_;  // plain_ offset of each cells[] row
    size_t sheet_ = 0;
    size_t first_visible_sheet_ = 0;
    size_t response_sheet_ = 0;
    size_t total_sheets_ = 0, loaded_sheets_ = 0;
    bool spreadsheet_ = false;
    bool footer_tabs_ = false;
    bool lazy_sheets_ = false, awaiting_sheet_ = false, pending_sheet_request_ = false;

    // Selection in body rows / columns (anchor and focus corners).
    bool sel_valid_ = false;
    int sel_r0_ = 0, sel_c0_ = 0, sel_r1_ = 0, sel_c1_ = 0;

    Drag drag_ = Drag::None;
    float drag_origin_ = 0.0f, drag_scroll_ = 0.0f;
    float drag_x_ = 0.0f, drag_y_ = 0.0f;

    int hover_tab_ = -1;
    int hover_row_ = -1, hover_col_ = -1;  // body cell under the pointer (crosshair)
    int tip_row_ = -1, tip_col_ = -1;
    bool tip_shown_ = false;
    float tip_x_ = 0.0f, tip_y_ = 0.0f;
    std::vector<D2D1_RECT_F> tab_rects_;
    D2D1_RECT_F previous_sheet_{}, next_sheet_{};
    D2D1_RECT_F vthumb_{}, hthumb_{};

    D2D1_RECT_F view_{};
    float scale_ = 1.0f;
    IDWriteFactory2* factory_ = nullptr;
    float format_scale_ = 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_, text_right_, bold_, bold_right_, small_, tip_;
    float digit_w_ = 7.0f, cjk_w_ = 12.5f, latin_w_ = 6.5f, upper_w_ = 8.0f;
    ID2D1DeviceContext* brush_owner_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
};

}  // namespace pulse::ui
