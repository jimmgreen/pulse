// fluent_menu.h — Fluent context menu / dropdown (ui.md §4 右键菜单/下拉, §6 动效).
//
// Rendering: an independent WS_EX_LAYERED popup with per-pixel alpha. The
// menu surface and items are drawn with ui::fluent::Painter (DrawMenuSurface
// / DrawMenuItem) into an offscreen D2D bitmap on the *main* compositor's
// device, plus a CLSID_D2D1Shadow drop shadow, then pushed to the window via
// UpdateLayeredWindow. Rounded 8px corners come from the alpha channel.
//
// Behavior: TrackPopup runs a nested modal loop (like TrackPopupMenu):
// hover tracking, keyboard nav (Up/Down/Enter/Esc), click-outside dismiss,
// 120ms OutQuad fade-in + 8px slide (motion::MenuTravelDip), 80ms fade-out.
// The same component serves the right-click menu and the toolbar "新建▾"
// dropdown.
//
// FluentMenuModel is the windowless layout/hit-test half; the console
// self-test drives it directly with a standalone DWrite factory.
#pragma once
#include "fluent_components.h"

#include <dwrite_3.h>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct FluentMenuSwatch {
    int command = 0;
    D2D1_COLOR_F color{};
    bool checked = false;
    bool mixed = false;
    // Non-empty => icon button (Segoe Fluent Icons codepoint) instead of a
    // tag color dot: wider hit target, hover pill, theme text color.
    std::wstring glyph;
};

struct FluentMenuItem {
    int command = 0;              // app-defined verb id; 0 is never returned
    std::wstring text;
    std::wstring glyph;           // Segoe Fluent Icons codepoint string
    std::wstring shortcut;        // right-aligned caption, e.g. L"Ctrl+Shift+C"
    std::wstring badge_text;      // right-aligned Fluent badge (takes priority over shortcut)
    bool enabled = true;
    bool separator_after = false;
    bool has_swatch = false;
    bool checked = false;
    bool mixed = false;
    bool radio = false;            // selected single-choice item: filled dot, never a checkmark
    bool radio_group = false;      // reserve a leading selection-dot column (left of the icon)
    fluent::MenuPictogram pictogram = fluent::MenuPictogram::None;
    D2D1_COLOR_F swatch_color{};
    std::vector<FluentMenuSwatch> quick_swatches;
    // Non-empty => the row is a one-level flyout header (software-owned
    // Explorer submenu). Hover/click/→ opens the flyout; the row itself never
    // returns a command. Children may not nest further.
    std::vector<FluentMenuItem> children;
    float glyph_scale = 1.0f;      // Per-item visual scale inside the fixed icon slot.
    bool shortcut_inline = false; // Search paths follow a shared, compact filename column.
};

// Windowless menu layout + hit-testing (theme.row_menu = 36 DIP rows).
class FluentMenuModel {
public:
    void SetItems(std::vector<FluentMenuItem> items);
    // Measures text; dwrite may be nullptr (falls back to an estimate).
    void Layout(IDWriteFactory2* dwrite, float scale, float min_width_px = 0);

    int WidthPx() const { return width_; }
    int HeightPx() const { return height_; }
    float InlineLabelWidthPx() const { return inline_label_width_; }
    float RowHeightPx() const { return row_h_; }
    int Count() const { return (int)items_.size(); }
    const FluentMenuItem* At(int i) const;
    // Copy command ids from src when display text/structure matches. No layout.
    bool PatchCommands(const std::vector<FluentMenuItem>& src);
    // Content y (px) of row i's top edge.
    float RowTopPx(int i) const;
    int HitTestRow(float y_px) const;                 // -1 = not on a row
    int NextEnabled(int from, int dir) const;         // wraps; -1 if none
    int FirstEnabled() const { return NextEnabled(-1, 1); }

private:
    std::vector<FluentMenuItem> items_;
    float scale_ = 1.0f;
    float row_h_ = 32.0f;
    float pad_v_ = 4.0f;      // surface inner padding (DIP*scale)
    float sep_h_ = 5.0f;      // separator slot height (line + gaps)
    float inline_label_width_ = 0.0f;
    int width_ = 0;
    int height_ = 0;
};

// The popup window. One instance per app, reused across invocations.
class FluentMenu {
    friend struct FluentMenuTestPeer;
public:
    FluentMenu() = default;
    ~FluentMenu();
    FluentMenu(const FluentMenu&) = delete;
    FluentMenu& operator=(const FluentMenu&) = delete;

    // owner: main window. compositor supplies the D2D device + DWrite factory
    // (drawing happens on the UI thread between its own BeginDraw/EndDraw).
    bool Create(HWND owner, Compositor* compositor, float scale);
    void SetTheme(bool dark, D2D1_COLOR_F accent);

    // Modal; returns the invoked command id, or 0 when dismissed.
    // If filter is set, typing rebuilds the item list (Ctrl+K command palette).
    using FilterFn = std::function<std::vector<FluentMenuItem>(const std::wstring& query)>;
    int TrackPopup(POINT screen_pt, std::vector<FluentMenuItem> items,
                   FilterFn filter = nullptr, bool top_center = false);

    // Renders the menu to a PNG without showing it (GUI verification helper).
    bool SaveDebugSnapshot(const wchar_t* png_path, std::vector<FluentMenuItem> items);

    bool IsOpen() const { return open_; }
    void Dismiss(); // immediate (no animation); safe anytime
    // Safe from the owner window while TrackPopup's modal loop is running
    // (e.g. async index hits arrived). No-op if the menu is closed.
    void RequestFilterRefresh();
    // Swap command ids while the menu is open if the visible structure matches.
    // Never grows or shrinks an open menu (Explorer COM arriving late must not
    // restyle the popup). Returns true when ids were patched.
    bool ReplaceItems(std::vector<FluentMenuItem> items);
    void SetFilterPlaceholder(std::wstring text) { filter_placeholder_ = std::move(text); }
    const std::wstring& LastFilterQuery() const { return filter_query_; }
    // True when the last TrackPopup closed because Enter was pressed with no
    // highlighted command (omnibar: commit the typed query).
    bool LastFilterCommitted() const { return filter_committed_; }
    // Prefill the filter edit on the next TrackPopup (used as a rename field).
    void SetInitialFilterText(std::wstring text) { initial_filter_text_ = std::move(text); }
    void SetSelectAllOnOpen(bool select_all) { select_all_on_open_ = select_all; }
    void SetHoverFirstOnOpen(bool hover_first) { hover_first_on_open_ = hover_first; }
    // Align the next TrackPopup to this screen rect (address bar). The filter
    // field covers the rect; results hang below. Cleared after the popup.
    void SetAnchorRect(RECT screen_rc) { anchor_rect_ = screen_rc; anchor_to_rect_ = true; }
    // Override the filter-mode minimum width (dips) for the next TrackPopup;
    // <= 0 restores the palette default. Reset after each popup.
    void SetFilterMinWidth(float dips) { filter_min_width_ = dips; }

    static constexpr int kShadowMargin = 20; // px of transparent border around the card

private:
    // ULW staging buffer (GDI DIB); the main menu and the flyout each own one.
    struct Surface {
        HDC mem_dc = nullptr;
        HBITMAP dib = nullptr;
        void* bits = nullptr;
        int w = 0;
        int h = 0;
    };

    bool EnsureWindow();
    bool EnsureFilterEdit();
    void PlaceFilterEdit(int y_offset_px);
    void HideFilterEdit();
    void SyncFilterFromEdit();
    bool IsPaletteHwnd(HWND hwnd) const;
    bool RenderSurface(const FluentMenuModel& model, int hover_row, int hover_swatch,
                       float header_px, bool draw_filter_field, Surface& s);
    bool Render();                 // model_ -> dib_ (pixels), current hover state
    void Present(BYTE alpha, int y_offset_px);
    // One-level flyout for FluentMenuItem::children.
    bool EnsureSubWindow();
    void OpenSubmenu(int row);
    void CloseSubmenu();
    void OnSubMouse(POINT client_pt, bool button_up);
    bool RenderSub();
    void PresentSub(BYTE alpha);
    void HideSubWindow();
    void LayoutWindow(POINT screen_pt);
    void RefreshFilter();
    int RunModalLoop();
    void OnMouse(POINT client_pt, bool button_up);
    void UpdateHover(int row, int swatch = -1);
    int HitTestSwatch(int row, float client_x) const;
    int HitTestSwatch(const FluentMenuModel& model, int row, float client_x) const;
    int InvokeRow(int row);        // returns command or 0
    int InvokeAt(int row, float client_x) const;
    int InvokeAt(const FluentMenuModel& model, int row, float client_x) const;
    float FilterHeaderPx() const;
    bool AppendFilterChar(wchar_t ch);
    bool HandleFilterKey(UINT msg, WPARAM wParam);
    void PasteFilterText();

    static LRESULT CALLBACK MenuWndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK FilterEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    Compositor* compositor_ = nullptr;
    fluent::Painter painter_;
    float scale_ = 1.0f;
    bool dark_ = false;
    D2D1_COLOR_F accent_{};

    FluentMenuModel model_;
    FilterFn filter_fn_;
    std::wstring filter_query_;
    std::wstring filter_placeholder_;
    std::wstring initial_filter_text_;   // prefilled into the edit on open
    float filter_min_width_ = 0.0f;      // dips; <= 0 = palette default (440)
    bool select_all_on_open_ = true;
    bool hover_first_on_open_ = true;
    bool filter_committed_ = false;
    RECT anchor_rect_{};
    bool anchor_to_rect_ = false;
    POINT popup_pt_{};
    bool top_center_ = false;
    HWND edit_ = nullptr;
    HFONT edit_font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    int present_offset_ = 0;
    int hover_row_ = -1;
    int hover_swatch_ = -1;
    bool open_ = false;
    bool animating_out_ = false;
    int result_ = 0;
    int base_x_ = 0;               // window position before slide animation
    int base_y_ = 0;
    std::chrono::steady_clock::time_point anim_start_;

    Surface surf_;                 // main menu staging

    // Flyout (one level, FluentMenuItem::children).
    HWND sub_hwnd_ = nullptr;
    Surface sub_surf_;
    FluentMenuModel sub_model_;
    int sub_parent_row_ = -1;      // row in model_ whose children are shown
    int sub_hover_ = -1;
    int sub_hover_swatch_ = -1;
    int sub_pending_row_ = -1;     // hover-to-open delay target
    std::chrono::steady_clock::time_point sub_pending_since_;
    int sub_x_ = 0;                // flyout window origin (screen px)
    int sub_y_ = 0;
};

} // namespace pulse::ui
