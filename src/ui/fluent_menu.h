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
// open: the card grows from the anchor point (0.94 -> 1, Pop spring) while it
// fades in over 90ms and the rows follow with a 9ms stagger (capped); the
// hover plate glides between rows (Plate spring); flyouts fade/slide in and
// glide to a new header instead of closing and reopening; close shrinks to
// 0.97 while fading from the alpha on screen. Everything is instant when
// Windows animation effects are off. Frames are paced by DwmFlush.
// The same component serves the right-click menu and the toolbar "新建▾"
// dropdown.
//
// FluentMenuModel is the windowless layout/hit-test half; the console
// self-test drives it directly with a standalone DWrite factory.
#pragma once
#include "fluent_components.h"
#include "ui_motion.h"
#include "../common/menu_icon.h"

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
    wchar_t mnemonic = 0;          // keyboard access key (case-insensitive), 0 = none
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
    bool toggle = false;
    bool secondary = false;
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
    bool accent_glyph = false;     // Glyph drawn in the accent colour (assistant hand-off).
    bool shortcut_inline = false; // Search paths follow a shared, compact filename column.
    int trailing_command = 0;     // Optional hover-revealed close action at the right edge.
    std::wstring tooltip;
    // Keyboard access key like a native menu's "&X" (case-insensitive). 0 falls
    // back to a trailing "(X)" in text, which Explorer's CJK labels keep.
    wchar_t mnemonic = 0;
    // App icon of an Explorer row (handler bitmap / registry verb icon), drawn
    // in place of the glyph like Explorer does. Does not affect layout.
    common::MenuIconRef icon;
    // Command palette: a section caption (a short row, never selectable) and
    // the label characters the query matched, drawn semibold.
    bool heading = false;
    uint16_t match_start = 0;
    uint16_t match_length = 0;
};

// Access key of a row: explicit mnemonic, else trailing "(X)" / "（X）" in text
// (ignoring a final "..." or "…"). Upper-cased; 0 when the row has none.
wchar_t MenuItemMnemonic(const FluentMenuItem& item);
// One access-key match: a row (swatch == -1) or one of its quick swatches.
struct MenuMnemonicTarget {
    int row = -1;
    int swatch = -1;
};
// Enabled rows / swatches whose access key equals key_a or key_b
// (case-insensitive), in visual order. Rows qualify when they carry a command
// or a flyout; command-less strips only through their swatches.
std::vector<MenuMnemonicTarget> FindMenuMnemonic(const std::vector<FluentMenuItem>& items,
                                                 wchar_t key_a, wchar_t key_b = 0);
// Several rows sharing a key: index of the first match after (row, swatch),
// wrapping to 0. Native menus cycle the highlight this way without invoking.
size_t NextMenuMnemonicTarget(const std::vector<MenuMnemonicTarget>& targets, int row,
                              int swatch);

// Windowless menu layout + hit-testing (theme.row_menu = 36 DIP rows by default).
class FluentMenuModel {
public:
    void SetItems(std::vector<FluentMenuItem> items);
    // Row height in DIPs (clamped to kMinRowDip..kMaxRowDip); applies on the next Layout.
    void SetRowHeightDip(float dip);
    static constexpr float kDefaultRowDip = 36.0f;
    static constexpr float kMinRowDip = 26.0f;
    // Label size; matches Painter::NavFormat() and the file list's names.
    static constexpr float kLabelDip = 13.0f;
    static constexpr float kMaxRowDip = 40.0f;
    // Measures text; dwrite may be nullptr (falls back to an estimate).
    void Layout(IDWriteFactory2* dwrite, float scale, float min_width_px = 0);

    int WidthPx() const { return width_; }
    int HeightPx() const { return height_; }
    float InlineLabelWidthPx() const { return inline_label_width_; }
    float RowHeightPx() const { return row_h_; }
    float RowHeightPx(int i) const;   // row i's height: heading rows are shorter
    int Count() const { return (int)items_.size(); }
    const std::vector<FluentMenuItem>& Items() const { return items_; }
    const FluentMenuItem* At(int i) const;
    // True when row i's label is wider than the menu and gets ellipsized.
    bool Truncated(int i) const;
    // Copy command ids from src when display text/structure matches. No layout.
    bool PatchCommands(const std::vector<FluentMenuItem>& src);
    // Takes src when it has as many rows with the same separators (labels,
    // states and flyouts may differ): the height stays, the width is kept and
    // a longer label ellipsizes. False, changing nothing, for any other shape.
    bool PatchInPlace(std::vector<FluentMenuItem> src, IDWriteFactory2* dwrite);
    // Takes src when it is the shown rows (same separators, except that the
    // last may gain one) with more rows after them: every shown row keeps its
    // place, the menu grows downwards and widens only for a longer new label.
    // False, changing nothing, for any other shape.
    bool PatchGrow(std::vector<FluentMenuItem> src, IDWriteFactory2* dwrite);
    // Content y (px) of row i's top edge.
    float RowTopPx(int i) const;
    int HitTestRow(float y_px) const;                 // -1 = not on a row
    int NextEnabled(int from, int dir) const;         // wraps; -1 if none
    int FirstEnabled() const { return NextEnabled(-1, 1); }

private:
    std::vector<FluentMenuItem> items_;
    float scale_ = 1.0f;
    float row_dip_ = kDefaultRowDip;
    float row_h_ = 32.0f;
    float pad_v_ = 4.0f;      // surface inner padding (DIP*scale)
    float sep_h_ = 5.0f;      // separator slot height (line + gaps)
    float heading_h_ = 26.0f; // a heading row (26 DIP)
    float inline_label_width_ = 0.0f;
    int width_ = 0;
    int locked_width_ = 0;    // PatchInPlace: Layout keeps this width
    std::vector<unsigned char> truncated_;
    int height_ = 0;
};

// The popup window. One instance per app, reused across invocations.
class FluentMenu {
    friend struct FluentMenuTestPeer;
#ifdef PULSE_WITH_SELFTEST
    friend struct P1MenuTestPeer;
#endif
public:
    FluentMenu() = default;
    ~FluentMenu();
    FluentMenu(const FluentMenu&) = delete;
    FluentMenu& operator=(const FluentMenu&) = delete;

    // owner: main window. compositor supplies the D2D device + DWrite factory
    // (drawing happens on the UI thread between its own BeginDraw/EndDraw).
    bool Create(HWND owner, Compositor* compositor, float scale);
    void SetTheme(bool dark, D2D1_COLOR_F accent);
    // Row height of this menu and its submenus, in DIPs (follows list density, #27).
    void SetRowHeightDip(float dip);

    // Modal; returns the invoked command id, or 0 when dismissed.
    // If filter is set, typing rebuilds the item list (Ctrl+K command palette).
    using FilterFn = std::function<std::vector<FluentMenuItem>(const std::wstring& query)>;
    int TrackPopup(POINT screen_pt, std::vector<FluentMenuItem> items,
                   FilterFn filter = nullptr, bool top_center = false);
    int TrackDropdown(RECT control_rect, std::vector<FluentMenuItem> items);

    // Renders the menu to a PNG without showing it (GUI verification helper).
    bool SaveDebugSnapshot(const wchar_t* png_path, std::vector<FluentMenuItem> items,
                           int hover_row = -1);

    bool IsOpen() const { return open_; }
    void Dismiss(); // immediate (no animation); safe anytime
    // Safe from the owner window while TrackPopup's modal loop is running
    // (e.g. async index hits arrived). No-op if the menu is closed.
    void RequestFilterRefresh();
    // Patches the open menu in place: same rows and separators, while labels,
    // states and flyouts may change (the context menu maps late rows onto the
    // shown ones first, ReconcileCtxDisplay), or the shown rows plus rows
    // appended below them (PatchGrow). No shown row ever moves, unless the
    // grown menu no longer fits below its top edge and is lifted to stay on
    // screen. Never shrinks an open menu. False, changing nothing, for another
    // shape or when a visible flyout's rows would change.
    bool ReplaceItems(std::vector<FluentMenuItem> items);
    void SetFilterPlaceholder(std::wstring text) { filter_placeholder_ = std::move(text); }
    const std::wstring& LastFilterQuery() const { return filter_query_; }
    // True when the last TrackPopup closed because Enter was pressed with no
    // highlighted command (omnibar: commit the typed query).
    bool LastFilterCommitted() const { return filter_committed_; }
    // Prefill the filter edit on the next TrackPopup (used as a rename field).
    void SetInitialFilterText(std::wstring text) { initial_filter_text_ = std::move(text); }
    // The next TrackPopup draws `lead` (">") at the start of the filter in the
    // accent colour and `hint` after it while nothing else is typed.
    void SetFilterLead(std::wstring lead, std::wstring hint) {
        filter_lead_ = std::move(lead);
        filter_lead_hint_ = std::move(hint);
    }
    void SetSelectAllOnOpen(bool select_all) { select_all_on_open_ = select_all; }
    void SetHoverFirstOnOpen(bool hover_first) { hover_first_on_open_ = hover_first; }
    // Align the next TrackPopup to this screen rect (address bar). The filter
    // field covers the rect; results hang below. Cleared after the popup.
    void SetAnchorRect(RECT screen_rc) { anchor_rect_ = screen_rc; anchor_to_rect_ = true; }
    // Motion v2 "newmenu": the next popup grows out of this button (screen
    // px) - the container morphs from the button's pill, then rows fade in.
    void SetMorphFrom(RECT screen_rc) { morph_button_ = screen_rc; morph_next_ = true; }
    // Align an ordinary selection menu below its control; cleared after use.
    void SetDropdownRect(RECT screen_rc) { dropdown_rect_ = screen_rc; dropdown_ = true; }
    // Override the filter-mode minimum width (dips) for the next TrackPopup;
    // <= 0 restores the palette default. Reset after each popup.
    void SetFilterMinWidth(float dips) { filter_min_width_ = dips; }
    // Keys that, typed into the still-empty filter, invoke their command at
    // once instead of filtering (the tag panel's 1-9). Reset after each popup.
    void SetEmptyFilterKeys(std::vector<std::pair<wchar_t, int>> keys) { empty_filter_keys_ = std::move(keys); }
    // Limit the next popup's body to this many rows; wheel/keys reveal the rest.
    // Non-positive values preserve the ordinary unconstrained menu behavior.
    void SetMaxVisibleRows(int rows) { max_visible_rows_ = rows > 0 ? rows : 0; }

    // Borrow the search editor; show only results below its screen rect.
    void SetExternalFilterEdit(HWND edit) { external_edit_ = edit; }
    // Owner hosts a topmost overlay (system preview handler) over the area the
    // menu opens into: place the popup in the topmost band so it stays visible,
    // and dismiss on clicks that land in windows of other threads, which the
    // modal loop cannot otherwise observe. Sticky until changed.
    void SetTopmost(bool topmost) { topmost_ = topmost; }

    // Acrylic menus (process wide, Settings > menu acrylic). Turning it on also
    // makes every Compositor keep a GPU copy of its last frame: menus blur what
    // the owner window shows under them, baked once per open. High contrast,
    // Windows "Transparency effects" off and battery saver keep the solid plate.
    static void SetAcrylicEnabled(bool enabled) noexcept;
    static bool AcrylicEnabled() noexcept;

    static constexpr int kShadowMargin = 20; // px of transparent border around the card

private:
    // ULW staging buffer (GDI DIB); the main menu and the flyout each own one.
    struct Surface {
        HDC mem_dc = nullptr;
        HBITMAP dib = nullptr;
        void* bits = nullptr;
        int w = 0;
        int h = 0;
        // Acrylic plate for the card, keyed by its screen rect and theme.
        ComPtr<ID2D1Bitmap1> plate;
        RECT plate_key{};
        bool plate_dark = false;
        // Render targets reused across frames (hover glides and the open
        // transition redraw every frame; allocating per frame churned VRAM).
        const void* dc_key = nullptr;
        int tw = 0;
        int th = 0;
        ComPtr<ID2D1Bitmap1> target;
        ComPtr<ID2D1Bitmap1> scaled;
        ComPtr<ID2D1Bitmap1> readback;
    };

    bool EnsureWindow();
    bool EnsureFilterEdit();
    void ApplyFilterLead();   // SetFilterLead's props on edit_ (none: removed)
    void PlaceFilterEdit(int y_offset_px);
    void HideFilterEdit();
    void SyncFilterFromEdit();
    bool IsPaletteHwnd(HWND hwnd) const;
    bool RenderSurface(const FluentMenuModel& model, int hover_row, int hover_swatch,
                       float header_px, bool draw_filter_field, Surface& s);
    // Device bitmap for a row's app icon, created on first use and kept for
    // the popup's lifetime (reset by TrackPopup along with the plates).
    ID2D1Bitmap* IconBitmapFor(const common::MenuIconRef& pixels, ID2D1DeviceContext* dc);
    bool Render();                 // model_ -> dib_ (pixels), current hover state
    // Bakes s.plate for `card` (bitmap px) of a window at screen `origin` from
    // the owner compositor's frame copy; false keeps the solid plate.
    bool EnsurePlate(Surface& s, const D2D1_RECT_F& card, POINT origin, ID2D1Image* target);
    void Present(BYTE alpha, int y_offset_px);
    // One-level flyout for FluentMenuItem::children.
    bool EnsureSubWindow();
    void OpenSubmenu(int row);
    void CloseSubmenu();
    void OnSubMouse(POINT client_pt, bool button_up);
    bool RenderSub();
    void PresentSub(BYTE alpha);
    bool TickSubMotion();            // flyout fade/slide or glide; true while running
    void HideSubWindow();
    void LayoutWindow(POINT screen_pt);
    void RefreshFilter();
    int RunModalLoop();
    void OnMouse(POINT client_pt, bool button_up);
    void UpdateHover(int row, int swatch = -1);
    float BodyHeightPx() const;
    // Overflow scrolling (#67): a menu taller than the work area keeps its
    // card on screen and scrolls the rows between two arrow strips.
    bool Scrollable() const { return max_visible_rows_ > 0 || overflow_; }
    float ArrowPx() const;          // arrow strip height; 0 unless overflowing
    float ViewportPx() const;       // body height left for rows
    float MaxScrollPx() const;
    void ApplyHeightLimit(float available_px);
    bool ScrollTo(float y);         // clamps; true when the rows moved
    bool ScrollRows(int rows);      // whole rows, top row aligned like native menus
    void TickArrowScroll();         // modal-loop tick while resting on an arrow
    void UpdateTooltip(int row);
    bool RenderTip(const std::wstring& text);   // tooltip text -> tip_surf_
    int HitTestSwatch(int row, float client_x) const;
    int HitTestSwatch(const FluentMenuModel& model, int row, float client_x) const;
    int InvokeRow(int row);        // returns command or 0
    // Letter/digit keys in an ordinary (non-filter) menu: access-key dispatch.
    // Returns true when the key belongs to the menu (always for printable keys,
    // so typing never leaks into the window behind the popup).
    bool HandleMnemonicKey(const MSG& msg);
    int InvokeAt(int row, float client_x) const;
    int InvokeAt(const FluentMenuModel& model, int row, float client_x) const;
    float FilterHeaderPx() const;
    bool AppendFilterChar(wchar_t ch);
    bool TakeEmptyFilterKey(const std::wstring& typed);
    bool HandleFilterKey(UINT msg, WPARAM wParam);
    void PasteFilterText();

    static LRESULT CALLBACK MenuWndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ExternalFilterEditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
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
    std::wstring filter_lead_;           // SetFilterLead; one popup only
    std::wstring filter_lead_hint_;
    float filter_min_width_ = 0.0f;      // dips; <= 0 = palette default (440)
    std::vector<std::pair<wchar_t, int>> empty_filter_keys_;
    bool select_all_on_open_ = true;
    bool hover_first_on_open_ = true;
    bool filter_committed_ = false;
    bool filter_composing_ = false;
    bool forward_tab_key_ = false;
    RECT anchor_rect_{};
    bool anchor_to_rect_ = false;
    RECT dropdown_rect_{};
    bool dropdown_ = false;
    // The window the current left press went down on. The popup only acts on a
    // release it saw the press for: a dropdown opens from the button's
    // mouse-down, so the release that ends that click arrives with the list
    // already up, and when the list is tall enough to cover the pointer it
    // lands here — it ends the opening click, it does not pick a row.
    HWND pressed_hwnd_ = nullptr;
    POINT popup_pt_{};
    bool top_center_ = false;
    bool topmost_ = false;
    bool acrylic_active_ = false;  // decided when a popup opens
    HWND external_edit_ = nullptr; // borrowed; never moved, hidden or destroyed
    HWND edit_ = nullptr;
    HFONT edit_font_ = nullptr;
    std::uint64_t edit_font_generation_ = 0;   // typography::Generation() of edit_font_
    float edit_font_scale_ = 0.0f;
    HBRUSH edit_brush_ = nullptr;
    int present_offset_ = 0;
    BYTE present_alpha_ = 0;       // alpha of the last Present (repaints keep it)
    int slide_sign_ = -1;          // -1 slides down into place, +1 slides up
    int hover_row_ = -1;
    int hover_swatch_ = -1;
    int max_visible_rows_ = 0;
    float scroll_y_ = 0.0f;
    float body_limit_px_ = 0.0f;
    bool overflow_ = false;
    int arrow_hover_ = 0;          // -1 top arrow, +1 bottom arrow, 0 none
    std::chrono::steady_clock::time_point arrow_tick_{};
    float arrow_accum_ms_ = 0.0f;
    static constexpr float kScrollArrowDip = 20.0f;
    static constexpr float kScrollArrowMsPerRow = 60.0f;
    HWND tooltip_ = nullptr;
    std::wstring tooltip_text_;
    bool open_ = false;
    bool animating_out_ = false;
    int result_ = 0;
    int base_x_ = 0;               // window position before slide animation
    int base_y_ = 0;
    std::chrono::steady_clock::time_point anim_start_;

    Surface surf_;                 // main menu staging
    struct IconBitmap {
        common::MenuIconRef pixels;
        ComPtr<ID2D1Bitmap> bitmap;
    };
    std::vector<IconBitmap> icon_bitmaps_;  // a menu holds a few dozen rows at most

    // Flyout (one level, FluentMenuItem::children).
    HWND sub_hwnd_ = nullptr;
    Surface sub_surf_;
    Surface tip_surf_;             // hover tooltip staging
    FluentMenuModel sub_model_;
    int sub_parent_row_ = -1;      // row in model_ whose children are shown
    int sub_hover_ = -1;
    int sub_hover_swatch_ = -1;
    int sub_pending_row_ = -1;     // hover-to-open delay target
    std::chrono::steady_clock::time_point sub_pending_since_;
    int sub_x_ = 0;                // flyout window origin (screen px)
    int sub_y_ = 0;

    // Motion (see the header comment). motion_ samples SystemAnimationsEnabled
    // once per TrackPopup.
    bool motion_ = true;
    float open_t_ms_ = -1.0f;      // elapsed of the open transition; <0 = settled
    float close_k_ = 0.0f;         // 0..1 progress of the dismiss
    D2D1_POINT_2F open_origin_{};  // grow/shrink origin in surface px
    RECT morph_button_{};          // SetMorphFrom, screen px
    bool morph_next_ = false;      // armed for the next TrackPopup
    bool morph_ = false;           // this open morphs from morph_from_
    D2D1_RECT_F morph_from_{};     // the button pill in surface px
    ::pulse::ui::motion::SpringRectMotion plate_;
    ::pulse::ui::motion::SpringRectMotion sub_plate_;
    uint64_t plate_frame_ = 0;
    uint64_t sub_plate_frame_ = 0;
    uint64_t plate_context_ = 0;   // bumped per TrackPopup
    bool sub_animating_ = false;
    bool sub_fade_ = false;        // true: fade+slide in, false: glide to new header
    std::chrono::steady_clock::time_point sub_anim_start_;
    int sub_dx_ = 0;               // presented offset from sub_x_/sub_y_
    int sub_dy_ = 0;
    int sub_from_dx_ = 0;
    int sub_from_dy_ = 0;
    BYTE sub_alpha_ = 255;         // cap for PresentSub while fading in
};

} // namespace pulse::ui
