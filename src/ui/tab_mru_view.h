#pragma once
// tab_mru_view.h - The Ctrl+Tab "最近使用的标签" panel (ui modes step 7),
// after the mockup's .mru: a 520 DIP flyout centred on the window, a title
// row with the count, one 44 DIP row per tab (folder, name, parent folder,
// 「当前」 on the first) and a key hint footer. The selected row has the
// selection fill and an accent ring that springs from row to row.
// The app (app/window_switcher_ui.cpp) owns the selection and the input;
// this class only lays out, draws and hit-tests.
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui_motion.h"

namespace pulse::ui {

struct Theme;

struct TabMruRow {
    std::wstring name;
    std::wstring parent;  // folder holding it, or empty
};

// A footer hint: text runs, the `kbd` ones drawn as key caps.
struct TabMruHintPart {
    bool kbd = false;
    std::wstring text;
};

struct TabMruData {
    std::vector<TabMruRow> rows;  // most recent first; row 0 is the current tab
    int selected = 1;
    std::wstring title;
    std::wstring count;
    std::wstring current;
    std::vector<std::vector<TabMruHintPart>> hints;
};

class TabMruView {
public:
    ~TabMruView();
    void Show(TabMruData data, const D2D1_RECT_F& client, float scale, HWND host = nullptr);
    void Hide() noexcept;
    bool IsOpen() const noexcept { return open_ || closing_; }
    bool SetHover(int row) noexcept;
    int HitRow(float x, float y) const noexcept;
    bool Contains(float x, float y) const noexcept;
    void Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, IDWriteTextFormat* base_format, const Theme& theme);

private:
    void Layout();
    void EnsureFormats(IDWriteFactory* dwrite, IDWriteTextFormat* base_format);
    void ReleaseFormats();
    void RequestFrame() const noexcept;

    TabMruData data_;
    D2D1_RECT_F client_{};
    D2D1_RECT_F panel_{};
    std::vector<D2D1_RECT_F> row_rects_;
    float scale_ = 1.0f;
    bool open_ = false;
    IDWriteTextFormat* f_title_ = nullptr;
    IDWriteTextFormat* f_name_ = nullptr;
    IDWriteTextFormat* f_small_ = nullptr;
    IDWriteTextFormat* f_hint_ = nullptr;
    float format_scale_ = 0.0f;

    HWND host_ = nullptr;
    bool closing_ = false;
    bool animations_ = true;
    int hover_ = -1;
    uint64_t open_at_ = 0;
    uint64_t close_at_ = 0;
    uint64_t frame_ = 0;
    uint64_t context_ = 0;
    motion::SpringRectMotion ring_;
};

}  // namespace pulse::ui
