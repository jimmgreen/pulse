#include "window_switcher_view.h"
#include "FluentTokens.h"
#include "typography.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

constexpr float kCardW = 208.0f;   // DIP
constexpr float kCardH = 128.0f;
constexpr float kGap = 10.0f;
constexpr float kPad = 16.0f;
constexpr float kHintH = 30.0f;
constexpr int kTabLines = 3;

// Motion tokens (pulse_motion_v2).
constexpr motion::SpringSpec kRingSpring{700.0f, 40.0f};  // a little overshoot as it lands
constexpr float kPanelFrom = 0.985f;
constexpr float kFadeInMs = 120.0f;
constexpr float kCloseMs = 110.0f;
constexpr float kCardRise = 10.0f;  // DIP
constexpr float kHoverFadeInMs = 90.0f, kHoverFadeOutMs = 120.0f;
constexpr UINT_PTR kSwitcherFrameTimer = 0x53574D54;  // 'SWMT'

void CALLBACK SwitcherFrameTick(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    InvalidateRect(hwnd, nullptr, FALSE);
}


template <class T> void Release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

D2D1_COLOR_F Rgb(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, a);
}

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

D2D1_COLOR_F Opaque(const D2D1_COLOR_F& top, const D2D1_COLOR_F& under) {
    const float a = std::clamp(top.a, 0.0f, 1.0f);
    return D2D1::ColorF(top.r * a + under.r * (1.0f - a), top.g * a + under.g * (1.0f - a),
                        top.b * a + under.b * (1.0f - a), 1.0f);
}

bool In(const D2D1_RECT_F& r, float x, float y) {
    return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

}  // namespace

float MeasureTextWidth(IDWriteFactory* dwrite, IDWriteTextFormat* f, const std::wstring& s) {
    if (!dwrite || !f || s.empty()) return 0.0f;
    IDWriteTextLayout* layout = nullptr;
    float w = 0.0f;
    if (SUCCEEDED(dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 8192.0f, 256.0f, &layout)) &&
        layout) {
        DWRITE_TEXT_METRICS m{};
        if (SUCCEEDED(layout->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        layout->Release();
    }
    return w;
}

// A long folder path keeps its end, where the folder is: "...\Work\Pulse\src".
std::wstring FitPathTail(IDWriteFactory* dwrite, IDWriteTextFormat* f, const std::wstring& path, float width) {
    if (MeasureTextWidth(dwrite, f, path) <= width) return path;
    std::wstring tail;
    for (size_t sep = path.find(L'\\', 1); sep != std::wstring::npos; sep = path.find(L'\\', sep + 1)) {
        tail = L"\x2026" + path.substr(sep);
        if (MeasureTextWidth(dwrite, f, tail) <= width) return tail;
    }
    return tail.empty() ? path : tail;  // even the last folder is too long: the end gets trimmed
}

WindowSwitcherView::~WindowSwitcherView() { ReleaseFormats(); }

void WindowSwitcherView::Show(WindowSwitcherData data, const D2D1_RECT_F& client, float scale, HWND host) {
    const bool was_open = open_;
    data_ = std::move(data);
    client_ = client;
    scale_ = scale;
    if (host) host_ = host;
    open_ = !data_.cards.empty();
    if (open_ && !was_open) {  // a fresh opening (a refresh while open keeps its motion)
        animations_ = motion::SystemAnimationsEnabled();
        ring_.SetAnimationsEnabled(animations_);
        plate_.SetAnimationsEnabled(animations_);
        open_at_ = motion::NowMs();
        closing_ = false;
        hover_ = -1;
        hover_alpha_ = 0.0f;
        ++context_;
    }
    Layout();
}

void WindowSwitcherView::Hide() noexcept {
    if (open_ && animations_ && host_) {
        closing_ = true;
        close_at_ = motion::NowMs();
        RequestFrame();
    }
    open_ = false;
    hover_ = -1;
}

bool WindowSwitcherView::SetHover(int card) noexcept {
    if (!open_ || card == hover_) return false;
    hover_ = card;
    return true;
}

void WindowSwitcherView::RequestFrame() const noexcept {
    if (host_) SetTimer(host_, kSwitcherFrameTimer, 15, SwitcherFrameTick);
}

// Cards in rows, centred; as many columns as fit. When the window is too
// small for every row, the rows around the selected card show.
void WindowSwitcherView::Layout() {
    const float s = scale_;
    const int count = static_cast<int>(data_.cards.size());
    card_rects_.assign(static_cast<size_t>(count), D2D1_RECT_F{});
    if (count == 0) return;
    const float avail_w = (client_.right - client_.left) - 2.0f * (kPad + 12.0f) * s;
    const float avail_h = (client_.bottom - client_.top) - 2.0f * (kPad + 12.0f) * s - kHintH * s;
    const int cols = std::clamp(static_cast<int>((avail_w + kGap * s) / ((kCardW + kGap) * s)), 1, count);
    const int rows = (count + cols - 1) / cols;
    const int fit_rows = std::clamp(static_cast<int>((avail_h + kGap * s) / ((kCardH + kGap) * s)), 1, rows);
    const int sel_row = std::clamp(data_.selected, 0, count - 1) / cols;
    const int first_row = std::clamp(sel_row - fit_rows + 1, 0, rows - fit_rows);
    const float grid_w = static_cast<float>(cols) * kCardW * s + static_cast<float>(cols - 1) * kGap * s;
    const float grid_h = static_cast<float>(fit_rows) * kCardH * s + static_cast<float>(fit_rows - 1) * kGap * s;
    const float panel_w = std::min(grid_w + 2.0f * kPad * s, client_.right - client_.left - 16.0f * s);
    const float panel_h = grid_h + 2.0f * kPad * s + kHintH * s;
    const float cx = (client_.left + client_.right) * 0.5f;
    const float cy = (client_.top + client_.bottom) * 0.5f;
    panel_ = D2D1::RectF(cx - panel_w * 0.5f, cy - panel_h * 0.5f, cx + panel_w * 0.5f, cy + panel_h * 0.5f);
    for (int i = 0; i < count; ++i) {
        const int row = i / cols - first_row;
        if (row < 0 || row >= fit_rows) continue;
        const int in_row = std::min(cols, count - (i / cols) * cols);  // last row may be short: centre it
        const float row_w = static_cast<float>(in_row) * kCardW * s + static_cast<float>(in_row - 1) * kGap * s;
        const float x = cx - row_w * 0.5f + static_cast<float>(i % cols) * (kCardW + kGap) * s;
        const float y = panel_.top + kPad * s + static_cast<float>(row) * (kCardH + kGap) * s;
        card_rects_[static_cast<size_t>(i)] = D2D1::RectF(x, y, x + kCardW * s, y + kCardH * s);
    }
}

int WindowSwitcherView::HitCard(float x, float y) const noexcept {
    if (!open_) return -1;
    for (size_t i = 0; i < card_rects_.size(); ++i)
        if (In(card_rects_[i], x, y)) return static_cast<int>(i);
    return -1;
}

bool WindowSwitcherView::Contains(float x, float y) const noexcept { return open_ && In(panel_, x, y); }

void WindowSwitcherView::ReleaseFormats() {
    Release(f_name_);
    Release(f_small_);
    Release(f_hint_);
    format_scale_ = 0.0f;
}

void WindowSwitcherView::EnsureFormats(IDWriteFactory* dwrite, IDWriteTextFormat* base) {
    // Text follows Settings > Interface font size, like the other pickers.
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
    make(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &f_name_);
    make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &f_small_);
    make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &f_hint_);
    if (f_hint_) f_hint_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

void WindowSwitcherView::Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, IDWriteTextFormat* base_format,
                              const Theme& theme) {
    if (!IsOpen() || !dc || !dwrite || data_.cards.empty()) return;
    const uint64_t now = motion::NowMs();
    const float dt = last_draw_ ? static_cast<float>(std::min<uint64_t>(now - last_draw_, 64)) : 0.0f;
    last_draw_ = now;
    ++frame_;
    bool moving = false;

    // Opening: fade + pop; closing: fade out and settle back a little.
    float fade = 1.0f, panel_scale = 1.0f;
    const float open_ms = static_cast<float>(now - open_at_);
    if (closing_) {
        const float k = std::min(1.0f, static_cast<float>(now - close_at_) / kCloseMs);
        if (k >= 1.0f) { closing_ = false; return; }
        fade = 1.0f - motion::EaseOutCubic(k);
        panel_scale = 1.0f - 0.03f * k;
        moving = true;
    } else if (animations_) {
        const float progress = std::min(1.0f, open_ms / kFadeInMs);
        panel_scale = kPanelFrom + (1.0f - kPanelFrom) * motion::EaseOutCubic(progress);
        if (open_ms < kFadeInMs) moving = true;
    }
    // Asks for the next frame on every return below while something moves.
    struct FrameRequest {
        const WindowSwitcherView* view;
        const bool& moving;
        ~FrameRequest() { if (moving) view->RequestFrame(); }
    } frame_request{this, moving};

    EnsureFormats(dwrite, base_format);
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &brush)) || !brush) return;
    float alpha = fade;  // multiplies every colour drawn; per card while it rises in
    const auto use = [&](const D2D1_COLOR_F& c) { brush->SetColor(Alpha(c, alpha)); return brush; };
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

    // Backdrop: the window behind steps back; the panel floats with a soft shadow.
    dc->FillRectangle(client_, use(D2D1::ColorF(0, 0, 0, 0.30f)));
    D2D1_MATRIX_3X2_F base_transform{};
    dc->GetTransform(&base_transform);
    const D2D1_POINT_2F centre = D2D1::Point2F((panel_.left + panel_.right) * 0.5f, (panel_.top + panel_.bottom) * 0.5f);
    if (panel_scale != 1.0f)
        dc->SetTransform(D2D1::Matrix3x2F::Scale(panel_scale, panel_scale, centre) * base_transform);
    const float radius = 14.0f * s;
    for (int i = 6; i >= 1; --i) {
        const float g = static_cast<float>(i) * 2.5f * s;
        fill(D2D1::RectF(panel_.left - g, panel_.top - g + 8.0f * s, panel_.right + g, panel_.bottom + g + 8.0f * s),
             radius + g, D2D1::ColorF(0, 0, 0, 0.04f));
    }
    const D2D1_COLOR_F surface = Opaque(theme.surface_flyout, theme.bg);
    fill(panel_, radius, surface);
    stroke(panel_, radius, theme.stroke_card, std::max(1.0f, s));

    const float cr = 10.0f * s;
    const int count = static_cast<int>(data_.cards.size());
    // Cards are readable immediately; only the container and selection move.
    const auto rise = [](size_t) { return 1.0f; };
    const auto shifted = [&](D2D1_RECT_F r, float k) {
        const float dy = (1.0f - k) * kCardRise * s;
        r.top += dy;
        r.bottom += dy;
        return r;
    };

    // Card bodies first, then the plate and the ring over them, then the content.
    for (size_t i = 0; i < data_.cards.size(); ++i) {
        const D2D1_RECT_F r = card_rects_[i];
        if (r.right <= r.left) continue;
        const float k = rise(i);
        alpha = fade * std::clamp(k * 1.6f, 0.0f, 1.0f);
        fill(shifted(r, k), cr, Alpha(theme.text, 0.035f));
    }
    alpha = fade;

    // Hover plate: glides between cards, fades in 90 ms / out 120 ms.
    const bool hover_live = open_ && hover_ >= 0 && hover_ < count && hover_ != data_.selected &&
                            card_rects_[static_cast<size_t>(hover_)].right > card_rects_[static_cast<size_t>(hover_)].left;
    if (hover_live) {
        if (hover_alpha_ <= 0.0f) plate_.Hide();  // appear in place, not gliding from the last card
        hover_alpha_ = animations_ ? std::min(1.0f, hover_alpha_ + dt / kHoverFadeInMs) : 1.0f;
    } else {
        hover_alpha_ = animations_ ? std::max(0.0f, hover_alpha_ - dt / kHoverFadeOutMs) : 0.0f;
    }
    if (hover_live) plate_target_ = card_rects_[static_cast<size_t>(hover_)];
    if (hover_alpha_ > 0.0f) {
        const D2D1_RECT_F pr = plate_.Update(context_, hover_live ? hover_ : -2, plate_target_, frame_, now, motion::spring::Plate);
        fill(pr, cr, Alpha(theme.text, 0.05f * hover_alpha_));
        if (plate_.Active(now) || (hover_alpha_ < 1.0f && hover_alpha_ > 0.0f)) moving = true;
    } else {
        plate_.Hide();
    }

    // Selection ring: springs to the selected card, overshooting a touch.
    const int sel = std::clamp(data_.selected, 0, count - 1);
    D2D1_RECT_F sel_rect = card_rects_[static_cast<size_t>(sel)];
    if (sel_rect.right > sel_rect.left) {
        sel_rect = shifted(sel_rect, rise(static_cast<size_t>(sel)));
        const D2D1_RECT_F rr = ring_.Update(context_, sel, sel_rect, frame_, now, kRingSpring);
        if (ring_.Active(now)) moving = true;
        fill(rr, cr, Alpha(theme.accent, 0.12f));
        stroke(D2D1::RectF(rr.left + s, rr.top + s, rr.right - s, rr.bottom - s), cr, theme.accent, 2.0f * s);
    }

    for (size_t i = 0; i < data_.cards.size(); ++i) {
        const float k = rise(i);
        alpha = fade * std::clamp(k * 1.6f, 0.0f, 1.0f);
        const D2D1_RECT_F r = shifted(card_rects_[i], k);
        if (card_rects_[i].right <= card_rects_[i].left) continue;
        const WindowSwitcherCard& card = data_.cards[i];
        const bool on = static_cast<int>(i) == data_.selected;
        // Window color on the card's left edge, then dot + name.
        fill(D2D1::RectF(r.left + 1.0f * s, r.top + 16.0f * s, r.left + 4.0f * s, r.bottom - 16.0f * s), 1.5f * s,
             Rgb(card.rgb, on ? 1.0f : 0.7f));
        const float x = r.left + 14.0f * s, w = r.right - 12.0f * s - x;
        float y = r.top + 10.0f * s;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 5.0f * s, y + 11.0f * s), 5.0f * s, 5.0f * s), use(Rgb(card.rgb)));
        text(f_name_, card.name, D2D1::RectF(x + 16.0f * s, y, x + w, y + 22.0f * s), theme.text);
        y += 24.0f * s;
        text(f_small_, FitPathTail(dwrite, f_small_, card.folder, w), D2D1::RectF(x, y, x + w, y + 18.0f * s),
             theme.text_secondary);
        y += 18.0f * s;
        text(f_small_, card.tab_count, D2D1::RectF(x, y, x + w, y + 18.0f * s), theme.text_secondary);
        y += 22.0f * s;
        // First tabs: a short line each, the active one stronger.
        for (size_t t = 0; t < card.tabs.size() && t < static_cast<size_t>(kTabLines); ++t) {
            if (y + 16.0f * s > r.bottom - 6.0f * s) break;
            const bool active = static_cast<int>(t) == card.active_tab;
            fill(D2D1::RectF(x, y + 5.0f * s, x + 6.0f * s, y + 11.0f * s), 1.5f * s,
                 active ? theme.accent : Alpha(theme.text, 0.25f));
            text(f_small_, card.tabs[t], D2D1::RectF(x + 12.0f * s, y, x + w, y + 16.0f * s),
                 active ? theme.text : Alpha(theme.text_secondary, 0.85f));
            y += 16.0f * s;
        }
    }
    alpha = fade;
    text(f_hint_, data_.hint, D2D1::RectF(panel_.left + kPad * s, panel_.bottom - kHintH * s - 4.0f * s,
                                          panel_.right - kPad * s, panel_.bottom - 4.0f * s),
         theme.text_secondary);
    if (panel_scale != 1.0f) dc->SetTransform(base_transform);
    brush->Release();
}

}  // namespace pulse::ui
