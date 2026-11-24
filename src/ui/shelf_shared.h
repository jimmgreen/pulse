#pragma once
#include "FluentTokens.h"
#include "shell_icons.h"
#include "thumbnail_cache.h"
#include "ui_compositor.h"
#include "ui_view_model.h"
#include <d2d1_1.h>
#include <dwrite_2.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

struct ShelfColors {
    D2D1_COLOR_F tray, tray_edge;
    D2D1_COLOR_F shelf_bg, shelf_solid, shelf_edge;
    D2D1_COLOR_F strong, text, sub, faint;
    D2D1_COLOR_F chip, chip_hot, ib_hot, kbd, sel, sel_strong;
    D2D1_COLOR_F half_edge, ck_edge, rm_bg, paper, div;
    D2D1_COLOR_F acc, acc2, move, move_soft, warn, ok, danger, on_acc, folder;
};

ShelfColors MakeShelfColors(bool dark, bool hc, const Theme& theme);

enum class ShelfGlyph : uint8_t {
    Tray, Chev, Up, Down, Pin, More, Dock, Popout,
    Copy, Move, Plus, Grid, List, Check, Close
};

void DrawShelfGlyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                    ShelfGlyph glyph, const D2D1_RECT_F& box, float px);

// Thumbnail card (.th.doc / .th.dir / .th.img / .th.snip / .th.link).
void DrawShelfItemThumb(ID2D1DeviceContext* dc, Compositor* comp,
                        IDWriteTextFormat* badge_fmt,
                        ThumbnailCache* thumbs, ShellIconCache* icons,
                        const std::wstring& path, const std::wstring& name,
                        const std::wstring& domain, DWORD attrs, bool is_dir,
                        bool missing, uint8_t content_kind, uint64_t id_seed,
                        const D2D1_RECT_F& slot, float size_px, float scale);

// Fanned 1..3 card pile + ringed count badge (.pile).
void DrawShelfPile(ID2D1DeviceContext* dc, Compositor* comp,
                   IDWriteTextFormat* badge_fmt, IDWriteTextFormat* count_fmt,
                   ThumbnailCache* thumbs, ShellIconCache* icons,
                   const TrayDeckView& d, const ShelfColors& p,
                   const D2D1_COLOR_F& ring_color,
                   const D2D1_RECT_F& fan_rc, float scale);

// Meta block: "N 项  size" + "来自 ..." or "⚠ N 项已被移动或删除 · 移出".
// If `out_stale_remove` is non-null, receives the clickable rect of "移出".
void DrawShelfMeta(Compositor* comp, ID2D1SolidColorBrush* br,
                   IDWriteTextFormat* strong_fmt, IDWriteTextFormat* size_fmt,
                   IDWriteTextFormat* sub_fmt, IDWriteTextFormat* link_fmt,
                   const TrayDeckView& d, const ShelfColors& p,
                   const D2D1_RECT_F& meta_rc, float scale, bool stale_hot,
                   D2D1_RECT_F* out_stale_remove = nullptr);

// Release split button (solid blue, solid orange, or proportional blue->orange
// gradient) + "N 同名" amber pill + optional snippet materialization note.
void DrawShelfReleaseButton(ID2D1DeviceContext* dc, Compositor* comp,
                            ID2D1SolidColorBrush* br,
                            IDWriteTextFormat* btn_fmt, IDWriteTextFormat* badge_fmt,
                            IDWriteTextFormat* note_fmt,
                            const TrayDeckView& d, const ShelfColors& p,
                            const D2D1_COLOR_F& ring_color,
                            const D2D1_RECT_F& split_rc, const D2D1_RECT_F& go_rc,
                            const D2D1_RECT_F& dd_rc, float scale,
                            bool compact_sidebar, bool hot_go, bool hot_dd,
                            float* out_bottom_y = nullptr);

// Shared drop layer geometry (files 2-half or content 1-half + bottom shelf chips).
struct ShelfDropGeom {
    D2D1_RECT_F header{};
    D2D1_RECT_F half[2]{};            // half[0] = full width when drop_content
    D2D1_RECT_F chips_label{};
    std::vector<D2D1_RECT_F> shelf_chips;
    std::vector<int> shelf_indices;   // index into d.shelves
    D2D1_RECT_F new_shelf_chip{};
};

ShelfDropGeom LayoutShelfDropLayer(const TrayDeckView& d, const D2D1_RECT_F& rc,
                                   float scale,
                                   const std::function<float(const std::wstring&)>& measure_chip);
int HitTestShelfDropLayer(const ShelfDropGeom& g, float x, float y, bool content, bool shift_move) noexcept;

void DrawShelfDropLayer(ID2D1DeviceContext* dc, Compositor* comp,
                        ID2D1SolidColorBrush* br,
                        IDWriteTextFormat* title_fmt, IDWriteTextFormat* sub_fmt,
                        IDWriteTextFormat* chip_fmt,
                        const TrayDeckView& d, const ShelfColors& p,
                        const ShelfDropGeom& g, float scale, bool dark);

// Shelf switcher button: [● ShelfName ▾] (+ optional "退出时清空" pill).
D2D1_RECT_F MeasureShelfSwitchButton(const TrayDeckView& d, float left, float cy,
                                     float max_w, float scale,
                                     const std::function<float(const std::wstring&)>& measure_bold);

void DrawShelfSwitchButton(ID2D1DeviceContext* dc, Compositor* comp,
                           ID2D1SolidColorBrush* br,
                           IDWriteTextFormat* name_fmt, IDWriteTextFormat* tag_fmt,
                           const TrayDeckView& d, const ShelfColors& p,
                           const D2D1_RECT_F& btn_rc, float scale, bool hot,
                           bool show_exit_tag, float max_tag_right);

// Smart line-break hints before ".ext" and after "_", "-", "·" for 2-line grid labels.
std::wstring FormatShelfGridName(const std::wstring& name, bool is_file);

} // namespace pulse::ui
