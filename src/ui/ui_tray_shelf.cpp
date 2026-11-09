// ui_tray_shelf.cpp — staging tray shelf.
//
// The tray panel shows one of three layers, swapped with a short cross-fade
// while the panel height springs to the new layer:
//   strip  (40 dip)  empty tray: icon, 暂存区, "拖入文件，或 Ctrl Shift S"
//   drop   (112 dip) files dragged over the window: 复制暂存 | 移动暂存
//   shelf  (150 dip) fan of the newest items, the release split button that
//                    names its target, ZIP / rename, 送到 chips
// Geometry lives in TrayShelfGeometry (ui_renderer_internal.h), shared with
// the hit test.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "typography.h"
#include <algorithm>
#include <cmath>

namespace pulse::ui {

namespace {

using pulse::l10n::StringId;

std::wstring Fill(std::wstring text, const wchar_t* key, const std::wstring& value) {
    const size_t at = text.find(key);
    if (at != std::wstring::npos) text.replace(at, wcslen(key), value);
    return text;
}

D2D1_COLOR_F Rgba(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                        a.a + (b.a - a.a) * t);
}

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float EaseOutCubic(float t) { t = Clamp01(t); const float u = 1.0f - t; return 1.0f - u * u * u; }
// cubic-bezier(.2,1.2,.3,1)-like: a small overshoot that settles.
float EaseOutBack(float t) {
    t = Clamp01(t);
    const float c1 = 1.2f, c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// popin: scale .5 -> 1 on cubic-bezier(.3,1.6,.5,1), fading in (450 ms).
float PopInScale(float t) {
    t = Clamp01(t);
    const float c1 = 2.2f, c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 0.5f + 0.5f * (1.0f + c3 * u * u * u + c1 * u * u);
}

struct ShelfPalette {
    D2D1_COLOR_F tray, edge, strong, text, sub, faint;
    D2D1_COLOR_F chip, chip_hot, ib_hot, kbd;
    D2D1_COLOR_F acc, acc2, move, move2, warn, on_acc;
    D2D1_COLOR_F folder;
};

ShelfPalette MakeShelfPalette(bool dark, bool hc, const Theme& theme) {
    ShelfPalette p{};
    p.acc = theme.accent;
    p.acc.a = 1.0f;
    p.acc2 = Mix(p.acc, Rgba(0xFFFFFF), dark ? 0.30f : 0.0f);
    p.move = Rgba(0xD9772F);
    p.move2 = Rgba(0xE08A48);
    p.warn = Rgba(0xF2A93B);
    p.on_acc = Rgba(0xFFFFFF);
    p.folder = Rgba(0xF5B942);
    if (hc) {
        p.tray = theme.surface_flyout;
        p.edge = theme.text;
        p.strong = theme.text;
        p.text = theme.text;
        p.sub = theme.text_secondary;
        p.faint = theme.text_secondary;
        p.chip = theme.fill_hover;
        p.chip_hot = theme.fill_selected;
        p.ib_hot = theme.fill_hover;
        p.kbd = theme.fill_hover;
        return p;
    }
    if (dark) {
        p.tray = Mix(Rgba(0x0F1521), p.acc, 0.02f);
        p.edge = Alpha(p.acc, 0.32f);
        p.strong = Rgba(0xEEF0FF);
        p.text = Rgba(0xCFD5EA);
        p.sub = Rgba(0x8A93AD);
        p.faint = Rgba(0x6F7891);
        p.chip = Rgba(0xFFFFFF, 0.055f);
        p.chip_hot = Alpha(p.acc, 0.26f);
        p.ib_hot = Rgba(0xFFFFFF, 0.07f);
        p.kbd = Rgba(0xFFFFFF, 0.05f);
    } else {
        p.tray = Mix(Rgba(0xF6F7FB), p.acc, 0.035f);
        p.edge = Alpha(p.acc, 0.30f);
        p.strong = Rgba(0x1B1F2A);
        p.text = Rgba(0x2E3442);
        p.sub = Rgba(0x5B6478);
        p.faint = Rgba(0x7B8396);
        p.chip = Rgba(0x000000, 0.05f);
        p.chip_hot = Alpha(p.acc, 0.16f);
        p.ib_hot = Rgba(0x000000, 0.06f);
        p.kbd = Rgba(0x000000, 0.06f);
    }
    return p;
}

// Small vector icons from the prototype (16x16 viewBox).
enum class ShelfIcon { Tray, Up, Down, Zip, Rename, More, Copy, Move, Clip, Check, Close };

void DrawShelfIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, ShelfIcon icon,
                   const D2D1_RECT_F& box, float px /* drawn edge in pixels */) {
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory.get()) return;
    const float k = px / 16.0f;
    const float ox = (box.left + box.right - px) * 0.5f;
    const float oy = (box.top + box.bottom - px) * 0.5f;
    auto P = [&](float x, float y) { return D2D1::Point2F(ox + x * k, oy + y * k); };
    ComPtr<ID2D1StrokeStyle> round;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &round);
    auto line = [&](float x0, float y0, float x1, float y1, float w) {
        dc->DrawLine(P(x0, y0), P(x1, y1), br, w * k, round.get());
    };
    auto rrect = [&](float x, float y, float w, float h, float r, float sw) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ox + x * k, oy + y * k,
            ox + (x + w) * k, oy + (y + h) * k), r * k, r * k), br, sw * k, round.get());
    };
    auto poly = [&](std::initializer_list<D2D1_POINT_2F> pts, bool closed, float sw) {
        ComPtr<ID2D1PathGeometry> path;
        factory->CreatePathGeometry(&path);
        if (!path.get()) return;
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(&sink);
        auto it = pts.begin();
        sink->BeginFigure(P(it->x, it->y), D2D1_FIGURE_BEGIN_HOLLOW);
        for (++it; it != pts.end(); ++it) sink->AddLine(P(it->x, it->y));
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
        dc->DrawGeometry(path.get(), br, sw * k, round.get());
    };
    switch (icon) {
    case ShelfIcon::Tray:
        poly({{2.5f, 9.5f}, {5.5f, 9.5f}, {6.5f, 11.1f}, {9.5f, 11.1f}, {10.5f, 9.5f}, {13.5f, 9.5f}},
             false, 1.5f);
        poly({{2.5f, 9.5f}, {4.1f, 4.3f}, {5.2f, 3.5f}, {10.8f, 3.5f}, {11.9f, 4.3f}, {13.5f, 9.5f},
              {13.5f, 12.5f}, {12.5f, 13.5f}, {3.5f, 13.5f}, {2.5f, 12.5f}}, true, 1.5f);
        break;
    case ShelfIcon::Up:
        poly({{4.0f, 10.0f}, {8.0f, 6.0f}, {12.0f, 10.0f}}, false, 1.7f);
        break;
    case ShelfIcon::Down:
        poly({{4.0f, 6.0f}, {8.0f, 10.0f}, {12.0f, 6.0f}}, false, 1.7f);
        break;
    case ShelfIcon::Zip:
        rrect(3.0f, 1.8f, 10.0f, 12.4f, 1.8f, 1.4f);
        line(8.0f, 2.0f, 8.0f, 3.4f, 1.4f);
        line(8.0f, 4.4f, 8.0f, 5.8f, 1.4f);
        line(8.0f, 6.8f, 8.0f, 8.2f, 1.4f);
        rrect(6.5f, 9.2f, 3.0f, 2.7f, 0.8f, 1.4f);
        break;
    case ShelfIcon::Rename:
        rrect(1.8f, 4.5f, 8.5f, 7.0f, 1.6f, 1.4f);
        line(4.2f, 8.0f, 7.8f, 8.0f, 1.4f);
        line(12.8f, 2.2f, 12.8f, 13.8f, 1.4f);
        line(11.4f, 2.2f, 14.2f, 2.2f, 1.4f);
        line(11.4f, 13.8f, 14.2f, 13.8f, 1.4f);
        break;
    case ShelfIcon::More:
        for (float x : {3.5f, 8.0f, 12.5f})
            dc->FillEllipse(D2D1::Ellipse(P(x, 8.0f), 1.3f * k, 1.3f * k), br);
        break;
    case ShelfIcon::Copy:
        rrect(5.0f, 5.0f, 8.5f, 8.5f, 1.8f, 1.2f);
        poly({{3.0f, 10.5f}, {3.0f, 3.8f}, {3.4f, 2.9f}, {4.3f, 2.5f}, {11.0f, 2.5f}}, false, 1.2f);
        break;
    case ShelfIcon::Move:
        line(2.5f, 8.0f, 11.5f, 8.0f, 1.2f);
        poly({{8.5f, 4.8f}, {11.7f, 8.0f}, {8.5f, 11.2f}}, false, 1.2f);
        line(13.5f, 3.0f, 13.5f, 13.0f, 1.2f);
        break;
    case ShelfIcon::Check:  // M3 8.5l3 3 7-7
        poly({{3.0f, 8.5f}, {6.0f, 11.5f}, {13.0f, 4.5f}}, false, 2.6f);
        break;
    case ShelfIcon::Close:
        line(4.5f, 4.5f, 11.5f, 11.5f, 1.5f);
        line(11.5f, 4.5f, 4.5f, 11.5f, 1.5f);
        break;
    case ShelfIcon::Clip:
        rrect(3.5f, 3.0f, 9.0f, 11.0f, 1.6f, 1.5f);
        poly({{6.0f, 3.0f}, {6.0f, 2.0f}, {10.0f, 2.0f}, {10.0f, 3.0f}}, false, 1.5f);
        break;
    }
}

// Text in a box: one line, end ellipsis, centered vertically.
void ShelfText(Compositor* compositor, IDWriteTextFormat* fmt,
               ID2D1SolidColorBrush* br, const std::wstring& text, const D2D1_RECT_F& rc,
               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    const float w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (!compositor || !compositor->Dc()) return;
    IDWriteFactory2* factory = compositor->DwriteFactory();
    if (!factory || !fmt || text.empty() || w <= 1.0f || h <= 1.0f) return;
    auto foreground = br->GetColor();
    foreground.a *= br->GetOpacity();
    if (!IsHighContrast() && compositor->DrawLumaText(text, fmt, rc, foreground,
            D2D1::ColorF(0, 0.0f), align)) {
        return;
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, w, h,
                                         &layout)) || !layout.get())
        return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(layout.get(), &ellipsis);
    layout->SetTrimming(&trimming, ellipsis.get());
    compositor->DrawTextLayout(D2D1::Point2F(rc.left, rc.top), layout.get(), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

} // namespace

IDWriteTextFormat* MainRenderer::ShelfFormat(int role) const {
    if (!compositor_ || role < 0 || role >= kShelfFormatCount) return nullptr;
    if (shelf_formats_scale_ != scale_) {
        for (auto& f : shelf_formats_) f.reset();
        shelf_formats_scale_ = scale_;
    }
    if (!shelf_formats_[role].get()) {
        struct Spec { float size; DWRITE_FONT_WEIGHT weight; };
        static constexpr Spec kSpecs[kShelfFormatCount] = {
            {13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},  // 0 title / half label
            {12.0f, DWRITE_FONT_WEIGHT_NORMAL},     // 1 hint, chips
            {12.5f, DWRITE_FONT_WEIGHT_BOLD},       // 2 release label
            {14.0f, DWRITE_FONT_WEIGHT_BOLD},       // 3 meta count
            {11.5f, DWRITE_FONT_WEIGHT_NORMAL},     // 4 sub lines, 送到
            {10.5f, DWRITE_FONT_WEIGHT_BOLD},       // 5 badges
            {10.5f, DWRITE_FONT_WEIGHT_NORMAL},     // 6 kbd
            {11.5f, DWRITE_FONT_WEIGHT_BOLD},       // 7 clip button
            {11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD},  // 8 clip row
            {11.0f, DWRITE_FONT_WEIGHT_BOLD},       // 9 open view source header
            {13.0f, DWRITE_FONT_WEIGHT_NORMAL},     // 10 open view row name
        };
        typography::CreateTextFormat(compositor_->DwriteFactory(),
            {typography::FontRole::Text, kSpecs[role].size * scale_, kSpecs[role].weight},
            &shelf_formats_[role]);
        if (shelf_formats_[role].get()) {
            shelf_formats_[role]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            shelf_formats_[role]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    return shelf_formats_[role].get() ? shelf_formats_[role].get() : compositor_->SmallFormat();
}

float MainRenderer::ShelfMeasure(const std::wstring& text, bool compact_text) const {
    if (!compositor_) return 0.0f;
    return MeasureTextWidth(compositor_->DwriteFactory(), ShelfFormat(compact_text ? 1 : 0), text);
}

void MainRenderer::DrawTrayShelf(const WindowViewModel& vm, const D2D1_RECT_F& rc,
                                 const Theme& theme) {
    if (!compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const TrayDeckView& d = vm.tray_deck;
    const ShelfPalette p = MakeShelfPalette(vm.dark, IsHighContrast(), theme);
    const float s = scale_;
    tray_panel_rc_ = rc;
    const float panel_h = rc.bottom - rc.top;
    const float radius = std::min(12.0f * s, panel_h * 0.5f);

    ComPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(p.tray, &br);
    if (!br.get()) return;
    // A hiding empty tray fades out over its last 40 dip.
    const bool fading = d.hide_empty && panel_h < 40.0f * s;
    if (fading)
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::IdentityMatrix(), Clamp01(panel_h / (40.0f * s))), nullptr);
    dc->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), br.get());
    // Drop target: the edge brightens while files hover the tray itself.
    const D2D1_COLOR_F edge = vm.tray_drop ? Alpha(p.acc2, 0.75f) : p.edge;
    br->SetColor(edge);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(rc.left + 0.5f, rc.top + 0.5f,
        rc.right - 0.5f, rc.bottom - 0.5f), radius - 0.5f, radius - 0.5f), br.get(), 1.0f);

    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    if (factory.get())
        factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rc, radius, radius), &clip);
    if (clip.get())
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.get()), nullptr);

    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    const float swap_ms = Clamp01(d.swap) * 410.0f;
    if (d.prev_stage >= 0 && d.prev_stage != d.stage && swap_ms < 130.0f) {
        const float t = swap_ms / 130.0f;
        const float a = 1.0f - EaseOutCubic(t);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -4.0f * s * t) * old);
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
        DrawTrayShelfLayer(vm, d.prev_stage, rc, theme);
        dc->PopLayer();
        dc->SetTransform(old);
    }
    const float tin = d.prev_stage >= 0 ? Clamp01((swap_ms - 60.0f) / 280.0f) : 1.0f;
    if (tin > 0.0f) {
        const float e = EaseOutBack(tin);
        const float scale = 0.985f + 0.015f * e;
        const D2D1_POINT_2F origin = D2D1::Point2F((rc.left + rc.right) * 0.5f, rc.top);
        const bool layered = tin < 1.0f;
        if (layered) {
            dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, origin) *
                             D2D1::Matrix3x2F::Translation(0.0f, 10.0f * s * (1.0f - e)) * old);
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), EaseOutCubic(tin)), nullptr);
        }
        DrawTrayShelfLayer(vm, d.stage, rc, theme);
        if (layered) {
            dc->PopLayer();
            dc->SetTransform(old);
        }
    }
    if (clip.get()) dc->PopLayer();
    if (fading) dc->PopLayer();
}

void MainRenderer::DrawTrayShelfLayer(const WindowViewModel& vm, int stage, const D2D1_RECT_F& rc,
                                      const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* dw = compositor_->DwriteFactory();
    const TrayDeckView& d = vm.tray_deck;
    const ShelfPalette p = MakeShelfPalette(vm.dark, IsHighContrast(), theme);
    const float s = scale_;
    const TrayShelfGeom g = TrayShelfGeometry(d, rc, s,
        [this](const std::wstring& t, bool compact_text) { return ShelfMeasure(t, compact_text); });

    ComPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(p.text, &br);
    if (!br.get()) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br.get(); };
    auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), B(c));
    };
    auto hot = [&](TrayShelfHit h, int sub = -1) {
        return IsHovered(vm, HitTestResult::TrayShelf, static_cast<int>(h), sub);
    };
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);

    if (stage == static_cast<int>(TrayStage::Strip)) {
        DrawShelfIcon(dc, B(p.acc2), ShelfIcon::Tray, g.strip_icon, 16.0f * s);
        ShelfText(compositor_, ShelfFormat(0), B(p.acc2), l10n::Get(StringId::TrayShelfTitle), g.strip_title);
        ShelfText(compositor_, ShelfFormat(1), B(p.faint),
                  l10n::Get(g.strip_hint_short ? StringId::TrayStripHintShort : StringId::TrayStripHint),
                  g.strip_hint);
        if (g.strip_kbd.right > g.strip_kbd.left) {
            fill(g.strip_kbd, 4.0f * s, p.kbd);
            ShelfText(compositor_, ShelfFormat(6), B(p.sub), l10n::Get(StringId::TrayStripKbd), g.strip_kbd,
                      DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (d.clip_count > 0 && g.strip_clip.right > g.strip_clip.left)
            DrawShelfClip(vm, g.strip_clip, false, theme);
        return;
    }

    if (stage == static_cast<int>(TrayStage::Drop)) {
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1StrokeStyle> dash;
        if (factory.get()) {
            const float dashes[] = {3.0f, 2.5f};
            factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT,
                D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                D2D1_DASH_STYLE_CUSTOM, 0.0f), dashes, 2, &dash);
        }
        for (int i = 0; i < 2; ++i) {
            const bool mv = i == 1;
            // mode_t follows the active half while dragging (0 copy, 1 move).
            const float on = mv ? Clamp01(d.mode_t) : 1.0f - Clamp01(d.mode_t);
            const D2D1_RECT_F h = g.half[i];
            const float cx = (h.left + h.right) * 0.5f, cy = (h.top + h.bottom) * 0.5f;
            const float sc = 1.0f + 0.035f * EaseOutBack(on);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(cx, cy)) * old);
            const D2D1_COLOR_F on_fill = mv ? Rgba(0xD9772F, 0.20f) : Alpha(p.acc, 0.22f);
            const D2D1_COLOR_F on_edge = mv ? p.move2 : p.acc2;
            const D2D1_COLOR_F off_edge = Alpha(p.acc2, 0.28f);
            const float r = 10.0f * s;
            const D2D1_RECT_F inset = D2D1::RectF(h.left + 0.75f * s, h.top + 0.75f * s,
                                                  h.right - 0.75f * s, h.bottom - 0.75f * s);
            if (on > 0.01f) fill(h, r, Alpha(on_fill, on));
            if (on < 0.99f)
                dc->DrawRoundedRectangle(D2D1::RoundedRect(inset, r, r), B(Alpha(off_edge, 1.0f - on)),
                                         1.5f * s, dash.get());
            if (on > 0.01f)
                dc->DrawRoundedRectangle(D2D1::RoundedRect(inset, r, r), B(Alpha(on_edge, on)), 1.5f * s);
            const D2D1_COLOR_F icon_c = Mix(p.sub, mv ? Rgba(0xFFC89E) : Rgba(0xC6CEFF), on);
            const D2D1_COLOR_F b_c = Mix(p.text, vm.dark ? Rgba(0xFFFFFF) : p.strong, on);
            const float block = 20.0f * s + 5.0f * s + 17.0f * s + 5.0f * s + 14.0f * s;
            float y = cy - block * 0.5f;
            DrawShelfIcon(dc, B(icon_c), mv ? ShelfIcon::Move : ShelfIcon::Copy,
                          D2D1::RectF(cx - 10.0f * s, y, cx + 10.0f * s, y + 20.0f * s), 20.0f * s);
            y += 25.0f * s;
            ShelfText(compositor_, ShelfFormat(0), B(b_c),
                      l10n::Get(mv ? StringId::TrayDropMoveTitle : StringId::TrayDropCopyTitle),
                      D2D1::RectF(h.left + 4.0f * s, y, h.right - 4.0f * s, y + 17.0f * s),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            y += 22.0f * s;
            ShelfText(compositor_, ShelfFormat(4), B(p.faint),
                      l10n::Get(mv ? StringId::TrayDropOrShift : StringId::TrayDropDefault),
                      D2D1::RectF(h.left + 4.0f * s, y, h.right - 4.0f * s, y + 14.0f * s),
                      DWRITE_TEXT_ALIGNMENT_CENTER);
            dc->SetTransform(old);
        }
        return;
    }

    if (stage == static_cast<int>(TrayStage::Open)) {
        DrawTrayOpen(vm, rc, theme);
        return;
    }

    // ---------------------------------------------------------------- shelf
    if (d.clip_count > 0 && g.clip_row.right > g.clip_row.left) DrawShelfClip(vm, g.clip_row, true, theme);
    // r1: fan.
    {
        const D2D1_RECT_F f = g.fan;
        const float b = Clamp01(d.bump);
        float bump = 0.0f;
        if (b > 0.0f && b < 1.0f) bump = b < 0.3f ? EaseOutCubic(b / 0.3f) : 1.0f - EaseOutCubic((b - 0.3f) / 0.7f);
        const float bs = 1.0f + 0.16f * bump;
        const D2D1_MATRIX_3X2_F fan_m = D2D1::Matrix3x2F::Scale(bs, bs,
            D2D1::Point2F((f.left + f.right) * 0.5f, (f.top + f.bottom) * 0.5f)) *
            D2D1::Matrix3x2F::Translation(0.0f, -3.0f * s * bump) * old;
        const int n = static_cast<int>(d.fan.size());
        const float dw_ = 26.0f * s, dh = 33.0f * s;
        const float spread = EaseOutBack(d.fan_spread);
        for (int k = 0; k < n; ++k) {
            const TrayCardView& c = d.fan[static_cast<size_t>(k)];
            float r = static_cast<float>(k - (n - 1)) * 9.0f;
            float tx = r * 0.7f;
            if (n >= 2 && k == 0) {  // :nth-child(1) spreads left
                tx += (-10.0f - tx) * spread;
                r += (-14.0f - r) * spread;
            } else if (n == 3 && k == 2) {  // :nth-child(3) spreads right
                tx += (10.0f - tx) * spread;
                r += (12.0f - r) * spread;
            }
            const D2D1_RECT_F doc = D2D1::RectF(f.left + 12.0f * s, f.top + 3.0f * s,
                                                f.left + 12.0f * s + dw_, f.top + 3.0f * s + dh);
            const D2D1_POINT_2F center = D2D1::Point2F((doc.left + doc.right) * 0.5f, (doc.top + doc.bottom) * 0.5f);
            dc->SetTransform(D2D1::Matrix3x2F::Rotation(r, center) *
                             D2D1::Matrix3x2F::Translation(tx * s, 0.0f) * fan_m);
            const float rr = (c.is_dir ? 3.0f : 4.0f) * s;
            // Shadow: 0 3px 10px rgba(0,0,0,.4).
            for (int l = 3; l >= 1; --l) {
                const float grow = static_cast<float>(l) * 1.4f * s;
                fill(D2D1::RectF(doc.left - grow, doc.top - grow + 3.0f * s, doc.right + grow,
                                 doc.bottom + grow + 3.0f * s), rr + grow,
                     Rgba(0x000000, (vm.dark ? 0.16f : 0.07f) * (1.0f - 0.25f * (l - 1))));
            }
            if (c.is_dir) {
                fill(doc, rr, Rgba(0xE8B64A));
                fill(D2D1::RectF(doc.left, doc.top, doc.right, doc.top + dh * 0.45f), rr, Rgba(0xF0C35A));
                fill(D2D1::RectF(doc.left + 2.0f * s, doc.top - 2.5f * s, doc.left + 11.0f * s, doc.top + 3.0f * s),
                     2.0f * s, Rgba(0xE0A93A));
            } else {
                // Paper with a folded corner and a type strip (.doc).
                fill(doc, rr, Rgba(0xF4F5F7));
                fill(D2D1::RectF(doc.left, doc.top + dh * 0.5f, doc.right, doc.bottom), rr, Rgba(0xE6E8EC));
                const D2D1_RECT_F art = D2D1::RectF(doc.left + 3.0f * s, doc.top + 3.0f * s,
                                                    doc.right - 3.0f * s, doc.bottom - 16.0f * s);
                bool drew = false;
                if (!c.missing) {
                    drew = thumbnail_cache_.Draw(dc, D2D1::RectF(doc.left + 1.0f * s, doc.top + 1.0f * s,
                        doc.right - 1.0f * s, doc.bottom - 1.0f * s), c.path, c.attrs, 64, 0, 0, 0, 1.0f) ==
                        PreviewDrawResult::Bitmap;
                }
                if (!drew) {
                    const float icon = std::round(14.0f * s);
                    if (ID2D1Bitmap* bitmap = icon_cache_.BitmapFor(c.missing ? L"" : c.path, c.name, false,
                                                                    c.attrs, icon)) {
                        const float ix = (art.left + art.right - icon) * 0.5f;
                        const float iy = (art.top + art.bottom - icon) * 0.5f + 1.0f * s;
                        const D2D1_RECT_F dest = D2D1::RectF(ix, iy, ix + icon, iy + icon);
                        dc->DrawBitmap(bitmap, &dest, c.missing ? 0.45f : 1.0f,
                                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                    }
                    const std::wstring chip = TypeChipLabel(c.name, false);
                    const D2D1_RECT_F strip = D2D1::RectF(doc.left + 3.0f * s, doc.bottom - 14.0f * s,
                                                          doc.right - 3.0f * s, doc.bottom - 4.0f * s);
                    fill(strip, 2.0f * s, Rgba(TypeChipRgb(chip)));
                    // Folded corner.
                    fill(D2D1::RectF(doc.right - 8.0f * s, doc.top, doc.right, doc.top + 8.0f * s), 2.0f * s,
                         Rgba(0xC3C6CD));
                    if (chip.size() <= 4) {
                        ShelfText(compositor_, ShelfFormat(5), B(Rgba(0xFFFFFF)), chip,
                                  D2D1::RectF(strip.left - 2.0f * s, strip.top - 1.0f * s, strip.right + 2.0f * s,
                                              strip.bottom + 1.0f * s), DWRITE_TEXT_ALIGNMENT_CENTER);
                    }
                }
            }
        }
        dc->SetTransform(fan_m);
        // Count badge, ringed in the tray colour.
        const std::wstring count = std::to_wstring(std::max(0, d.total_count - d.stale_count));
        const float tw = MeasureTextWidth(dw, ShelfFormat(5), count);
        const float bw = std::max(20.0f * s, tw + 12.0f * s);
        const D2D1_RECT_F badge = D2D1::RectF(f.right - bw, f.bottom - 20.0f * s, f.right, f.bottom);
        const float ring = 2.5f * s;
        fill(D2D1::RectF(badge.left - ring, badge.top - ring, badge.right + ring, badge.bottom + ring),
             10.0f * s + ring, p.tray);
        fill(badge, 10.0f * s, Mix(p.acc, p.move, d.mode_t));
        ShelfText(compositor_, ShelfFormat(5), B(p.on_acc), count, badge, DWRITE_TEXT_ALIGNMENT_CENTER);
        dc->SetTransform(old);
    }
    // r1: meta.
    {
        const D2D1_RECT_F m = g.meta;
        const float y = m.top + 4.0f * s;
        const std::wstring count = Fill(l10n::Get(StringId::TrayItemsFormat), L"{n}", std::to_wstring(std::max(0, d.total_count - d.stale_count)));
        const float cw = std::min(m.right - m.left, MeasureTextWidth(dw, ShelfFormat(3), count) + 1.0f);
        ShelfText(compositor_, ShelfFormat(3), B(p.strong), count, D2D1::RectF(m.left, y, m.left + cw, y + 20.0f * s));
        if (d.total_size > 0)
            ShelfText(compositor_, ShelfFormat(1), B(p.sub), pulse::format::ByteSize(d.total_size, true),
                      D2D1::RectF(m.left + cw + 6.0f * s, y + 1.0f * s, m.right, y + 20.0f * s));
        const D2D1_RECT_F line_b = D2D1::RectF(m.left, y + 21.0f * s, m.right, y + 37.0f * s);
        if (d.stale_count > 0)
            ShelfText(compositor_, ShelfFormat(4), B(p.warn),
                      Fill(l10n::Get(StringId::TrayStaleFormat), L"{n}", std::to_wstring(d.stale_count)), line_b);
        else
            ShelfText(compositor_, ShelfFormat(4), B(p.faint), d.sources_text, line_b);
    }
    auto icon_button = [&](const D2D1_RECT_F& r, ShelfIcon icon, bool is_hot, float px) {
        if (is_hot) fill(r, 8.0f * s, p.ib_hot);
        DrawShelfIcon(dc, B(is_hot ? p.strong : p.sub), icon, r, px);
    };
    if (g.has_expand) icon_button(g.expand, ShelfIcon::Up, hot(TrayShelfHit::Expand), 14.0f * s);
    icon_button(g.more, ShelfIcon::More, hot(TrayShelfHit::More), 16.0f * s);

    // r2: split button.
    {
        const D2D1_RECT_F sp = g.split;
        const float r = 9.0f * s;
        const bool usable = !d.target.empty();
        const D2D1_COLOR_F base = Mix(p.acc, p.move, d.mode_t);
        // 0 4px 14px -6px glow under the button.
        for (int l = 0; l < 3; ++l) {
            const float off = (3.0f + 1.5f * static_cast<float>(l)) * s;
            const float inset = (4.0f - 1.0f * static_cast<float>(l)) * s;
            fill(D2D1::RectF(sp.left + inset, sp.top + off, sp.right - inset, sp.bottom + off), r,
                 Alpha(base, (usable ? 0.16f : 0.06f) * (1.0f - 0.3f * static_cast<float>(l))));
        }
        fill(sp, r, usable ? base : Alpha(base, 0.45f));
        // inset 0 1px 0 rgba(255,255,255,.14).
        dc->DrawLine(D2D1::Point2F(sp.left + r * 0.6f, sp.top + 0.5f * s),
                     D2D1::Point2F(sp.right - r * 0.6f, sp.top + 0.5f * s), B(Rgba(0xFFFFFF, 0.14f)), 1.0f * s);
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        if (factory.get()) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(sp, r, r), &clip);
        if (clip.get()) dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.get()), nullptr);
        if (usable && hot(TrayShelfHit::Release)) fill(g.go, 0.0f, Rgba(0xFFFFFF, 0.12f));
        if (hot(TrayShelfHit::ReleaseMenu)) fill(g.dd, 0.0f, Rgba(0xFFFFFF, 0.12f));
        if (clip.get()) dc->PopLayer();
        dc->DrawLine(D2D1::Point2F(g.dd.left, g.dd.top), D2D1::Point2F(g.dd.left, g.dd.bottom),
                     B(Rgba(0xFFFFFF, 0.22f)), 1.0f);
        DrawShelfIcon(dc, B(p.on_acc), ShelfIcon::Down, g.dd, 14.0f * s);
        if (!d.zipping) {
            std::wstring label = usable
                ? Fill(l10n::Get(d.move_mode ? StringId::TrayMoveToFormat : StringId::TrayCopyToFormat),
                       L"{name}", d.target)
                : l10n::Get(StringId::TrayNoTarget);
            const float lt = Clamp01(d.label_t);
            const float e = EaseOutBack(lt);
            const D2D1_RECT_F lr = D2D1::RectF(g.go.left + 8.0f * s, g.go.top + 6.0f * s * (1.0f - e),
                                               g.go.right - 8.0f * s, g.go.bottom + 6.0f * s * (1.0f - e));
            ShelfText(compositor_, ShelfFormat(2), B(Alpha(p.on_acc, (usable ? 1.0f : 0.75f) * EaseOutCubic(lt))),
                      label, lr, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (d.conflicts > 0 && usable) {
            const std::wstring c = Fill(l10n::Get(StringId::TrayConflictsFormat), L"{n}", std::to_wstring(d.conflicts));
            const float cw = MeasureTextWidth(dw, ShelfFormat(5), c) + 12.0f * s;
            const D2D1_RECT_F cb = D2D1::RectF(sp.right + 5.0f * s - cw, sp.top - 8.0f * s, sp.right + 5.0f * s,
                                               sp.top + 8.0f * s);
            const float ring = 2.5f * s;
            fill(D2D1::RectF(cb.left - ring, cb.top - ring, cb.right + ring, cb.bottom + ring), 8.0f * s + ring, p.tray);
            fill(cb, 8.0f * s, p.warn);
            ShelfText(compositor_, ShelfFormat(5), B(Rgba(0x2A1800)), c, cb, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    // r2: ZIP (ring while compressing) and rename.
    if (d.zipping) {
        const D2D1_POINT_2F c = D2D1::Point2F((g.zip.left + g.zip.right) * 0.5f, (g.zip.top + g.zip.bottom) * 0.5f);
        const float rad = 6.0f * s, sw = 1.9f * s;
        dc->DrawEllipse(D2D1::Ellipse(c, rad, rad), B(vm.dark ? Rgba(0xFFFFFF, 0.15f) : Rgba(0x000000, 0.12f)), sw);
        const float prog = Clamp01(d.zip_progress);
        ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ComPtr<ID2D1PathGeometry> arc;
        if (factory.get() && prog > 0.005f) factory->CreatePathGeometry(&arc);
        if (arc.get()) {
            ComPtr<ID2D1GeometrySink> sink;
            arc->Open(&sink);
            const float a0 = -3.14159265f * 0.5f;
            const float a1 = a0 + 2.0f * 3.14159265f * std::min(prog, 0.999f);
            sink->BeginFigure(D2D1::Point2F(c.x + rad * std::cos(a0), c.y + rad * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(c.x + rad * std::cos(a1), c.y + rad * std::sin(a1)),
                D2D1::SizeF(rad, rad), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                prog > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            ComPtr<ID2D1StrokeStyle> cap;
            factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND),
                                       nullptr, 0, &cap);
            dc->DrawGeometry(arc.get(), B(vm.dark ? Rgba(0xA7B2FF) : p.acc), sw, cap.get());
        }
    } else {
        icon_button(g.zip, ShelfIcon::Zip, hot(TrayShelfHit::Zip), 16.0f * s);
    }
    icon_button(g.rename, ShelfIcon::Rename, hot(TrayShelfHit::Rename), 16.0f * s);

    // r3: 送到 + chips + ＋.
    ShelfText(compositor_, ShelfFormat(4), B(p.faint), l10n::Get(StringId::TraySendTo), g.send_label);
    for (size_t i = 0; i < g.chips.size(); ++i) {
        const D2D1_RECT_F c = g.chips[i];
        const int dest = g.chip_dest[i];
        const bool is_hot = hot(TrayShelfHit::Chip, dest);
        D2D1_COLOR_F bg = is_hot ? p.chip_hot : p.chip;
        float sc = 1.0f;
        if (d.flash_chip == dest && d.flash_t < 1.0f) {
            const float t = EaseOutCubic(d.flash_t);
            bg = Mix(Alpha(p.acc2, 0.7f), bg, t);
            sc = 1.08f - 0.08f * t;
        }
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc,
            D2D1::Point2F((c.left + c.right) * 0.5f, (c.top + c.bottom) * 0.5f)) * old);
        fill(c, 13.0f * s, bg);
        // Yellow folder (.fo): body 13x10 with a small tab.
        const float fx = c.left + 8.0f * s, fy = (c.top + c.bottom) * 0.5f - 4.0f * s;
        fill(D2D1::RectF(fx, fy - 1.5f * s, fx + 6.0f * s, fy + 2.0f * s), 1.2f * s, Mix(p.folder, Rgba(0xC88A1E), 0.25f));
        fill(D2D1::RectF(fx, fy, fx + 13.0f * s, fy + 10.0f * s), 2.0f * s, p.folder);
        ShelfText(compositor_, ShelfFormat(1), B(is_hot ? (vm.dark ? Rgba(0xFFFFFF) : p.strong) : p.text),
                  d.dests[static_cast<size_t>(dest)].label,
                  D2D1::RectF(fx + 13.0f * s + 6.0f * s, c.top, c.right - 10.0f * s, c.bottom));
        dc->SetTransform(old);
    }
    {
        const bool is_hot = hot(TrayShelfHit::Pick);
        fill(g.pick, 13.0f * s, is_hot ? p.chip_hot : p.chip);
        ShelfText(compositor_, ShelfFormat(1), B(is_hot ? p.strong : p.sub), L"\xFF0B", g.pick,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void MainRenderer::DrawShelfClip(const WindowViewModel& vm, const D2D1_RECT_F& rc, bool row,
                                 const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* dw = compositor_->DwriteFactory();
    const TrayDeckView& d = vm.tray_deck;
    const ShelfPalette p = MakeShelfPalette(vm.dark, IsHighContrast(), theme);
    const float s = scale_;
    ComPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(p.acc, &br);
    if (!br.get()) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br.get(); };
    const bool hot = IsHovered(vm, HitTestResult::TrayShelf, static_cast<int>(TrayShelfHit::Clip));
    const float cy = (rc.top + rc.bottom) * 0.5f;
    const float t = Clamp01(d.clip_t);
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    const bool layered = t < 1.0f;
    if (layered) {
        // popin: the row grows from its left edge, the button from its centre.
        const float k = PopInScale(t);
        const D2D1_POINT_2F origin = row ? D2D1::Point2F(rc.left, cy)
                                         : D2D1::Point2F((rc.left + rc.right) * 0.5f, cy);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, origin) * old);
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), EaseOutCubic(t / 0.6f)), nullptr);
    }
    const float radius = row ? 8.0f * s : (rc.bottom - rc.top) * 0.5f;
    const float a = row ? (hot ? 0.30f : 0.16f) : (hot ? 0.38f : 0.24f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(rc, radius, radius), B(Alpha(p.acc, a)));
    const D2D1_COLOR_F ink = vm.dark ? Rgba(0xC6CEFF) : Mix(p.acc, Rgba(0x000000), 0.2f);
    const D2D1_RECT_F ib = D2D1::RectF(rc.left + 10.0f * s, cy - 6.5f * s, rc.left + 23.0f * s, cy + 6.5f * s);
    DrawShelfIcon(dc, B(ink), ShelfIcon::Clip, ib, 13.0f * s);
    const std::wstring n = std::to_wstring(d.clip_count);
    if (!row) {
        ShelfText(compositor_, ShelfFormat(7), B(ink), Fill(l10n::Get(StringId::TrayClipStrip), L"{n}", n),
                  D2D1::RectF(ib.right + 5.0f * s, rc.top, rc.right - 8.0f * s, rc.bottom));
    } else {
        const std::wstring take = l10n::Get(StringId::TrayClipTake);
        const float take_w = MeasureTextWidth(dw, ShelfFormat(8), take) + 1.0f * s;
        const D2D1_RECT_F tr = D2D1::RectF(std::max(ib.right, rc.right - 10.0f * s - take_w), rc.top,
                                           rc.right - 10.0f * s, rc.bottom);
        ShelfText(compositor_, ShelfFormat(8), B(ink), Fill(l10n::Get(StringId::TrayClipRow), L"{n}", n),
                  D2D1::RectF(ib.right + 7.0f * s, rc.top, tr.left - 6.0f * s, rc.bottom));
        ShelfText(compositor_, ShelfFormat(8), B(vm.dark ? Rgba(0xFFFFFF) : p.strong), take, tr,
                  DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (layered) {
        dc->PopLayer();
        dc->SetTransform(old);
    }
}

void MainRenderer::DrawTrayOpen(const WindowViewModel& vm, const D2D1_RECT_F& rc, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* dw = compositor_->DwriteFactory();
    const TrayDeckView& d = vm.tray_deck;
    const ShelfPalette p = MakeShelfPalette(vm.dark, IsHighContrast(), theme);
    const float s = scale_;
    const TrayOpenGeom g = TrayOpenGeometry(d, rc, s,
        [this](const std::wstring& t, bool compact_text) { return ShelfMeasure(t, compact_text); });
    ComPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(p.text, &br);
    if (!br.get()) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br.get(); };
    auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), B(c));
    };
    auto hot = [&](TrayShelfHit h, int sub = -1) {
        return IsHovered(vm, HitTestResult::TrayShelf, static_cast<int>(h), sub);
    };
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);

    // .oh: 暂存区  N 项 · size              ⌄
    ShelfText(compositor_, ShelfFormat(3), B(p.strong), l10n::Get(StringId::TrayShelfTitle), g.title);
    std::wstring count = Fill(l10n::Get(StringId::TrayItemsFormat), L"{n}", std::to_wstring(d.total_count));
    if (d.total_size > 0) count += L" \u00B7 " + format::ByteSize(d.total_size, true);
    ShelfText(compositor_, ShelfFormat(1), B(p.sub), count, g.count);
    if (hot(TrayShelfHit::Collapse)) fill(g.collapse, 8.0f * s, p.ib_hot);
    DrawShelfIcon(dc, B(hot(TrayShelfHit::Collapse) ? p.strong : p.sub), ShelfIcon::Down, g.collapse, 14.0f * s);

    // .flt chips.
    const StringId names[4] = {StringId::TrayFilterAll, StringId::TrayFilterImages, StringId::TrayFilterDocs,
                               StringId::TrayFilterOther};
    for (size_t i = 0; i < g.filters.size(); ++i) {
        const int f = g.filter_id[i];
        const bool on = d.filter == f;
        const bool h = hot(TrayShelfHit::Filter, f);
        fill(g.filters[i], 12.0f * s, on ? Alpha(p.acc, vm.dark ? 0.26f : 0.18f) : (h ? p.ib_hot : p.chip));
        const std::wstring label = l10n::Get(names[f]) + L" " + std::to_wstring(d.filter_counts[f]);
        ShelfText(compositor_, ShelfFormat(on ? 7 : 4), B(on ? (vm.dark ? Rgba(0xE2E6FF) : p.acc) : p.sub), label,
                  g.filters[i], DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // .ol: grouped rows, clipped to the list.
    dc->PushAxisAlignedClip(g.list, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < g.rows.size(); ++i) {
        const TrayOpenRow& row = d.open_rows[i];
        const D2D1_RECT_F& rr = g.rows[i];
        const D2D1_RECT_F& hr = g.heads[i];
        const float top = row.group_start ? hr.top : rr.top;
        if (rr.bottom < g.list.top || top > g.list.bottom) continue;
        // .oi.in: fade + rise 10; .oi.out: slide left 30 + fade.
        const float in = EaseOutBack(row.in_t);
        const float out = row.out_t > 0.0f ? EaseOutCubic(row.out_t) : 0.0f;
        const float alpha = Clamp01(EaseOutCubic(row.in_t)) * (1.0f - out);
        if (alpha <= 0.01f) continue;
        const bool layered = alpha < 0.999f;
        dc->SetTransform(D2D1::Matrix3x2F::Translation(-30.0f * s * out, 10.0f * s * (1.0f - in)) * old);
        if (layered)
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                D2D1::IdentityMatrix(), alpha), nullptr);
        if (row.group_start) {
            // .og: folder 12x9, name (11 bold), 打开 › on the right.
            const float cy = hr.top + 8.0f * s + 7.5f * s;
            const D2D1_RECT_F fo = D2D1::RectF(hr.left + 6.0f * s, cy - 4.5f * s, hr.left + 18.0f * s, cy + 4.5f * s);
            fill(fo, 2.0f * s, p.folder);
            fill(D2D1::RectF(fo.left, fo.top - 1.5f * s, fo.left + 5.0f * s, fo.top + 2.0f * s), 1.0f * s, p.folder);
            const D2D1_RECT_F& orc = g.head_open[i];
            ShelfText(compositor_, ShelfFormat(9), B(p.faint), row.group,
                      D2D1::RectF(fo.right + 7.0f * s, hr.top + 8.0f * s, orc.left - 6.0f * s, hr.bottom - 4.0f * s));
            const bool oh = hot(TrayShelfHit::GroupOpen, static_cast<int>(i));
            ShelfText(compositor_, ShelfFormat(6), B(oh ? p.acc2 : p.faint), l10n::Get(StringId::TrayGroupOpen),
                      D2D1::RectF(orc.left, hr.top + 8.0f * s, orc.right, hr.bottom - 4.0f * s),
                      DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        if (rr.bottom - rr.top > 1.0f) {
            const bool h = hot(TrayShelfHit::Row, static_cast<int>(i));
            if (row.checked) fill(rr, 8.0f * s, Alpha(p.acc, vm.dark ? 0.24f : 0.14f));
            else if (h) fill(rr, 8.0f * s, p.ib_hot);
            const float cy = (rr.top + rr.bottom) * 0.5f;
            // .ck 16, r5: inset ring, or filled with a check (scale 1.08).
            const float ck = (row.checked ? 8.64f : 8.0f) * s;
            const D2D1_RECT_F box = D2D1::RectF(rr.left + 16.0f * s - ck, cy - ck, rr.left + 16.0f * s + ck, cy + ck);
            if (row.checked) {
                fill(box, 5.0f * s, p.acc2);
                DrawShelfIcon(dc, B(Rgba(0xFFFFFF)), ShelfIcon::Check, box, 10.0f * s);
            } else {
                const D2D1_RECT_F in_box = D2D1::RectF(box.left + 0.75f * s, box.top + 0.75f * s,
                                                       box.right - 0.75f * s, box.bottom - 0.75f * s);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(in_box, 4.25f * s, 4.25f * s),
                                         B(vm.dark ? Rgba(0xFFFFFF, 0.2f) : Rgba(0x000000, 0.25f)), 1.5f * s);
            }
            // Icon 16, gap 9.
            const float ix = rr.left + 8.0f * s + 16.0f * s + 9.0f * s;
            const float icon = std::round(16.0f * s);
            if (row.is_dir) {
                DrawFolderIcon(ix, cy - icon * 0.5f, icon, theme, row.exists ? 1.0f : 0.45f);
            } else if (ID2D1Bitmap* bitmap = icon_cache_.BitmapFor(row.exists ? row.path : L"", row.name, false,
                                                                   row.attrs, icon)) {
                const D2D1_RECT_F dest = D2D1::RectF(ix, cy - icon * 0.5f, ix + icon, cy + icon * 0.5f);
                dc->DrawBitmap(bitmap, &dest, row.exists ? 1.0f : 0.45f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                               nullptr, nullptr);
            }
            const float sw = row.size.empty() ? 0.0f : MeasureTextWidth(dw, ShelfFormat(4), row.size) + 2.0f * s;
            const D2D1_RECT_F sr = D2D1::RectF(rr.right - 8.0f * s - sw, rr.top, rr.right - 8.0f * s, rr.bottom);
            ShelfText(compositor_, ShelfFormat(10), B(row.exists ? (vm.dark ? Rgba(0xDFE3F0) : p.text) : p.faint), row.name,
                      D2D1::RectF(ix + icon + 9.0f * s, rr.top, sr.left - 9.0f * s, rr.bottom));
            if (sw > 0.0f) ShelfText(compositor_, ShelfFormat(4), B(p.faint), row.size, sr, DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        if (layered) dc->PopLayer();
        dc->SetTransform(old);
    }
    dc->PopAxisAlignedClip();

    // .of: top rule, selection line, split button.
    dc->DrawLine(D2D1::Point2F(g.footer.left, g.footer.top + 0.5f), D2D1::Point2F(g.footer.right, g.footer.top + 0.5f),
                 B(vm.dark ? Rgba(0xFFFFFF, 0.06f) : Rgba(0x000000, 0.07f)), 1.0f);
    if (d.checked > 0) {
        std::wstring prefix = Fill(l10n::Get(StringId::TraySelectedFormat), L"{n}", std::to_wstring(d.checked));
        prefix += L" \u00B7 ";
        ShelfText(compositor_, ShelfFormat(4), B(p.sub), prefix,
                  D2D1::RectF(g.selinfo.left, g.selinfo.top, g.none_link.left + 1.0f * s, g.selinfo.bottom));
        ShelfText(compositor_, ShelfFormat(7), B(hot(TrayShelfHit::SelNone) ? p.strong : (vm.dark ? Rgba(0xAAB5F5) : p.acc)),
                  l10n::Get(StringId::Cancel), D2D1::RectF(g.none_link.left, g.selinfo.top, g.none_link.right + 4.0f * s,
                  g.selinfo.bottom));
    } else {
        ShelfText(compositor_, ShelfFormat(4), B(p.sub),
                  Fill(l10n::Get(StringId::TrayNoneCheckedFormat), L"{n}", std::to_wstring(d.live_total)), g.selinfo);
    }
    if (d.checked == 2) {
        fill(g.cmp, 9.0f * s, Alpha(p.acc, (hot(TrayShelfHit::Compare) || d.cmp_shown) ? 0.5f : 0.3f));
        ShelfText(compositor_, ShelfFormat(4), B(vm.dark ? Rgba(0xFFFFFF) : p.strong), l10n::Get(StringId::TrayCompareThese),
                  g.cmp, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    {
        const D2D1_RECT_F sp = g.split;
        const float r = 9.0f * s;
        const bool usable = !d.target.empty() && d.target_count > 0;
        const D2D1_COLOR_F base = Mix(p.acc, p.move, d.mode_t);
        for (int l = 0; l < 3; ++l) {
            const float off = (3.0f + 1.5f * static_cast<float>(l)) * s;
            const float inset = (4.0f - 1.0f * static_cast<float>(l)) * s;
            fill(D2D1::RectF(sp.left + inset, sp.top + off, sp.right - inset, sp.bottom + off), r,
                 Alpha(base, (usable ? 0.16f : 0.06f) * (1.0f - 0.3f * static_cast<float>(l))));
        }
        fill(sp, r, usable ? base : Alpha(base, 0.45f));
        dc->DrawLine(D2D1::Point2F(sp.left + r * 0.6f, sp.top + 0.5f * s),
                     D2D1::Point2F(sp.right - r * 0.6f, sp.top + 0.5f * s), B(Rgba(0xFFFFFF, 0.14f)), 1.0f * s);
        dc->PushAxisAlignedClip(sp, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (usable && hot(TrayShelfHit::Release))
            fill(D2D1::RectF(g.go.left, g.go.top, g.go.right + r, g.go.bottom), r, Rgba(0xFFFFFF, 0.12f));
        if (hot(TrayShelfHit::ReleaseMenu))
            fill(D2D1::RectF(g.dd.left - r, g.dd.top, g.dd.right, g.dd.bottom), r, Rgba(0xFFFFFF, 0.12f));
        dc->PopAxisAlignedClip();
        dc->DrawLine(D2D1::Point2F(g.dd.left, g.dd.top), D2D1::Point2F(g.dd.left, g.dd.bottom),
                     B(Rgba(0xFFFFFF, 0.22f)), 1.0f);
        DrawShelfIcon(dc, B(p.on_acc), ShelfIcon::Down, g.dd, 14.0f * s);
        std::wstring label = !d.target.empty()
            ? Fill(Fill(l10n::Get(d.move_mode ? StringId::TrayMoveNToFormat : StringId::TrayCopyNToFormat), L"{n}",
                        std::to_wstring(d.target_count)), L"{name}", d.target)
            : l10n::Get(StringId::TrayNoTarget);
        const float lt = Clamp01(d.label_t);
        const float e = EaseOutBack(lt);
        const D2D1_RECT_F lr = D2D1::RectF(g.go.left + 8.0f * s, g.go.top + 6.0f * s * (1.0f - e),
                                           g.go.right - 8.0f * s, g.go.bottom + 6.0f * s * (1.0f - e));
        ShelfText(compositor_, ShelfFormat(2), B(Alpha(p.on_acc, (usable ? 1.0f : 0.75f) * EaseOutCubic(lt))), label, lr,
                  DWRITE_TEXT_ALIGNMENT_CENTER);
        if (d.conflicts > 0 && usable) {
            const std::wstring c = Fill(l10n::Get(StringId::TrayConflictsFormat), L"{n}", std::to_wstring(d.conflicts));
            const float cw = MeasureTextWidth(dw, ShelfFormat(5), c) + 12.0f * s;
            const D2D1_RECT_F cb = D2D1::RectF(sp.right + 5.0f * s - cw, sp.top - 8.0f * s, sp.right + 5.0f * s,
                                               sp.top + 8.0f * s);
            const float ring = 2.5f * s;
            fill(D2D1::RectF(cb.left - ring, cb.top - ring, cb.right + ring, cb.bottom + ring), 8.0f * s + ring, p.tray);
            fill(cb, 8.0f * s, p.warn);
            ShelfText(compositor_, ShelfFormat(5), B(Rgba(0x2A1800)), c, cb, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
}

void MainRenderer::DrawTrayCompareCard(const WindowViewModel& vm, const Theme& theme) {
    const TrayDeckView& d = vm.tray_deck;
    if (!d.cmp_shown || !compositor_ || !compositor_->Dc() || tray_panel_rc_.bottom <= tray_panel_rc_.top) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    const ShelfPalette p = MakeShelfPalette(vm.dark, IsHighContrast(), theme);
    const float s = scale_;
    D2D1_SIZE_F target = dc->GetSize();
    const TrayCardGeom g = TrayCompareCardGeometry(tray_panel_rc_, s, target.width);
    ComPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(p.text, &br);
    if (!br.get()) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br.get(); };
    auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), B(c));
    };
    auto hot = [&](TrayShelfHit h) { return IsHovered(vm, HitTestResult::TrayShelf, static_cast<int>(h)); };
    // #cmp.on: opacity .2 s, translateX(-14) scale(.97) -> none on (.2,1.3,.3,1) .45 s.
    const float t = Clamp01(d.cmp_t);
    const float e = EaseOutBack(t);
    const float a = EaseOutCubic(Clamp01(t * 0.45f / 0.2f));
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    const float k = 0.97f + 0.03f * e;
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(g.box.left, g.box.bottom)) *
                     D2D1::Matrix3x2F::Translation(-14.0f * s * (1.0f - e), 0.0f) * old);
    const bool layered = a < 0.999f;
    if (layered)
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), a), nullptr);
    const float r = 14.0f * s;
    // 0 30px 70px -10px rgba(0,0,0,.75): a few soft layers.
    for (int l = 0; l < 6; ++l) {
        const float grow = (2.0f + 5.0f * static_cast<float>(l)) * s;
        fill(D2D1::RectF(g.box.left - grow + 10.0f * s, g.box.top - grow + 24.0f * s, g.box.right + grow - 10.0f * s,
                         g.box.bottom + grow + 24.0f * s), r + grow, Rgba(0x000000, (vm.dark ? 0.11f : 0.04f)));
    }
    fill(g.box, r, vm.dark ? Rgba(0x1D222E) : Rgba(0xFFFFFF));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g.box.left + 0.5f, g.box.top + 0.5f, g.box.right - 0.5f,
        g.box.bottom - 0.5f), r, r), B(vm.dark ? Rgba(0xFFFFFF, 0.08f) : Rgba(0x000000, 0.10f)), 1.0f);
    const TrayCompareView& cv = d.compare;
    ShelfText(compositor_, ShelfFormat(0), B(p.strong), L"\u21C4 " + l10n::Get(StringId::TrayCompareTwo), g.title);
    if (hot(TrayShelfHit::CardClose)) fill(g.close, 6.0f * s, p.ib_hot);
    DrawShelfIcon(dc, B(p.sub), ShelfIcon::Close, g.close, 14.0f * s);
    const D2D1_COLOR_F key = vm.dark ? Rgba(0x7D869E) : p.faint;
    const D2D1_COLOR_F val = vm.dark ? Rgba(0xDFE3F0) : p.text;
    const D2D1_COLOR_F newer = vm.dark ? Rgba(0x7EE0A8) : Rgba(0x1F9D57);
    const D2D1_COLOR_F diff = vm.dark ? Rgba(0xFFCF8A) : Rgba(0xB8730E);
    auto cell = [&](int row, int col) {
        const float h = (row == 0 ? 20.0f : 18.0f) * s;
        const float y = g.row_y[row];
        if (col < 0) return D2D1::RectF(g.label_l, y, g.label_l + 64.0f * s, y + h);
        return D2D1::RectF(g.col_l[col], y, g.col_l[col] + g.col_w, y + h);
    };
    const StringId keys[3] = {StringId::TrayCmpLocation, StringId::TrayCmpModified, StringId::TrayCmpSize};
    for (int row = 1; row <= 3; ++row)
        ShelfText(compositor_, ShelfFormat(1), B(key), l10n::Get(keys[row - 1]), cell(row, -1));
    for (int c = 0; c < 2; ++c) {
        ShelfText(compositor_, ShelfFormat(7), B(p.strong), cv.name[c], cell(0, c));
        ShelfText(compositor_, ShelfFormat(1), B(val), cv.where[c], cell(1, c));
        const bool nw = cv.newer == c;
        ShelfText(compositor_, ShelfFormat(1), B(nw ? newer : val),
                  nw ? cv.time[c] + L" \u00B7 " + l10n::Get(StringId::TrayCmpNewer) : cv.time[c], cell(2, c));
        ShelfText(compositor_, ShelfFormat(1), B(cv.size_differs ? diff : val), cv.size[c], cell(3, c));
    }
    // .res: the content verdict (clickable while it can do something).
    std::wstring res;
    D2D1_COLOR_F bg = Alpha(p.acc, 0.14f), ink = vm.dark ? Rgba(0xC6CEFF) : p.acc;
    switch (cv.content) {
    case 0: res = l10n::Get(StringId::TrayCmpCheck); break;
    case 1: res = l10n::Get(StringId::TrayCmpChecking); break;
    case 2: res = l10n::Get(StringId::TrayCmpSame); bg = Alpha(newer, 0.14f); ink = newer; break;
    case 3:
        res = l10n::Get(cv.text == 1 ? StringId::TrayCmpViewDiff : (cv.text == 0 ? StringId::TrayCmpBinary
                                                                                  : StringId::TrayCmpDifferent));
        bg = Rgba(0xF2A93B, 0.12f);
        ink = diff;
        break;
    default: res = l10n::Get(StringId::TrayCmpFailed); bg = Rgba(0xF2A93B, 0.12f); ink = diff; break;
    }
    if (hot(TrayShelfHit::CardAction)) bg.a = std::min(1.0f, bg.a + 0.1f);
    fill(g.res, 9.0f * s, bg);
    ShelfText(compositor_, ShelfFormat(7), B(ink), res,
              D2D1::RectF(g.res.left + 11.0f * s, g.res.top, g.res.right - 11.0f * s, g.res.bottom));
    if (layered) dc->PopLayer();
    dc->SetTransform(old);
}

} // namespace pulse::ui
