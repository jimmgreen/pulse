#pragma once
// window_overview_view.h - The window overview (multi-window M7e,
// Ctrl+Shift+O) drawn over the window it was opened in: a header with the
// "merge all" and "done" buttons, one card per Pulse window with its color,
// name and tabs, and a dashed "new window" card. A dragged tab follows the
// cursor and the card it would land on is highlighted. The app
// (app/window_overview_ui.h) owns the state and forwards input while it is
// open; this class only lays out, draws, scrolls and hit-tests.
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {

struct Theme;

struct WindowOverviewChip {
    std::wstring title;
    std::wstring path;  // its folder, shown by its end
    size_t tab = 0;     // index in the window's tabs
    bool active = false;
};

struct WindowOverviewCard {
    std::wstring name;        // window name (M7c)
    uint32_t rgb = 0x94A3B8;  // window color
    std::wstring meta;        // "3 个标签页 · 已最小化"
    std::vector<WindowOverviewChip> chips;
    std::wstring more;        // "还有 4 个标签页" when not every tab has a chip
    bool current = false;     // the window the overview is in
};

struct WindowOverviewData {
    std::wstring title, hint, notice;  // notice replaces the hint (accent)
    std::wstring merge_all, done, switch_to, current, new_window, arrange;
    std::vector<std::wstring> arrangements;
    bool can_merge_all = false;
    std::vector<WindowOverviewCard> cards;
};

enum class OverviewHitKind { None, Outside, Panel, Caption, Card, Chip, Switch, More, NewWindow, MergeAll, Done, Arrange };

struct OverviewHit {
    OverviewHitKind kind = OverviewHitKind::None;
    int card = -1;
    int chip = -1;  // index into that card's chips
    bool operator==(const OverviewHit&) const = default;
};

class WindowOverviewView {
public:
    ~WindowOverviewView();
    // Shows or refreshes it (the scroll position stays); client = window
    // client rect (px).
    void Show(WindowOverviewData data, const D2D1_RECT_F& client, float scale);
    void Hide() noexcept;
    bool IsOpen() const noexcept { return open_; }
    OverviewHit HitTest(float x, float y) const noexcept;
    // Hover highlight; true when it changed.
    bool SetHover(const OverviewHit& hit) noexcept;
    // A tab being dragged: its card and chip, the cursor (px) and what it is
    // over. EndDrag drops the picture.
    void SetDrag(int card, int chip, float x, float y, const OverviewHit& over) noexcept;
    void EndDrag() noexcept;
    // Scrolls the cards by `dip` (positive: down); true when they moved.
    bool Scroll(float dip) noexcept;
    void Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, IDWriteTextFormat* base_format, const Theme& theme);

private:
    struct CardRects {
        D2D1_RECT_F card{}, switch_button{}, more{};
        std::vector<D2D1_RECT_F> chips;
    };
    void Layout();
    void EnsureFormats(IDWriteFactory* dwrite, IDWriteTextFormat* base_format);
    void ReleaseFormats();

    WindowOverviewData data_;
    D2D1_RECT_F client_{};
    D2D1_RECT_F panel_{};
    D2D1_RECT_F area_{};  // where the cards scroll
    D2D1_RECT_F done_button_{};
    D2D1_RECT_F new_card_{};
    std::vector<D2D1_RECT_F> arrange_buttons_;
    bool arrange_in_header_ = false;
    std::vector<CardRects> rects_;
    float scale_ = 1.0f;
    float scroll_ = 0.0f;  // px
    float content_h_ = 0.0f;
    bool open_ = false;
    bool laid_out_ = false;
    OverviewHit hover_;
    bool dragging_ = false;
    int drag_card_ = -1;
    int drag_chip_ = -1;
    float drag_x_ = 0.0f, drag_y_ = 0.0f;
    OverviewHit drag_over_;
    IDWriteFactory* dwrite_ = nullptr;  // the factory of the last Draw, for measuring
    IDWriteTextFormat* f_title_ = nullptr;
    IDWriteTextFormat* f_name_ = nullptr;
    IDWriteTextFormat* f_small_ = nullptr;
    IDWriteTextFormat* f_chip_ = nullptr;
    IDWriteTextFormat* f_path_ = nullptr;  // right-aligned
    IDWriteTextFormat* f_center_ = nullptr;
    IDWriteTextFormat* f_plus_ = nullptr;
    float format_scale_ = 0.0f;
};

}  // namespace pulse::ui
