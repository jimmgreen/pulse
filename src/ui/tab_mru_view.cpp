#include "tab_mru_view.h"
#include "FluentTokens.h"
#include "typography.h"
#include "window_switcher_view.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

// Sizes from the mockup's .mru (DIP).
constexpr float kPanelW = 520.0f;
constexpr float kPad = 10.0f;
constexpr float kTitleTop = 2.0f, kTitleH = 18.0f, kTitleGap = 8.0f;
constexpr float kRowH = 44.0f;
constexpr float kFootGap = 8.0f, kFootH = 20.0f, kFootBottom = 2.0f;
constexpr float kPanelRadius = 14.0f, kRowRadius = 9.0f;

constexpr motion::SpringSpec kRingSpring{700.0f, 40.0f};
constexpr float kFadeInMs = 100.0f;
constexpr float kCloseMs = 110.0f;
constexpr float kPanelFrom = 0.985f;
constexpr UINT_PTR kTabMruFrameTimer = 0x544D5255;  // 'TMRU'

void CALLBACK TabMruFrameTick(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    InvalidateRect(hwnd, nullptr, FALSE);
}

template <class T> void ReleaseCom(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

D2D1_COLOR_F Fade(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

D2D1_COLOR_F Solid(const D2D1_COLOR_F& top, const D2D1_COLOR_F& under) {
    const float a = std::clamp(top.a, 0.0f, 1.0f);
    return D2D1::ColorF(top.r * a + under.r * (1.0f - a), top.g * a + under.g * (1.0f - a),
                        top.b * a + under.b * (1.0f - a), 1.0f);
}

bool Inside(const D2D1_RECT_F& r, float x, float y) {
    return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

}  // namespace

TabMruView::~TabMruView() { ReleaseFormats(); }

void TabMruView::Show(TabMruData data, const D2D1_RECT_F& client, float scale, HWND host) {
    const bool was_open = open_;
    data_ = std::move(data);
    client_ = client;
    scale_ = scale;
    if (host) host_ = host;
    open_ = !data_.rows.empty();
    if (open_ && !was_open) {
        animations_ = motion::SystemAnimationsEnabled();
        ring_.SetAnimationsEnabled(animations_);
        open_at_ = motion::NowMs();
        closing_ = false;
        hover_ = -1;
        ++context_;
    }
    Layout();
    if (open_) RequestFrame();
}

void TabMruView::Hide() noexcept {
    if (open_ && animations_ && host_) {
        closing_ = true;
        close_at_ = motion::NowMs();
        RequestFrame();
    }
    open_ = false;
    hover_ = -1;
}

bool TabMruView::SetHover(int row) noexcept {
    if (!open_ || row == hover_) return false;
    hover_ = row;
    return true;
}

void TabMruView::RequestFrame() const noexcept {
    if (host_) SetTimer(host_, kTabMruFrameTimer, 15, TabMruFrameTick);
}

void TabMruView::Layout() {
    const float s = scale_;
    const size_t n = data_.rows.size();
    row_rects_.assign(n, D2D1_RECT_F{});
    if (!n) return;
    const float cw = client_.right - client_.left, ch = client_.bottom - client_.top;
    const float w = std::min(kPanelW * s, cw - 32.0f * s);
    const float h = (2.0f * kPad + kTitleTop + kTitleH + kTitleGap + kFootGap + kFootH + kFootBottom) * s +
                    static_cast<float>(n) * kRowH * s;
    const float cx = client_.left + cw * 0.5f;
    const float top = std::max(client_.top + 8.0f * s, client_.top + (ch - h) * 0.5f);
    panel_ = D2D1::RectF(cx - w * 0.5f, top, cx + w * 0.5f, top + h);
    float y = panel_.top + (kPad + kTitleTop + kTitleH + kTitleGap) * s;
    for (size_t i = 0; i < n; ++i, y += kRowH * s)
        row_rects_[i] = D2D1::RectF(panel_.left + kPad * s, y, panel_.right - kPad * s, y + kRowH * s);
}

int TabMruView::HitRow(float x, float y) const noexcept {
    if (!open_) return -1;
    for (size_t i = 0; i < row_rects_.size(); ++i)
        if (Inside(row_rects_[i], x, y)) return static_cast<int>(i);
    return -1;
}

bool TabMruView::Contains(float x, float y) const noexcept { return open_ && Inside(panel_, x, y); }

void TabMruView::ReleaseFormats() {
    ReleaseCom(f_title_);
    ReleaseCom(f_name_);
    ReleaseCom(f_small_);
    ReleaseCom(f_hint_);
    format_scale_ = 0.0f;
}

void TabMruView::EnsureFormats(IDWriteFactory* dwrite, IDWriteTextFormat* base) {
    const float text_scale = scale_ * typography::UiFontScale();
    if (f_name_ && std::abs(format_scale_ - text_scale) < 0.001f) return;
    ReleaseFormats();
    format_scale_ = text_scale;
    wchar_t family[128] = L"Segoe UI";
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"zh-cn";
    if (base) {
        base->GetFontFamilyName(family, 128);
        base->GetLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    }
    auto make = [&](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        if (FAILED(dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size * text_scale, locale, out)) || !*out)
            return;
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* ellipsis = nullptr;
        if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(*out, &ellipsis)) && ellipsis) {
            (*out)->SetTrimming(&trim, ellipsis);
            ellipsis->Release();
        } else {
            (*out)->SetTrimming(&trim, nullptr);
        }
    };
    make(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &f_title_);
    make(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &f_name_);
    make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &f_small_);
    make(11.5f, DWRITE_FONT_WEIGHT_NORMAL, &f_hint_);
}

void TabMruView::Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, IDWriteTextFormat* base_format,
                      const Theme& theme) {
    if (!IsOpen() || !dc || !dwrite || data_.rows.empty()) return;
    const uint64_t now = motion::NowMs();
    ++frame_;
    bool moving = false;
    float fade = 1.0f, panel_scale = 1.0f;
    if (closing_) {
        const float k = std::min(1.0f, static_cast<float>(now - close_at_) / kCloseMs);
        if (k >= 1.0f) { closing_ = false; return; }
        fade = 1.0f - motion::EaseOutCubic(k);
        moving = true;
    } else if (animations_) {
        const float t = static_cast<float>(now - open_at_);
        const float progress = std::min(1.0f, t / kFadeInMs);
        panel_scale = kPanelFrom + (1.0f - kPanelFrom) * motion::EaseOutCubic(progress);
        if (t < kFadeInMs) moving = true;
    }
    struct FrameRequest {
        const TabMruView* view;
        const bool& moving;
        ~FrameRequest() { if (moving) view->RequestFrame(); }
    } frame_request{this, moving};

    EnsureFormats(dwrite, base_format);
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &brush)) || !brush) return;
    const auto use = [&](const D2D1_COLOR_F& c) { brush->SetColor(Fade(c, fade)); return brush; };
    const auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), use(c));
    };
    const auto stroke = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float w) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), use(c), w);
    };
    const auto text = [&](IDWriteTextFormat* f, const std::wstring& str, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
        if (!f || str.empty() || r.right <= r.left) return;
        dc->DrawTextW(str.c_str(), static_cast<UINT32>(str.size()), f, r, use(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const float s = scale_;

    D2D1_MATRIX_3X2_F base_transform{};
    dc->GetTransform(&base_transform);
    const D2D1_POINT_2F centre = D2D1::Point2F((panel_.left + panel_.right) * 0.5f, (panel_.top + panel_.bottom) * 0.5f);
    if (panel_scale != 1.0f)
        dc->SetTransform(D2D1::Matrix3x2F::Scale(panel_scale, panel_scale, centre) * base_transform);

    // Flyout: soft shadow, opaque flyout surface, hairline.
    const float pr = kPanelRadius * s;
    for (int i = 5; i >= 1; --i) {
        const float g = static_cast<float>(i) * 2.5f * s;
        fill(D2D1::RectF(panel_.left - g, panel_.top - g + 6.0f * s, panel_.right + g, panel_.bottom + g + 6.0f * s),
             pr + g, D2D1::ColorF(0, 0, 0, 0.035f));
    }
    fill(panel_, pr, Solid(theme.surface_flyout, theme.bg));
    stroke(panel_, pr, theme.stroke_card, std::max(1.0f, s));

    // Title row: 最近使用的标签 ............ N 个
    const float inner_l = panel_.left + (kPad + 6.0f) * s, inner_r = panel_.right - (kPad + 6.0f) * s;
    const D2D1_RECT_F title_r = D2D1::RectF(inner_l, panel_.top + (kPad + kTitleTop) * s, inner_r,
                                            panel_.top + (kPad + kTitleTop + kTitleH) * s);
    const float count_w = MeasureTextWidth(dwrite, f_small_, data_.count);
    text(f_title_, data_.title, D2D1::RectF(title_r.left, title_r.top, title_r.right - count_w - 8.0f * s, title_r.bottom),
         theme.text_secondary);
    text(f_small_, data_.count, D2D1::RectF(title_r.right - count_w - 1.0f, title_r.top, title_r.right + 1.0f, title_r.bottom),
         theme.text_secondary);

    const int count = static_cast<int>(data_.rows.size());
    const float rr = kRowRadius * s;
    if (open_ && hover_ >= 0 && hover_ < count && hover_ != data_.selected)
        fill(row_rects_[static_cast<size_t>(hover_)], rr, theme.fill_hover);
    const int sel = std::clamp(data_.selected, 0, count - 1);
    const bool high_contrast = IsHighContrast();
    // High-contrast foreground and background must select the same row in the same frame.
    ring_.SetAnimationsEnabled(animations_ && !high_contrast);
    {
        const D2D1_RECT_F r = ring_.Update(context_, sel, row_rects_[static_cast<size_t>(sel)], frame_, now, kRingSpring);
        if (ring_.Active(now)) moving = true;
        fill(r, rr, theme.fill_selected);
        const float h = 0.75f * s;  // 1.5 DIP ring inside the row
        stroke(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h), rr - h, theme.accent, 1.5f * s);
    }

    const auto highlight_ink = high_contrast ? ColorFromRef(GetSysColor(COLOR_HIGHLIGHTTEXT)) : theme.text;
    for (int i = 0; i < count; ++i) {
        const D2D1_RECT_F r = row_rects_[static_cast<size_t>(i)];
        const bool highlighted = high_contrast && (i == sel || (open_ && i == hover_));
        const auto primary_ink = highlighted ? highlight_ink : theme.text;
        const auto secondary_ink = highlighted ? highlight_ink : theme.text_secondary;
        const auto folder_ink = highlighted ? highlight_ink : theme.icon_folder;
        const TabMruRow& row = data_.rows[static_cast<size_t>(i)];
        float x = r.left + 12.0f * s;
        const float cy = (r.top + r.bottom) * 0.5f;
        // Folder: back tab + body, in the folder colour.
        const float fw = 22.0f * s, fh = 19.0f * s, fy = cy - fh * 0.5f;
        fill(D2D1::RectF(x, fy, x + fw * 0.45f, fy + fh * 0.4f), 2.0f * s, highlighted ? folder_ink : Solid(Fade(folder_ink, 0.75f), theme.bg));
        fill(D2D1::RectF(x, fy + fh * 0.18f, x + fw, fy + fh), 2.5f * s, folder_ink);
        x += fw + 12.0f * s;
        float right = r.right - 12.0f * s;
        if (i == 0 && !data_.current.empty()) {
            const float cur_w = MeasureTextWidth(dwrite, f_hint_, data_.current);
            text(f_hint_, data_.current, D2D1::RectF(right - cur_w - 1.0f, r.top, right + 1.0f, r.bottom), secondary_ink);
            right -= cur_w + 12.0f * s;
        }
        const float name_w = std::min(MeasureTextWidth(dwrite, f_name_, row.name) + 1.0f, std::max(0.0f, right - x));
        text(f_name_, row.name, D2D1::RectF(x, r.top, x + name_w, r.bottom), primary_ink);
        x += name_w + 12.0f * s;
        if (!row.parent.empty() && right - x > 24.0f * s)
            text(f_small_, FitPathTail(dwrite, f_small_, row.parent, right - x), D2D1::RectF(x, r.top, right, r.bottom),
                 secondary_ink);
    }

    // Footer hints, centred, keys as small caps.
    const float fy0 = row_rects_.back().bottom + kFootGap * s, fy1 = fy0 + kFootH * s;
    const float kbd_pad = 5.0f * s, part_gap = 4.0f * s, hint_gap = 14.0f * s;
    std::vector<float> widths;
    float total = 0.0f;
    for (const auto& hint : data_.hints) {
        float w = 0.0f;
        for (const auto& part : hint) {
            const float tw = MeasureTextWidth(dwrite, f_hint_, part.text);
            w += part.kbd ? tw + 2.0f * kbd_pad + part_gap : tw;
        }
        widths.push_back(w);
        total += w;
    }
    if (!widths.empty()) total += hint_gap * static_cast<float>(widths.size() - 1);
    float hx = std::max(panel_.left + kPad * s, (panel_.left + panel_.right - total) * 0.5f);
    for (size_t h = 0; h < data_.hints.size(); ++h) {
        for (const auto& part : data_.hints[h]) {
            const float tw = MeasureTextWidth(dwrite, f_hint_, part.text);
            if (part.kbd) {
                const D2D1_RECT_F cap = D2D1::RectF(hx, fy0 + 1.0f * s, hx + tw + 2.0f * kbd_pad, fy1 - 1.0f * s);
                fill(cap, 4.0f * s, Fade(theme.text, 0.06f));
                stroke(cap, 4.0f * s, theme.stroke_card, std::max(1.0f, s));
                text(f_hint_, part.text, D2D1::RectF(cap.left + kbd_pad, cap.top, cap.right - kbd_pad + 1.0f, cap.bottom),
                     theme.text);
                hx = cap.right + part_gap;
            } else {
                text(f_hint_, part.text, D2D1::RectF(hx, fy0, hx + tw + 1.0f, fy1), theme.text_secondary);
                hx += tw;
            }
        }
        hx += hint_gap;
    }

    if (panel_scale != 1.0f) dc->SetTransform(base_transform);
    brush->Release();
}

}  // namespace pulse::ui
