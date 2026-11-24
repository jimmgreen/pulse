#include "shelf_shared.h"
#include "typography.h"
#include "window_helpers.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::ui {
namespace {
template<class T> using ShelfPtr = Microsoft::WRL::ComPtr<T>;
using l10n::StringId;

D2D1_COLOR_F Rgba(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }

D2D1_COLOR_F AlphaOf(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float EaseOutCubic(float t) { t = Clamp01(t); const float u = 1.0f - t; return 1.0f - u * u * u; }
float EaseOutBack(float t) {
    t = Clamp01(t);
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

std::wstring FillStr(std::wstring s, const wchar_t* key, const std::wstring& val) {
    if (const size_t at = s.find(key); at != std::wstring::npos)
        s.replace(at, wcslen(key), val);
    return s;
}

float MeasureWidth(IDWriteFactory2* dw, IDWriteTextFormat* fmt, const std::wstring& text) {
    if (!dw || !fmt || text.empty()) return 0.0f;
    ShelfPtr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                    fmt, 4000.0f, 200.0f, &layout)) || !layout)
        return 0.0f;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return 0.0f;
    return m.widthIncludingTrailingWhitespace;
}

void DrawOneLine(Compositor* comp, IDWriteTextFormat* fmt, ID2D1SolidColorBrush* br,
                 const std::wstring& text, const D2D1_RECT_F& rc,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    const float w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (!comp || !comp->Dc() || !fmt || !br || text.empty() || w <= 1.0f || h <= 1.0f) return;
    IDWriteFactory2* factory = comp->DwriteFactory();
    if (!factory) return;
    auto fg = br->GetColor();
    fg.a *= br->GetOpacity();
    if (!IsHighContrast() && comp->DrawLumaText(text, fmt, rc, fg, D2D1::ColorF(0, 0.0f), align))
        return;
    ShelfPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                         fmt, w, h, &layout)) || !layout)
        return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ShelfPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(layout.Get(), &ellipsis);
    layout->SetTrimming(&trimming, ellipsis.Get());
    comp->DrawTextLayout(D2D1::Point2F(rc.left, rc.top), layout.Get(), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

D2D1_COLOR_F HslToColor(float h_deg, float s_pct, float l_pct, float a = 1.0f) {
    float h = std::fmod(h_deg, 360.0f);
    if (h < 0.0f) h += 360.0f;
    const float s = std::clamp(s_pct * 0.01f, 0.0f, 1.0f);
    const float l = std::clamp(l_pct * 0.01f, 0.0f, 1.0f);
    const float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
    const float x = c * (1.0f - std::abs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = l - c * 0.5f;
    float r = 0, g = 0, b = 0;
    if (h < 60.0f) { r = c; g = x; }
    else if (h < 120.0f) { r = x; g = c; }
    else if (h < 180.0f) { g = c; b = x; }
    else if (h < 240.0f) { g = x; b = c; }
    else if (h < 300.0f) { r = x; b = c; }
    else { r = c; b = x; }
    return {r + m, g + m, b + m, a};
}

std::wstring DocExtLabel(const std::wstring& name, uint32_t* out_rgb) {
    const size_t dot = name.find_last_of(L'.');
    std::wstring ext = (dot != std::wstring::npos && dot + 1 < name.size()) ? name.substr(dot + 1) : L"";
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towupper(c));
    if (ext == L"PDF") { *out_rgb = 0xD93B3B; return L"PDF"; }
    if (ext == L"XLS" || ext == L"XLSX" || ext == L"CSV") { *out_rgb = 0x1E8E4E; return L"XLS"; }
    if (ext == L"DOC" || ext == L"DOCX") { *out_rgb = 0x2B63C6; return L"DOC"; }
    if (ext == L"PPT" || ext == L"PPTX") { *out_rgb = 0xD2522E; return L"PPT"; }
    if (ext == L"ZIP" || ext == L"7Z" || ext == L"RAR") { *out_rgb = 0x8C6BB1; return L"ZIP"; }
    if (ext == L"URL") { *out_rgb = 0x5B6CF0; return L"URL"; }
    if (ext == L"TXT" || ext == L"MD" || ext == L"LOG") { *out_rgb = 0x6B7685; return L"TXT"; }
    *out_rgb = 0x6B7685;
    if (ext.empty()) return L"FILE";
    if (ext.size() > 4) ext.resize(4);
    return ext;
}

bool IsImageFileName(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = name.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".jpg" || ext == L".jpeg" || ext == L".png" ||
           ext == L".webp" || ext == L".gif" || ext == L".bmp";
}

} // namespace

ShelfColors MakeShelfColors(bool dark, bool hc, const Theme& theme) {
    ShelfColors p{};
    p.acc = theme.accent;
    p.acc.a = 1.0f;
    p.acc2 = dark ? MixColor(p.acc, Rgba(0xFFFFFF), 0.28f) : p.acc;
    p.move = Rgba(dark ? 0xE08A48 : 0xD9772F);
    p.move_soft = Rgba(dark ? 0xE08A48 : 0xD9772F, dark ? 0.22f : 0.16f);
    p.warn = Rgba(dark ? 0xF2A93B : 0xE09A2B);
    p.ok = Rgba(0x2AA05A);
    p.danger = Rgba(dark ? 0xFF8A7A : 0xC42B1C);
    p.on_acc = Rgba(0xFFFFFF);
    p.folder = Rgba(0xF5B942);
    p.paper = Rgba(0xF6F7F9);
    if (hc) {
        p.tray = theme.surface_flyout;
        p.tray_edge = theme.text;
        p.shelf_bg = theme.surface_flyout;
        p.shelf_solid = theme.surface_flyout;
        p.shelf_edge = theme.text;
        p.strong = p.text = theme.text;
        p.sub = p.faint = theme.text_secondary;
        p.chip = p.ib_hot = p.kbd = p.rm_bg = theme.fill_hover;
        p.chip_hot = p.sel = p.sel_strong = theme.fill_selected;
        p.half_edge = p.ck_edge = p.div = theme.text;
        return p;
    }
    if (dark) {
        p.tray = MixColor(Rgba(0x121925), p.acc, 0.02f);
        p.tray_edge = AlphaOf(p.acc2, 0.32f);
        p.shelf_bg = Rgba(0x1C2028, 0.93f);
        p.shelf_solid = Rgba(0x1F242D);
        p.shelf_edge = Rgba(0xFFFFFF, 0.09f);
        p.strong = Rgba(0xF2F2F2);
        p.text = Rgba(0xE6E9F2);
        p.sub = Rgba(0xFFFFFF, 0.62f);
        p.faint = Rgba(0xFFFFFF, 0.42f);
        p.chip = Rgba(0xFFFFFF, 0.06f);
        p.chip_hot = AlphaOf(p.acc2, 0.26f);
        p.ib_hot = Rgba(0xFFFFFF, 0.08f);
        p.kbd = Rgba(0xFFFFFF, 0.07f);
        p.sel = AlphaOf(p.acc2, 0.16f);
        p.sel_strong = AlphaOf(p.acc2, 0.30f);
        p.half_edge = AlphaOf(p.acc2, 0.38f);
        p.ck_edge = Rgba(0xFFFFFF, 0.30f);
        p.rm_bg = Rgba(0xFFFFFF, 0.12f);
        p.div = Rgba(0xFFFFFF, 0.07f);
    } else {
        p.tray = MixColor(Rgba(0xEEF2F8), p.acc, 0.03f);
        p.tray_edge = AlphaOf(p.acc, 0.26f);
        p.shelf_bg = Rgba(0xF6F8FC, 0.92f);
        p.shelf_solid = Rgba(0xF3F5FA);
        p.shelf_edge = Rgba(0x000000, 0.08f);
        p.strong = Rgba(0x232B3B);
        p.text = Rgba(0x303B50);
        p.sub = Rgba(0x66748A);
        p.faint = Rgba(0x929CAE);
        p.chip = Rgba(0x000000, 0.05f);
        p.chip_hot = AlphaOf(p.acc, 0.14f);
        p.ib_hot = Rgba(0x000000, 0.06f);
        p.kbd = Rgba(0x000000, 0.06f);
        p.sel = AlphaOf(p.acc, 0.09f);
        p.sel_strong = AlphaOf(p.acc, 0.17f);
        p.half_edge = AlphaOf(p.acc, 0.32f);
        p.ck_edge = Rgba(0x000000, 0.28f);
        p.rm_bg = Rgba(0x000000, 0.08f);
        p.div = Rgba(0x000000, 0.07f);
    }
    return p;
}

void DrawShelfGlyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br,
                    ShelfGlyph glyph, const D2D1_RECT_F& box, float px) {
    if (!dc || !br) return;
    ShelfPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    if (!factory) return;
    const float u = px / 16.0f;
    const float ox = (box.left + box.right - px) * 0.5f;
    const float oy = (box.top + box.bottom - px) * 0.5f;
    auto P = [&](float x, float y) { return D2D1::Point2F(ox + x * u, oy + y * u); };
    ShelfPtr<ID2D1StrokeStyle> st;
    factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND), nullptr, 0, &st);
    const float sw = std::max(1.0f, 1.45f * u);
    auto line = [&](float x1, float y1, float x2, float y2, float w = 0.0f) {
        dc->DrawLine(P(x1, y1), P(x2, y2), br, w > 0.0f ? w : sw, st.Get());
    };
    auto poly = [&](std::initializer_list<D2D1_POINT_2F> pts, bool closed, float w = 0.0f) {
        ShelfPtr<ID2D1PathGeometry> g;
        if (FAILED(factory->CreatePathGeometry(&g)) || !g) return;
        ShelfPtr<ID2D1GeometrySink> sk;
        if (FAILED(g->Open(&sk)) || !sk) return;
        auto it = pts.begin();
        sk->BeginFigure(*it++, D2D1_FIGURE_BEGIN_HOLLOW);
        for (; it != pts.end(); ++it) sk->AddLine(*it);
        sk->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sk->Close();
        dc->DrawGeometry(g.Get(), br, w > 0.0f ? w : sw, st.Get());
    };

    switch (glyph) {
    case ShelfGlyph::Tray:
        poly({P(2.0f, 9.5f), P(5.2f, 9.5f), P(6.2f, 11.1f), P(9.8f, 11.1f), P(10.8f, 9.5f), P(14.0f, 9.5f)}, false);
        poly({P(3.6f, 3.5f), P(12.4f, 3.5f), P(14.0f, 9.5f), P(14.0f, 12.6f), P(2.0f, 12.6f), P(2.0f, 9.5f)}, true);
        break;
    case ShelfGlyph::Chev:
    case ShelfGlyph::Down:
        poly({P(4.5f, 6.5f), P(8.0f, 10.0f), P(11.5f, 6.5f)}, false);
        break;
    case ShelfGlyph::Up:
        poly({P(4.5f, 9.5f), P(8.0f, 6.0f), P(11.5f, 9.5f)}, false);
        break;
    case ShelfGlyph::Pin:
        poly({P(9.8f, 2.2f), P(13.8f, 6.2f), P(11.6f, 7.1f), P(9.4f, 9.3f),
              P(9.6f, 12.0f), P(4.0f, 6.4f), P(6.7f, 6.6f), P(8.9f, 4.4f)}, true);
        line(6.2f, 9.8f, 2.5f, 13.5f);
        break;
    case ShelfGlyph::More:
        for (float cx : {3.5f, 8.0f, 12.5f})
            dc->FillEllipse(D2D1::Ellipse(P(cx, 8.0f), 1.25f * u, 1.25f * u), br);
        break;
    case ShelfGlyph::Dock:
        dc->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(ox + 2.0f * u, oy + 2.5f * u, ox + 14.0f * u, oy + 13.5f * u),
            1.5f * u, 1.5f * u), br, sw);
        line(6.0f, 2.5f, 6.0f, 13.5f);
        line(12.0f, 8.0f, 8.6f, 8.0f);
        poly({P(10.1f, 6.5f), P(8.6f, 8.0f), P(10.1f, 9.5f)}, false);
        break;
    case ShelfGlyph::Popout:
        poly({P(9.0f, 2.5f), P(13.5f, 2.5f), P(13.5f, 7.0f)}, false);
        line(13.5f, 2.5f, 8.0f, 8.0f);
        poly({P(7.0f, 4.5f), P(3.0f, 4.5f), P(3.0f, 13.0f), P(12.0f, 13.0f), P(12.0f, 9.5f)}, false);
        break;
    case ShelfGlyph::Copy:
        dc->DrawRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(ox + 5.5f * u, oy + 5.5f * u, ox + 13.5f * u, oy + 13.5f * u),
            1.5f * u, 1.5f * u), br, sw);
        poly({P(10.5f, 5.5f), P(10.5f, 3.0f), P(3.0f, 3.0f), P(3.0f, 10.5f), P(5.5f, 10.5f)}, false);
        break;
    case ShelfGlyph::Move:
        line(2.5f, 8.0f, 11.0f, 8.0f);
        poly({P(8.0f, 4.5f), P(11.5f, 8.0f), P(8.0f, 11.5f)}, false);
        line(14.0f, 3.0f, 14.0f, 13.0f);
        break;
    case ShelfGlyph::Plus:
        line(8.0f, 3.0f, 8.0f, 13.0f);
        line(3.0f, 8.0f, 13.0f, 8.0f);
        break;
    case ShelfGlyph::Grid:
        for (float gx : {2.5f, 9.0f})
            for (float gy : {2.5f, 9.0f})
                dc->DrawRoundedRectangle(D2D1::RoundedRect(
                    D2D1::RectF(ox + gx * u, oy + gy * u, ox + (gx + 4.5f) * u, oy + (gy + 4.5f) * u),
                    1.0f * u, 1.0f * u), br, sw);
        break;
    case ShelfGlyph::List:
        for (float ly : {4.0f, 8.0f, 12.0f}) {
            dc->FillEllipse(D2D1::Ellipse(P(2.8f, ly), 1.0f * u, 1.0f * u), br);
            line(5.5f, ly, 13.5f, ly);
        }
        break;
    case ShelfGlyph::Check:
        poly({P(3.2f, 8.4f), P(6.4f, 11.6f), P(12.8f, 4.4f)}, false, 2.1f * u);
        break;
    case ShelfGlyph::Close:
        line(4.5f, 4.5f, 11.5f, 11.5f, 1.8f * u);
        line(11.5f, 4.5f, 4.5f, 11.5f, 1.8f * u);
        break;
    }
}

void DrawShelfItemThumb(ID2D1DeviceContext* dc, Compositor* comp,
                        IDWriteTextFormat* badge_fmt,
                        ThumbnailCache* thumbs, ShellIconCache* icons,
                        const std::wstring& path, const std::wstring& name,
                        const std::wstring& domain, DWORD attrs, bool is_dir,
                        bool missing, uint8_t content_kind, uint64_t id_seed,
                        const D2D1_RECT_F& slot, float size_px, float scale) {
    if (!dc) return;
    ShelfPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(Rgba(0xFFFFFF), &br);
    if (!br) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br.Get(); };
    const float alpha = missing ? 0.42f : 1.0f;
    const float cx = (slot.left + slot.right) * 0.5f;
    const float cy = (slot.top + slot.bottom) * 0.5f;

    // 1. Directory (.th.dir)
    if (is_dir) {
        const float w = size_px, h = size_px * 0.76f;
        const D2D1_RECT_F body = D2D1::RectF(cx - w * 0.5f, cy - h * 0.44f, cx + w * 0.5f, cy + h * 0.56f);
        const D2D1_RECT_F tab = D2D1::RectF(body.left, body.top - size_px * 0.10f,
                                            body.left + w * 0.42f, body.top + size_px * 0.06f);
        dc->FillRoundedRectangle(D2D1::RoundedRect(tab, 2.0f * scale, 2.0f * scale), B(Rgba(0xE0A93A, alpha)));
        dc->FillRoundedRectangle(D2D1::RoundedRect(body, 3.0f * scale, 3.0f * scale), B(Rgba(0xF0C35A, alpha)));
        return;
    }

    // 2. Text snippet (.th.snip: cream lined note + gold quote mark)
    if (content_kind == 2) {
        const float w = size_px * 0.80f, h = size_px;
        const D2D1_RECT_F card = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
        const float rr = 3.0f * scale;
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, rr, rr), B(Rgba(0xFFF7DD, alpha)));
        const float step = std::max(3.0f, size_px * 0.16f);
        for (float y = card.top + h * 0.36f; y < card.bottom - 3.0f * scale; y += step) {
            dc->DrawLine(D2D1::Point2F(card.left + 3.0f * scale, y),
                         D2D1::Point2F(card.right - 3.0f * scale, y),
                         B(Rgba(0x8C6E28, 0.22f * alpha)), 1.0f);
        }
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
            card.left + 0.5f, card.top + 0.5f, card.right - 0.5f, card.bottom - 0.5f), rr, rr),
            B(Rgba(0xA07814, 0.22f * alpha)), 1.0f);
        if (comp && comp->DwriteFactory()) {
            ShelfPtr<IDWriteTextFormat> qfmt;
            typography::CreateTextFormat(comp->DwriteFactory(),
                {typography::FontRole::Text, std::max(10.0f, size_px * 0.42f), DWRITE_FONT_WEIGHT_BOLD}, &qfmt);
            if (qfmt) {
                DrawOneLine(comp, qfmt.Get(), B(Rgba(0xC9962A, alpha)), L"\u201C",
                            D2D1::RectF(card.left + w * 0.12f, card.top, card.right, card.top + h * 0.42f));
            }
        }
        return;
    }

    // 3. Link snippet (.th.link: #5B8DEF -> #7A5AF5 rounded square + domain initial)
    if (content_kind == 3) {
        const float s = size_px * 0.88f;
        const D2D1_RECT_F card = D2D1::RectF(cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f);
        const float rr = s * 0.24f;
        D2D1_GRADIENT_STOP stops[2] = {
            {0.0f, Rgba(0x5B8DEF, alpha)},
            {1.0f, Rgba(0x7A5AF5, alpha)},
        };
        ShelfPtr<ID2D1GradientStopCollection> sc;
        ShelfPtr<ID2D1LinearGradientBrush> gb;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(card.left, card.top),
                                                    D2D1::Point2F(card.right, card.bottom)),
                sc.Get(), &gb)) && gb) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(card, rr, rr), gb.Get());
        } else {
            dc->FillRoundedRectangle(D2D1::RoundedRect(card, rr, rr), B(Rgba(0x6874F2, alpha)));
        }
        std::wstring ch = !domain.empty() ? domain.substr(0, 1) : (!name.empty() ? name.substr(0, 1) : L"W");
        ch[0] = static_cast<wchar_t>(towupper(ch[0]));
        if (comp && comp->DwriteFactory()) {
            ShelfPtr<IDWriteTextFormat> lfmt;
            typography::CreateTextFormat(comp->DwriteFactory(),
                {typography::FontRole::Text, std::max(9.0f, s * 0.50f), DWRITE_FONT_WEIGHT_BOLD}, &lfmt);
            if (lfmt) {
                DrawOneLine(comp, lfmt.Get(), B(Rgba(0xFFFFFF, alpha)), ch, card,
                            DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        return;
    }

    // 4. Image (.th.img): try real ThumbnailCache bitmap first, else scenic HSL card.
    const bool is_img = content_kind == 1 || IsImageFileName(name);
    if (is_img) {
        const float w = size_px, h = size_px * 0.78f;
        const D2D1_RECT_F img_rc = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
        const float rr = 3.0f * scale;
        ShelfPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ShelfPtr<ID2D1RoundedRectangleGeometry> clip;
        if (factory) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(img_rc, rr, rr), &clip);
        if (clip) dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha), nullptr);
        bool drew = false;
        if (!missing && thumbs && !path.empty()) {
            drew = thumbs->Draw(dc, img_rc, path, attrs, 96, 0, 0, 0, 1.0f) == PreviewDrawResult::Bitmap;
        }
        if (!drew) {
            const float hue = static_cast<float>((id_seed * 47ull + name.size() * 29ull) % 360ull);
            D2D1_GRADIENT_STOP stops[2] = {
                {0.0f, HslToColor(hue, 72.0f, 74.0f)},
                {1.0f, HslToColor(hue + 38.0f, 58.0f, 52.0f)},
            };
            ShelfPtr<ID2D1GradientStopCollection> sc;
            ShelfPtr<ID2D1LinearGradientBrush> gb;
            if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
                SUCCEEDED(dc->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(img_rc.left, img_rc.top),
                                                        D2D1::Point2F(img_rc.right, img_rc.bottom)),
                    sc.Get(), &gb)) && gb) {
                dc->FillRectangle(img_rc, gb.Get());
            } else {
                dc->FillRectangle(img_rc, B(HslToColor(hue, 65.0f, 62.0f)));
            }
            dc->FillEllipse(D2D1::Ellipse(
                D2D1::Point2F(img_rc.right - w * 0.26f, img_rc.top + h * 0.27f),
                w * 0.10f, w * 0.10f), B(Rgba(0xFFFFFF, 0.85f)));
            dc->FillEllipse(D2D1::Ellipse(
                D2D1::Point2F(cx, img_rc.bottom + h * 0.05f),
                w * 0.65f, h * 0.42f), B(HslToColor(hue + 80.0f, 35.0f, 30.0f, 0.50f)));
        }
        if (clip) dc->PopLayer();
        return;
    }

    // 5. Document / regular file (.th.doc: folded corner + colored type strip)
    const float w = size_px * 0.80f, h = size_px;
    const D2D1_RECT_F doc = D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
    const float rr = 3.0f * scale;
    dc->FillRoundedRectangle(D2D1::RoundedRect(doc, rr, rr), B(Rgba(0xF6F7F9, alpha)));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
        doc.left + 0.5f, doc.top + 0.5f, doc.right - 0.5f, doc.bottom - 0.5f), rr, rr),
        B(Rgba(0x000000, 0.09f * alpha)), 1.0f);
    // Folded top-right corner
    const float fold = std::max(3.0f, w * 0.28f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(doc.right - fold, doc.top, doc.right, doc.top + fold * 0.85f),
        2.0f * scale, 2.0f * scale), B(Rgba(0xCFD3DA, alpha)));

    if (icons && size_px >= 24.0f * scale) {
        const float ic = std::round(std::min(14.0f * scale, w * 0.48f));
        if (ID2D1Bitmap* bmp = icons->BitmapFor(missing ? L"" : path, name, false, attrs, ic)) {
            const float ix = cx - ic * 0.5f;
            const float iy = doc.top + h * 0.20f;
            const D2D1_RECT_F dest = D2D1::RectF(ix, iy, ix + ic, iy + ic);
            dc->DrawBitmap(bmp, &dest, alpha, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
    }

    uint32_t chip_rgb = 0x6B7685;
    const std::wstring label = DocExtLabel(name, &chip_rgb);
    const float sh = std::max(4.0f * scale, h * 0.26f);
    const D2D1_RECT_F strip = D2D1::RectF(doc.left + w * 0.12f, doc.bottom - h * 0.12f - sh,
                                          doc.right - w * 0.12f, doc.bottom - h * 0.12f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(strip, 2.0f * scale, 2.0f * scale), B(Rgba(chip_rgb, alpha)));
    if (size_px >= 24.0f * scale && comp && badge_fmt) {
        DrawOneLine(comp, badge_fmt, B(Rgba(0xFFFFFF, alpha)), label,
                    D2D1::RectF(strip.left - 2.0f * scale, strip.top - 1.0f * scale,
                                strip.right + 2.0f * scale, strip.bottom + 1.0f * scale),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void DrawShelfPile(ID2D1DeviceContext* dc, Compositor* comp,
                   IDWriteTextFormat* badge_fmt, IDWriteTextFormat* count_fmt,
                   ThumbnailCache* thumbs, ShellIconCache* icons,
                   const TrayDeckView& d, const ShelfColors& p,
                   const D2D1_COLOR_F& ring_color,
                   const D2D1_RECT_F& fan_rc, float scale) {
    if (!dc) return;
    ShelfPtr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(p.acc, &br);
    if (!br) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br.Get(); };

    const bool lifted = d.dragging_out;
    if (lifted) {
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 0.30f), nullptr);
    }

    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    const float b = Clamp01(d.bump);
    float bump = 0.0f;
    if (b > 0.0f && b < 1.0f)
        bump = b < 0.3f ? EaseOutCubic(b / 0.3f) : 1.0f - EaseOutCubic((b - 0.3f) / 0.7f);
    const float bs = 1.0f + 0.16f * bump;
    const D2D1_MATRIX_3X2_F fan_m = D2D1::Matrix3x2F::Scale(bs, bs,
        D2D1::Point2F((fan_rc.left + fan_rc.right) * 0.5f, (fan_rc.top + fan_rc.bottom) * 0.5f)) *
        D2D1::Matrix3x2F::Translation(0.0f, -3.0f * scale * bump) * old;

    const int n = std::min(3, static_cast<int>(d.fan.size()));
    const float spread = EaseOutBack(d.fan_spread);
    static constexpr float kP3[3][4] = {{-7.0f, -12.0f, -15.0f, -18.0f},
                                        {0.0f, -3.0f, 0.0f, -1.0f},
                                        {7.0f, 8.0f, 15.0f, 15.0f}};
    static constexpr float kP2[2][4] = {{-4.0f, -7.0f, -10.0f, -13.0f},
                                        {4.0f, 6.0f, 10.0f, 11.0f}};
    const float cw = 36.0f * scale, ch = 46.0f * scale;
    const float base_x = fan_rc.left + ((fan_rc.right - fan_rc.left) - cw) * 0.5f;
    const float base_y = fan_rc.top + ((fan_rc.bottom - fan_rc.top) - ch) * 0.5f - 1.0f * scale;

    for (int k = 0; k < n; ++k) {
        const TrayCardView& c = d.fan[static_cast<size_t>(k)];
        const float* pose = n == 1 ? nullptr : (n == 2 ? kP2[k] : kP3[k]);
        const float x0 = pose ? pose[0] : 0.0f;
        const float r0 = pose ? pose[1] : 0.0f;
        const float x1 = pose ? pose[2] : 0.0f;
        const float r1 = pose ? pose[3] : -4.0f;
        const float tx = (x0 + (x1 - x0) * spread) * scale;
        const float rot = r0 + (r1 - r0) * spread;
        const D2D1_RECT_F card = D2D1::RectF(base_x, base_y, base_x + cw, base_y + ch);
        const D2D1_POINT_2F center = D2D1::Point2F((card.left + card.right) * 0.5f,
                                                   (card.top + card.bottom) * 0.5f);
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(rot, center) *
                         D2D1::Matrix3x2F::Translation(tx, 0.0f) * fan_m);
        const float rr = 5.0f * scale;
        // Soft card shadow
        for (int l = 2; l >= 1; --l) {
            const float g = static_cast<float>(l) * 1.4f * scale;
            dc->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(card.left - g, card.top - g + 2.5f * scale,
                            card.right + g, card.bottom + g + 2.5f * scale), rr + g, rr + g),
                B(Rgba(0x000000, 0.10f * (1.0f - 0.3f * static_cast<float>(l - 1)))));
        }
        dc->FillRoundedRectangle(D2D1::RoundedRect(card, rr, rr), B(p.paper));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(card, rr, rr), B(Rgba(0x000000, 0.06f)), 1.0f);
        DrawShelfItemThumb(dc, comp, badge_fmt, thumbs, icons,
                           c.path, c.name, c.domain, c.attrs, c.is_dir, c.missing,
                           c.content_kind, static_cast<uint64_t>(k + 1), card, 30.0f * scale, scale);
    }

    dc->SetTransform(fan_m);
    const int live_cnt = std::max(0, d.total_count - d.stale_count);
    const std::wstring count_str = std::to_wstring(live_cnt);
    const float tw = comp ? MeasureWidth(comp->DwriteFactory(), count_fmt, count_str) : 8.0f * scale;
    const float bw = std::max(20.0f * scale, tw + 12.0f * scale);
    const D2D1_RECT_F badge = D2D1::RectF(fan_rc.right - bw + 2.0f * scale, fan_rc.bottom - 20.0f * scale,
                                          fan_rc.right + 2.0f * scale, fan_rc.bottom);
    const float ring = 2.5f * scale;
    dc->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(badge.left - ring, badge.top - ring, badge.right + ring, badge.bottom + ring),
        10.0f * scale + ring, 10.0f * scale + ring), B(ring_color));
    dc->FillRoundedRectangle(D2D1::RoundedRect(badge, 10.0f * scale, 10.0f * scale),
                             B(Rgba(d.active_shelf_color)));
    if (comp && count_fmt)
        DrawOneLine(comp, count_fmt, B(p.on_acc), count_str, badge, DWRITE_TEXT_ALIGNMENT_CENTER);
    dc->SetTransform(old);
    if (lifted) dc->PopLayer();
}

void DrawShelfMeta(Compositor* comp, ID2D1SolidColorBrush* br,
                   IDWriteTextFormat* strong_fmt, IDWriteTextFormat* size_fmt,
                   IDWriteTextFormat* sub_fmt, IDWriteTextFormat* link_fmt,
                   const TrayDeckView& d, const ShelfColors& p,
                   const D2D1_RECT_F& meta_rc, float scale, bool stale_hot,
                   D2D1_RECT_F* out_stale_remove) {
    if (out_stale_remove) *out_stale_remove = D2D1::RectF();
    if (!comp || !br) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br; };
    IDWriteFactory2* dw = comp->DwriteFactory();
    const float y = meta_rc.top + 8.0f * scale;
    const int live_cnt = std::max(0, d.total_count - d.stale_count);
    const std::wstring cnt_text = FillStr(l10n::Get(StringId::TrayItemsFormat), L"{n}", std::to_wstring(live_cnt));
    const float cw = std::min(meta_rc.right - meta_rc.left, MeasureWidth(dw, strong_fmt, cnt_text) + 2.0f * scale);
    DrawOneLine(comp, strong_fmt, B(p.strong), cnt_text,
                D2D1::RectF(meta_rc.left, y, meta_rc.left + cw, y + 20.0f * scale));
    if (d.total_size > 0) {
        DrawOneLine(comp, size_fmt, B(p.sub), format::ByteSize(d.total_size, true),
                    D2D1::RectF(meta_rc.left + cw + 6.0f * scale, y + 1.0f * scale, meta_rc.right, y + 20.0f * scale));
    }
    const D2D1_RECT_F line2 = D2D1::RectF(meta_rc.left, y + 21.0f * scale, meta_rc.right, y + 38.0f * scale);
    if (d.stale_count > 0) {
        std::wstring warn_txt = FillStr(l10n::Get(StringId::TrayStaleFormat), L"{n}", std::to_wstring(d.stale_count));
        const std::wstring sep = L" \u00B7 ";
        const std::wstring rm = l10n::Get(StringId::TrayStaleRemove);
        const float ww = MeasureWidth(dw, sub_fmt, warn_txt);
        const float sw = MeasureWidth(dw, sub_fmt, sep);
        const float rw = MeasureWidth(dw, link_fmt ? link_fmt : sub_fmt, rm) + 4.0f * scale;
        if (line2.left + ww + sw + rw <= line2.right + 4.0f * scale) {
            DrawOneLine(comp, sub_fmt, B(p.warn), warn_txt,
                        D2D1::RectF(line2.left, line2.top, line2.left + ww + 2.0f, line2.bottom));
            DrawOneLine(comp, sub_fmt, B(p.faint), sep,
                        D2D1::RectF(line2.left + ww, line2.top, line2.left + ww + sw + 2.0f, line2.bottom));
            const D2D1_RECT_F rm_rc = D2D1::RectF(line2.left + ww + sw, line2.top,
                                                  std::min(line2.right, line2.left + ww + sw + rw), line2.bottom);
            DrawOneLine(comp, link_fmt ? link_fmt : sub_fmt, B(stale_hot ? p.strong : p.acc2), rm, rm_rc);
            if (out_stale_remove) *out_stale_remove = rm_rc;
        } else {
            DrawOneLine(comp, sub_fmt, B(p.warn), warn_txt, line2);
        }
    } else {
        DrawOneLine(comp, sub_fmt, B(p.faint), d.sources_text, line2);
    }
}

void DrawShelfReleaseButton(ID2D1DeviceContext* dc, Compositor* comp,
                            ID2D1SolidColorBrush* br,
                            IDWriteTextFormat* btn_fmt, IDWriteTextFormat* badge_fmt,
                            IDWriteTextFormat* note_fmt,
                            const TrayDeckView& d, const ShelfColors& p,
                            const D2D1_COLOR_F& ring_color,
                            const D2D1_RECT_F& sp, const D2D1_RECT_F& go_rc,
                            const D2D1_RECT_F& dd_rc, float scale,
                            bool compact_sidebar, bool hot_go, bool hot_dd,
                            float* out_bottom_y) {
    if (out_bottom_y) *out_bottom_y = sp.bottom;
    if (!dc || !br) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br; };
    const float r = 9.0f * scale;
    const int c = d.release_copy_count;
    const int m = d.release_move_count;
    const int total = c + m;
    const bool has_target = !d.target.empty();
    const bool usable = has_target && total > 0;
    const bool all_move = m > 0 && c == 0;
    const bool mixed = c > 0 && m > 0;

    // Soft under-glow
    const D2D1_COLOR_F glow_base = all_move ? p.move : p.acc;
    for (int l = 0; l < 3; ++l) {
        const float off = (3.0f + 1.5f * static_cast<float>(l)) * scale;
        const float inset = (4.0f - 1.0f * static_cast<float>(l)) * scale;
        dc->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(sp.left + inset, sp.top + off, sp.right - inset, sp.bottom + off), r, r),
            B(AlphaOf(glow_base, (usable ? 0.16f : 0.05f) * (1.0f - 0.3f * static_cast<float>(l)))));
    }

    const float btn_alpha = usable ? 1.0f : 0.48f;
    if (mixed) {
        const float ratio = static_cast<float>(c) / static_cast<float>(total);
        const float s0 = std::clamp(ratio - 0.18f, 0.0f, 1.0f);
        const float s1 = std::clamp(ratio + 0.18f, 0.0f, 1.0f);
        D2D1_GRADIENT_STOP stops[2] = {
            {s0, AlphaOf(p.acc, btn_alpha)},
            {s1, AlphaOf(p.move, btn_alpha)},
        };
        ShelfPtr<ID2D1GradientStopCollection> sc;
        ShelfPtr<ID2D1LinearGradientBrush> gb;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &sc)) && sc &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(sp.left, sp.top),
                                                    D2D1::Point2F(sp.right, sp.top)),
                sc.Get(), &gb)) && gb) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(sp, r, r), gb.Get());
        } else {
            dc->FillRoundedRectangle(D2D1::RoundedRect(sp, r, r), B(AlphaOf(p.acc, btn_alpha)));
        }
    } else {
        const D2D1_COLOR_F base = MixColor(p.acc, p.move, all_move ? 1.0f : d.mode_t);
        dc->FillRoundedRectangle(D2D1::RoundedRect(sp, r, r), B(AlphaOf(base, btn_alpha)));
    }

    // Top specular highlight
    dc->DrawLine(D2D1::Point2F(sp.left + r * 0.6f, sp.top + 0.5f * scale),
                 D2D1::Point2F(sp.right - r * 0.6f, sp.top + 0.5f * scale),
                 B(Rgba(0xFFFFFF, 0.15f)), 1.0f * scale);

    // Clipped hover segments
    ShelfPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ShelfPtr<ID2D1RoundedRectangleGeometry> clip;
    if (factory) factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(sp, r, r), &clip);
    if (clip) dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), nullptr);
    if (usable && hot_go) dc->FillRectangle(go_rc, B(Rgba(0xFFFFFF, 0.13f)));
    if (hot_dd) dc->FillRectangle(dd_rc, B(Rgba(0xFFFFFF, 0.13f)));
    if (clip) dc->PopLayer();

    dc->DrawLine(D2D1::Point2F(dd_rc.left, dd_rc.top), D2D1::Point2F(dd_rc.left, dd_rc.bottom),
                 B(Rgba(0xFFFFFF, 0.24f)), 1.0f);
    DrawShelfGlyph(dc, B(p.on_acc), ShelfGlyph::Chev, dd_rc, 12.0f * scale);

    // Main label
    std::wstring label;
    if (!has_target) {
        label = l10n::Get(StringId::TrayNoTarget);
    } else if (total <= 0) {
        label = l10n::Get(StringId::ShelfReleaseNone);
    } else if (mixed) {
        if (compact_sidebar) {
            label = FillStr(l10n::Get(StringId::ShelfReleaseMixedShortFormat), L"{name}", d.target);
        } else {
            auto format = FillStr(l10n::Get(StringId::ShelfReleaseMixedFullFormat), L"{c}", std::to_wstring(c));
            format = FillStr(std::move(format), L"{m}", std::to_wstring(m));
            label = FillStr(format, L"{name}", d.target);
            if (comp && btn_fmt) {
                const float width = go_rc.right - go_rc.left - 16.0f * scale;
                const auto fits = [&](const std::wstring& text) {
                    return MeasureWidth(comp->DwriteFactory(), btn_fmt, text) <= width;
                };
                // Keep both counts visible. Only abbreviate the target folder.
                if (!fits(label)) {
                    label = FillStr(format, L"{name}", L"\u2026");
                    size_t low = 0, high = d.target.size();
                    while (low + 1 < high) {
                        size_t length = (low + high) / 2;
                        size_t end = length;
                        if (end && d.target[end - 1] >= 0xd800 && d.target[end - 1] <= 0xdbff) --end;
                        auto candidate = FillStr(format, L"{name}", d.target.substr(0, end) + L"\u2026");
                        if (fits(candidate)) {
                            label = std::move(candidate);
                            low = length;
                        } else high = length;
                    }
                }
            }
        }
    } else if (all_move) {
        label = FillStr(FillStr(l10n::Get(StringId::TrayMoveNToFormat), L"{n}", std::to_wstring(total)),
                        L"{name}", d.target);
    } else {
        label = FillStr(FillStr(l10n::Get(StringId::TrayCopyNToFormat), L"{n}", std::to_wstring(total)),
                        L"{name}", d.target);
    }

    if (comp && btn_fmt) {
        const float lt = Clamp01(d.label_t);
        const float e = EaseOutBack(lt);
        const D2D1_RECT_F lr = D2D1::RectF(go_rc.left + 8.0f * scale, go_rc.top + 5.0f * scale * (1.0f - e),
                                           go_rc.right - 8.0f * scale, go_rc.bottom + 5.0f * scale * (1.0f - e));
        DrawOneLine(comp, btn_fmt, B(AlphaOf(p.on_acc, (usable ? 1.0f : 0.78f) * EaseOutCubic(lt))),
                    label, lr, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // "N 同名" amber pill
    if (d.conflicts > 0 && usable && comp && badge_fmt) {
        const std::wstring cf = FillStr(l10n::Get(StringId::TrayConflictsFormat), L"{n}", std::to_wstring(d.conflicts));
        const float cw = MeasureWidth(comp->DwriteFactory(), badge_fmt, cf) + 12.0f * scale;
        const D2D1_RECT_F cb = D2D1::RectF(sp.right + 4.0f * scale - cw, sp.top - 8.0f * scale,
                                           sp.right + 4.0f * scale, sp.top + 8.0f * scale);
        const float ring = 2.5f * scale;
        dc->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(cb.left - ring, cb.top - ring, cb.right + ring, cb.bottom + ring),
            8.0f * scale + ring, 8.0f * scale + ring), B(ring_color));
        dc->FillRoundedRectangle(D2D1::RoundedRect(cb, 8.0f * scale, 8.0f * scale), B(p.warn));
        DrawOneLine(comp, badge_fmt, B(Rgba(0x2A1800)), cf, cb, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Snippet landing note underneath button (Floating Shelf only)
    if (!compact_sidebar && !d.snippet_note.empty() && comp && note_fmt) {
        const D2D1_RECT_F nr = D2D1::RectF(sp.left + 2.0f * scale, sp.bottom + 4.0f * scale,
                                           sp.right, sp.bottom + 20.0f * scale);
        DrawOneLine(comp, note_fmt, B(p.faint), d.snippet_note, nr);
        if (out_bottom_y) *out_bottom_y = nr.bottom;
    }
}

ShelfDropGeom LayoutShelfDropLayer(const TrayDeckView& d, const D2D1_RECT_F& rc,
                                   float s,
                                   const std::function<float(const std::wstring&)>& measure_chip) {
    ShelfDropGeom g;
    const float L = rc.left + 10.0f * s;
    const float R = rc.right - 10.0f * s;
    if (R <= L || rc.bottom - rc.top < 90.0f * s) return g;
    g.header = D2D1::RectF(L + 2.0f * s, rc.top + 6.0f * s, R, rc.top + 26.0f * s);

    // Reserve the New chip first, using a second row when the body has room.
    const float chip_h = 24.0f * s;
    const float gap = 5.0f * s;
    const float bottom_row = rc.bottom - 10.0f * s - chip_h;
    const std::wstring other_lbl = l10n::Get(StringId::ShelfDropOtherLabel);
    const std::wstring new_lbl = l10n::Get(StringId::ShelfDropNewChip);
    const float new_w = std::min(R - L, (measure_chip ? measure_chip(new_lbl) : 38.0f * s) + 18.0f * s);
    const float lbl_w = std::min(std::max(0.0f, R - L - new_w - gap),
                                 measure_chip ? measure_chip(other_lbl) : 36.0f * s);
    std::vector<std::pair<int, float>> alternatives;
    float full_row_width = lbl_w + gap + new_w;
    for (int i = 0; i < static_cast<int>(d.shelves.size()); ++i) {
        const auto& sh = d.shelves[static_cast<size_t>(i)];
        if (sh.id == d.active_shelf_id) continue;
        if (d.active_shelf_temp && sh.temp) continue;
        const float tw = measure_chip ? measure_chip(sh.name) : 48.0f * s;
        const float w = std::min(110.0f * s, tw + 28.0f * s);
        alternatives.emplace_back(i, w);
        full_row_width += w + gap;
    }
    const bool two_rows = full_row_width > R - L && bottom_row - chip_h - gap >= g.header.bottom + 66.0f * s;
    const float chips_y = two_rows ? bottom_row - chip_h - gap : bottom_row;
    g.chips_label = D2D1::RectF(L, chips_y, L + lbl_w, chips_y + chip_h);
    float x = g.chips_label.right + gap;
    float y = chips_y;
    for (const auto& [idx, w] : alternatives) {
        if (y < bottom_row && x + w > R) {
            x = L;
            y = bottom_row;
        }
        const float limit = y < bottom_row ? R : R - new_w - gap;
        if (x + w > limit) continue;
        g.shelf_chips.push_back(D2D1::RectF(x, y, x + w, y + chip_h));
        g.shelf_indices.push_back(idx);
        x += w + gap;
    }
    if (two_rows && y < bottom_row) {
        x = L;
        y = bottom_row;
    }
    const float new_left = std::min(x, R - new_w);
    g.new_shelf_chip = D2D1::RectF(new_left, y, new_left + new_w, y + chip_h);

    const float half_top = g.header.bottom + 4.0f * s;
    const float half_bot = std::max(half_top, chips_y - 8.0f * s);
    if (d.drop_content) {
        g.half[0] = D2D1::RectF(L, half_top, R, half_bot);
        g.half[1] = D2D1::RectF();
    } else {
        const float mid = (L + R) * 0.5f;
        g.half[0] = D2D1::RectF(L, half_top, mid - 4.0f * s, half_bot);
        g.half[1] = D2D1::RectF(mid + 4.0f * s, half_top, R, half_bot);
    }
    return g;
}

int HitTestShelfDropLayer(const ShelfDropGeom& g, float x, float y, bool content, bool shift_move) noexcept {
    const auto inside = [&](const D2D1_RECT_F& r) {
        return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    };
    for (size_t i = 0; i < g.shelf_chips.size(); ++i) {
        if (inside(g.shelf_chips[i])) return 10 + g.shelf_indices[i];
    }
    if (inside(g.new_shelf_chip)) return 2;
    if (inside(g.half[0])) return !content && shift_move ? 1 : 0;
    if (!content && inside(g.half[1])) return 1;
    return -1;
}

void DrawShelfDropLayer(ID2D1DeviceContext* dc, Compositor* comp,
                        ID2D1SolidColorBrush* br,
                        IDWriteTextFormat* title_fmt, IDWriteTextFormat* sub_fmt,
                        IDWriteTextFormat* chip_fmt,
                        const TrayDeckView& d, const ShelfColors& p,
                        const ShelfDropGeom& g, float s, bool dark) {
    if (!dc || !comp || !br || g.header.right <= g.header.left) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br; };

    ShelfPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ShelfPtr<ID2D1StrokeStyle> dash;
    if (factory) {
        const float dashes[] = {3.0f, 2.5f};
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f), dashes, 2, &dash);
    }

    // Header: [●] 放进「ShelfName」
    {
        const float cy = (g.header.top + g.header.bottom) * 0.5f;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(g.header.left + 4.0f * s, cy), 4.0f * s, 4.0f * s),
                        B(Rgba(d.active_shelf_color)));
        const std::wstring head = FillStr(l10n::Get(StringId::ShelfDropIntoFormat), L"{name}", d.active_shelf_name);
        DrawOneLine(comp, title_fmt, B(p.strong), head,
                    D2D1::RectF(g.header.left + 14.0f * s, g.header.top, g.header.right, g.header.bottom));
    }

    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    const int half_count = d.drop_content ? 1 : 2;
    for (int i = 0; i < half_count; ++i) {
        const bool mv = !d.drop_content && i == 1;
        const bool active = (d.drop_hover_target == 0 && i == 0) || (d.drop_hover_target == 1 && i == 1);
        const float on = active ? 1.0f : 0.0f;
        const D2D1_RECT_F h = g.half[i];
        if (h.bottom <= h.top || h.right <= h.left) continue;
        const float cx = (h.left + h.right) * 0.5f, cy = (h.top + h.bottom) * 0.5f;
        const float sc = 1.0f + 0.032f * on;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(cx, cy)) * old);
        const float r = 10.0f * s;
        const D2D1_RECT_F inset = D2D1::RectF(h.left + 0.75f * s, h.top + 0.75f * s,
                                              h.right - 0.75f * s, h.bottom - 0.75f * s);
        if (active) {
            dc->FillRoundedRectangle(D2D1::RoundedRect(h, r, r), B(mv ? p.move_soft : AlphaOf(p.acc, 0.20f)));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(inset, r, r), B(mv ? p.move : p.acc2), 1.5f * s);
        } else {
            dc->DrawRoundedRectangle(D2D1::RoundedRect(inset, r, r), B(p.half_edge), 1.5f * s, dash.Get());
        }

        const D2D1_COLOR_F ic = active ? (mv ? p.move : p.acc2) : p.sub;
        const D2D1_COLOR_F tc = active ? (dark ? Rgba(0xFFFFFF) : p.strong) : p.text;
        const bool full_height = h.bottom - h.top >= 56.0f * s;
        float y = cy - (full_height ? 28.0f : 17.0f) * s;
        if (full_height) {
            DrawShelfGlyph(dc, B(ic), d.drop_content ? ShelfGlyph::Plus : (mv ? ShelfGlyph::Move : ShelfGlyph::Copy),
                           D2D1::RectF(cx - 10.0f * s, y, cx + 10.0f * s, y + 20.0f * s), 20.0f * s);
            y += 23.0f * s;
        }
        const std::wstring t_str = d.drop_content
            ? FillStr(l10n::Get(StringId::ShelfDropContentTitleFormat), L"{name}", d.active_shelf_name)
            : l10n::Get(mv ? StringId::TrayDropMoveTitle : StringId::TrayDropCopyTitle);
        DrawOneLine(comp, title_fmt, B(tc), t_str,
                    D2D1::RectF(h.left + 4.0f * s, y, h.right - 4.0f * s, y + 17.0f * s),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 19.0f * s;
        const std::wstring s_str = d.drop_content
            ? l10n::Get(StringId::ShelfDropContentSub)
            : l10n::Get(mv ? StringId::TrayDropOrShift : StringId::TrayDropDefault);
        DrawOneLine(comp, sub_fmt, B(p.faint), s_str,
                    D2D1::RectF(h.left + 4.0f * s, y, h.right - 4.0f * s, y + 14.0f * s),
                    DWRITE_TEXT_ALIGNMENT_CENTER);
        dc->SetTransform(old);
    }

    // Bottom "或放进" chips
    DrawOneLine(comp, sub_fmt, B(p.faint), l10n::Get(StringId::ShelfDropOtherLabel), g.chips_label);
    for (size_t i = 0; i < g.shelf_chips.size(); ++i) {
        const int idx = g.shelf_indices[i];
        const auto& sh = d.shelves[static_cast<size_t>(idx)];
        const bool hot = d.drop_hover_target == 10 + idx;
        const D2D1_RECT_F c = g.shelf_chips[i];
        if (hot) {
            dc->SetTransform(D2D1::Matrix3x2F::Scale(1.06f, 1.06f,
                D2D1::Point2F((c.left + c.right) * 0.5f, (c.top + c.bottom) * 0.5f)) * old);
        }
        dc->FillRoundedRectangle(D2D1::RoundedRect(c, 12.0f * s, 12.0f * s),
                                 B(hot ? Rgba(sh.color_rgb, 0.22f) : p.chip));
        if (hot) {
            dc->DrawRoundedRectangle(D2D1::RoundedRect(c, 12.0f * s, 12.0f * s),
                                     B(Rgba(sh.color_rgb)), 1.5f * s);
        }
        const float cy = (c.top + c.bottom) * 0.5f;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.left + 11.0f * s, cy), 4.0f * s, 4.0f * s),
                        B(Rgba(sh.color_rgb)));
        DrawOneLine(comp, chip_fmt, B(p.text), sh.name,
                    D2D1::RectF(c.left + 20.0f * s, c.top, c.right - 8.0f * s, c.bottom));
        if (hot) dc->SetTransform(old);
    }
    if (g.new_shelf_chip.right > g.new_shelf_chip.left) {
        const bool hot = d.drop_hover_target == 2;
        const D2D1_RECT_F c = g.new_shelf_chip;
        if (hot) {
            dc->SetTransform(D2D1::Matrix3x2F::Scale(1.06f, 1.06f,
                D2D1::Point2F((c.left + c.right) * 0.5f, (c.top + c.bottom) * 0.5f)) * old);
            dc->FillRoundedRectangle(D2D1::RoundedRect(c, 12.0f * s, 12.0f * s), B(AlphaOf(p.acc, 0.20f)));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(c, 12.0f * s, 12.0f * s), B(p.acc2), 1.5f * s);
        } else {
            dc->DrawRoundedRectangle(D2D1::RoundedRect(c, 12.0f * s, 12.0f * s), B(p.half_edge), 1.5f * s, dash.Get());
        }
        DrawOneLine(comp, chip_fmt, B(hot ? p.strong : p.sub), l10n::Get(StringId::ShelfDropNewChip),
                    c, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (hot) dc->SetTransform(old);
    }
}

D2D1_RECT_F MeasureShelfSwitchButton(const TrayDeckView& d, float left, float cy,
                                     float max_w, float s,
                                     const std::function<float(const std::wstring&)>& measure_bold) {
    const std::wstring& name = d.active_shelf_name.empty()
        ? l10n::Get(StringId::ShelfTempName) : d.active_shelf_name;
    const float nw = measure_bold ? measure_bold(name) : 34.0f * s;
    const float w = std::min(max_w, 6.0f * s + 8.0f * s + 6.0f * s + nw + 5.0f * s + 12.0f * s + 6.0f * s);
    return D2D1::RectF(left, cy - 13.0f * s, left + std::max(42.0f * s, w), cy + 13.0f * s);
}

void DrawShelfSwitchButton(ID2D1DeviceContext* dc, Compositor* comp,
                           ID2D1SolidColorBrush* br,
                           IDWriteTextFormat* name_fmt, IDWriteTextFormat* tag_fmt,
                           const TrayDeckView& d, const ShelfColors& p,
                           const D2D1_RECT_F& btn_rc, float s, bool hot,
                           bool show_exit_tag, float max_tag_right) {
    if (!dc || !comp || !br) return;
    auto B = [&](const D2D1_COLOR_F& c) { br->SetColor(c); return br; };
    if (hot) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(btn_rc, 6.0f * s, 6.0f * s), B(p.ib_hot));
    }
    const float cy = (btn_rc.top + btn_rc.bottom) * 0.5f;
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(btn_rc.left + 10.0f * s, cy), 4.0f * s, 4.0f * s),
                    B(Rgba(d.active_shelf_color)));
    const D2D1_RECT_F chev_rc = D2D1::RectF(btn_rc.right - 18.0f * s, cy - 6.0f * s,
                                            btn_rc.right - 6.0f * s, cy + 6.0f * s);
    DrawShelfGlyph(dc, B(p.faint), ShelfGlyph::Chev, chev_rc, 11.0f * s);
    const std::wstring& name = d.active_shelf_name.empty()
        ? l10n::Get(StringId::ShelfTempName) : d.active_shelf_name;
    DrawOneLine(comp, name_fmt, B(p.strong), name,
                D2D1::RectF(btn_rc.left + 20.0f * s, btn_rc.top, chev_rc.left - 3.0f * s, btn_rc.bottom));

    if (show_exit_tag && d.active_shelf_temp && tag_fmt) {
        const std::wstring& tag = l10n::Get(StringId::ShelfClearedOnExit);
        const float tw = MeasureWidth(comp->DwriteFactory(), tag_fmt, tag) + 12.0f * s;
        const float x0 = btn_rc.right + 2.0f * s;
        if (x0 + tw <= max_tag_right) {
            const D2D1_RECT_F tr = D2D1::RectF(x0, cy - 8.5f * s, x0 + tw, cy + 8.5f * s);
            dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 8.0f * s, 8.0f * s), B(p.chip));
            DrawOneLine(comp, tag_fmt, B(p.faint), tag, tr, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
}

std::wstring FormatShelfGridName(const std::wstring& name, bool is_file) {
    const size_t dot = is_file ? name.find_last_of(L'.') : std::wstring::npos;
    std::wstring out;
    out.reserve(name.size() + 6);
    for (size_t i = 0; i < name.size(); ++i) {
        if (dot != std::wstring::npos && dot > 0 && i == dot)
            out.push_back(L'\u200B');  // break cleanly before ".ext"
        const wchar_t c = name[i];
        out.push_back(c);
        if ((c == L'_' || c == L'-' || c == L'\u00B7') && i + 1 < name.size())
            out.push_back(L'\u200B');
    }
    return out;
}

} // namespace pulse::ui
