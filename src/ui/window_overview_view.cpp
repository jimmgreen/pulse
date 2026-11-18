#include "window_overview_view.h"
#include "window_layout_diagram.h"
#include "FluentTokens.h"
#include "typography.h"
#include "window_switcher_view.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

// DIP
constexpr float kPad = 20.0f;       // inside the panel
constexpr float kHeaderH = 58.0f;
constexpr float kButtonH = 32.0f;
constexpr float kCardMinW = 250.0f;
constexpr float kGap = 14.0f;
constexpr float kCardTop = 58.0f;   // header part of a card, above the chips
constexpr float kChipH = 30.0f;
constexpr float kChipGap = 4.0f;
constexpr float kMoreH = 24.0f;
constexpr float kCardMinH = 140.0f;
constexpr float kNewCardH = 140.0f;

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

D2D1_RECT_F Shift(D2D1_RECT_F r, float dy) {
    r.top += dy;
    r.bottom += dy;
    return r;
}

}  // namespace

WindowOverviewView::~WindowOverviewView() { ReleaseFormats(); }

void WindowOverviewView::Show(WindowOverviewData data, const D2D1_RECT_F& client, float scale) {
    data_ = std::move(data);
    client_ = client;
    scale_ = scale;
    open_ = true;
    Layout();
}

void WindowOverviewView::Hide() noexcept {
    open_ = false;
    dragging_ = false;
    hover_ = {};
    scroll_ = 0.0f;
}

// Header with the buttons on the right; below it the cards in as many
// columns as fit, each row as tall as its tallest card, the "new window" card
// last. Rects are kept unscrolled; scroll_ is applied when used.
void WindowOverviewView::Layout() {
    const float s = scale_;
    panel_ = client_;
    const auto width_of = [&](const std::wstring& text) {
        const float w = f_small_ && dwrite_ ? MeasureTextWidth(dwrite_, f_small_, text)
                                            : static_cast<float>(text.size()) * 13.0f * s;
        return w + 32.0f * s;
    };
    const float by = panel_.top + (kHeaderH - kButtonH) * 0.5f * s;
    const float done_w = width_of(data_.done);
    done_button_ = D2D1::RectF(panel_.right - kPad * s - done_w, by, panel_.right - kPad * s, by + kButtonH * s);

    area_ = D2D1::RectF(panel_.left + kPad * s, panel_.top + kHeaderH * s, panel_.right - kPad * s,
                        panel_.bottom - 10.0f * s);
    const float avail_w = std::max(area_.right - area_.left, 1.0f);
    const float toolbar_w = width_of(data_.arrange) + 26*s;
    const float title_w = width_of(data_.title);
    arrange_in_header_ = done_button_.left - toolbar_w - 12*s > area_.left + title_w + 24*s;
    const float toolbar_x = arrange_in_header_ ? done_button_.left - toolbar_w - 10*s : area_.left;
    const float toolbar_y = arrange_in_header_ ? by : area_.top;
    arrange_buttons_.clear();
    if (!data_.arrangements.empty())
        arrange_buttons_.push_back(D2D1::RectF(toolbar_x, toolbar_y, toolbar_x + toolbar_w, toolbar_y + kButtonH*s));
    if (!arrange_in_header_ && !arrange_buttons_.empty()) area_.top=arrange_buttons_.back().bottom+12*s;
    const size_t items = data_.cards.size() + 1;
    const size_t cols = std::clamp<size_t>(static_cast<size_t>((avail_w + kGap * s) / ((kCardMinW + kGap) * s)), 1,
                                           items);
    const float card_w = (avail_w - static_cast<float>(cols - 1) * kGap * s) / static_cast<float>(cols);
    const auto card_h = [&](size_t i) {
        if (i >= data_.cards.size()) return kNewCardH * s;
        const WindowOverviewCard& c = data_.cards[i];
        const float n = static_cast<float>(c.chips.size());
        const float h = kCardTop + n * kChipH + std::max(n - 1.0f, 0.0f) * kChipGap + (c.more.empty() ? 0.0f : kMoreH) +
                        12.0f;
        return std::max(h, kCardMinH) * s;
    };

    rects_.assign(data_.cards.size(), CardRects{});
    float y = area_.top + 4.0f * s;
    for (size_t row_start = 0; row_start < items; row_start += cols) {
        const size_t row_end = std::min(row_start + cols, items);
        float row_h = 0.0f;
        for (size_t i = row_start; i < row_end; ++i) row_h = std::max(row_h, card_h(i));
        for (size_t i = row_start; i < row_end; ++i) {
            const float x = area_.left + static_cast<float>(i - row_start) * (card_w + kGap * s);
            const D2D1_RECT_F r = D2D1::RectF(x, y, x + card_w, y + row_h);
            if (i >= data_.cards.size()) {
                new_card_ = r;
                continue;
            }
            CardRects& cr = rects_[i];
            cr.card = r;
            const WindowOverviewCard& c = data_.cards[i];
            if (!c.current) {
                const float w = width_of(data_.switch_to) - 8.0f * s;
                cr.switch_button = D2D1::RectF(r.right - 10.0f * s - w, r.top + 12.0f * s, r.right - 10.0f * s,
                                               r.top + 40.0f * s);
            }
            float cy = r.top + kCardTop * s;
            cr.chips.clear();
            for (size_t k = 0; k < c.chips.size(); ++k) {
                cr.chips.push_back(D2D1::RectF(r.left + 10.0f * s, cy, r.right - 10.0f * s, cy + kChipH * s));
                cy += (kChipH + kChipGap) * s;
            }
            if (!c.more.empty())
                cr.more = D2D1::RectF(r.left + 12.0f * s, cy, r.right - 12.0f * s, cy + kMoreH * s);
        }
        y += row_h + kGap * s;
    }
    content_h_ = y - kGap * s - area_.top + 8.0f * s;
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content_h_ - (area_.bottom - area_.top)));
    laid_out_ = true;
}

OverviewHit WindowOverviewView::HitTest(float x, float y) const noexcept {
    OverviewHit hit;
    if (!open_) return hit;
    if (!In(panel_, x, y)) {
        hit.kind = OverviewHitKind::Outside;
        return hit;
    }
    if (In(done_button_, x, y)) {
        hit.kind = OverviewHitKind::Done;
        return hit;
    }
    for (size_t i=0;i<arrange_buttons_.size();++i) if(In(arrange_buttons_[i],x,y)) {
        hit.kind=OverviewHitKind::Arrange; hit.card=static_cast<int>(i); return hit;
    }
    hit.kind = y < panel_.top + kHeaderH * scale_ ? OverviewHitKind::Caption : OverviewHitKind::Panel;
    if (!In(area_, x, y)) return hit;
    const float dy = -scroll_;
    if (In(Shift(new_card_, dy), x, y)) {
        hit.kind = OverviewHitKind::NewWindow;
        return hit;
    }
    for (size_t i = 0; i < rects_.size(); ++i) {
        const CardRects& cr = rects_[i];
        if (!In(Shift(cr.card, dy), x, y)) continue;
        hit.card = static_cast<int>(i);
        hit.kind = OverviewHitKind::Card;
        if (In(Shift(cr.switch_button, dy), x, y)) {
            hit.kind = OverviewHitKind::Switch;
        } else if (In(Shift(cr.more, dy), x, y)) {
            hit.kind = OverviewHitKind::More;
        } else {
            for (size_t k = 0; k < cr.chips.size(); ++k) {
                if (!In(Shift(cr.chips[k], dy), x, y)) continue;
                hit.kind = OverviewHitKind::Chip;
                hit.chip = static_cast<int>(k);
                break;
            }
        }
        return hit;
    }
    return hit;
}

bool WindowOverviewView::SetHover(const OverviewHit& hit) noexcept {
    if (hit == hover_) return false;
    hover_ = hit;
    return true;
}

void WindowOverviewView::SetDrag(int card, int chip, float x, float y, const OverviewHit& over) noexcept {
    dragging_ = true;
    drag_card_ = card;
    drag_chip_ = chip;
    drag_x_ = x;
    drag_y_ = y;
    drag_over_ = over;
}

void WindowOverviewView::EndDrag() noexcept {
    dragging_ = false;
    drag_over_ = {};
}

bool WindowOverviewView::Scroll(float dip) noexcept {
    const float max_scroll = std::max(0.0f, content_h_ - (area_.bottom - area_.top));
    const float next = std::clamp(scroll_ + dip * scale_, 0.0f, max_scroll);
    if (std::abs(next - scroll_) < 0.5f) return false;
    scroll_ = next;
    return true;
}

void WindowOverviewView::ReleaseFormats() {
    Release(f_title_);
    Release(f_name_);
    Release(f_small_);
    Release(f_chip_);
    Release(f_path_);
    Release(f_center_);
    Release(f_plus_);
    format_scale_ = 0.0f;
}

void WindowOverviewView::EnsureFormats(IDWriteFactory* dwrite, IDWriteTextFormat* base) {
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
    make(18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &f_title_);
    make(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &f_name_);
    make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, &f_small_);
    make(12.5f, DWRITE_FONT_WEIGHT_NORMAL, &f_chip_);
    make(11.0f, DWRITE_FONT_WEIGHT_NORMAL, &f_path_);
    make(12.5f, DWRITE_FONT_WEIGHT_NORMAL, &f_center_);
    make(26.0f, DWRITE_FONT_WEIGHT_LIGHT, &f_plus_);
    if (f_path_) f_path_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (f_center_) f_center_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (f_plus_) f_plus_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

void WindowOverviewView::Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, IDWriteTextFormat* base_format,
                              const Theme& theme) {
    if (!open_ || !dc || !dwrite) return;
    const bool formats_changed = !f_name_;
    EnsureFormats(dwrite, base_format);
    if (dwrite_ != dwrite || formats_changed || !laid_out_) {
        dwrite_ = dwrite;  // the compositor's factory lives as long as the renderer
        Layout();          // the buttons fit their text now
    }
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &brush)) || !brush) return;
    const auto use = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); return brush; };
    const auto fill = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), use(c));
    };
    const auto stroke = [&](const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float w,
                            ID2D1StrokeStyle* style = nullptr) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), use(c), w, style);
    };
    const auto text = [&](IDWriteTextFormat* f, const std::wstring& str, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
        if (!f || str.empty() || r.right <= r.left) return;
        dc->DrawTextW(str.c_str(), static_cast<UINT32>(str.size()), f, r, use(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const float s = scale_;
    const float line = std::max(1.0f, s);
    const D2D1_COLOR_F surface = Opaque(theme.surface_flyout, theme.bg);

    // Overview is a full-window surface; the native window owns its outer frame.
    dc->FillRectangle(client_, use(surface));

    // Header: title, hint (or the notice), buttons.
    const float hx = panel_.left + kPad * s;
    const float hy0 = panel_.top, hy1 = panel_.top + kHeaderH * s;
    const float title_w = MeasureTextWidth(dwrite, f_title_, data_.title);
    text(f_title_, data_.title, D2D1::RectF(hx, hy0, hx + title_w + 2.0f * s, hy1), theme.text);
    const bool notice = !data_.notice.empty();
    const auto hint_rect = D2D1::RectF(hx + title_w + 16.0f * s, hy0,
        arrange_in_header_ && !arrange_buttons_.empty() ? arrange_buttons_.front().left - 12*s : done_button_.left - 16*s, hy1);
    if (hint_rect.right - hint_rect.left >= 100*s)
        text(f_small_, notice ? data_.notice : data_.hint, hint_rect, notice ? theme.accent : theme.text_secondary);
    if (!arrange_buttons_.empty()) {
        const auto r = arrange_buttons_.front();
        const bool hot = hover_.kind == OverviewHitKind::Arrange;
        fill(r, 6*s, hot ? theme.fill_hover : Alpha(theme.text, 0.05f));
        stroke(r, 6*s, theme.stroke_card, line);
        DrawWindowArrangementIcon(dc, D2D1::RectF(r.left+10*s,r.top+10*s,r.left+26*s,r.bottom-10*s),
            3, use(theme.text_secondary), surface);
        auto label = r; label.left += 26*s; label.right -= 12*s;
        text(f_center_, data_.arrange, label, theme.text);
        const float cx = r.right - 12*s, cy = (r.top + r.bottom)*0.5f;
        dc->DrawLine(D2D1::Point2F(cx-3*s,cy-1*s), D2D1::Point2F(cx,cy+2*s), use(theme.text_secondary), line);
        dc->DrawLine(D2D1::Point2F(cx,cy+2*s), D2D1::Point2F(cx+3*s,cy-1*s), use(theme.text_secondary), line);
    }
    const bool done_hot = hover_.kind == OverviewHitKind::Done;
    fill(done_button_, 6.0f * s, done_hot ? Alpha(theme.accent, 0.88f) : theme.accent);
    text(f_center_, data_.done, done_button_, D2D1::ColorF(1, 1, 1, 1));

    // Cards, clipped to the scrolling area.
    dc->PushAxisAlignedClip(area_, D2D1_ANTIALIAS_MODE_ALIASED);
    const float dy = -scroll_;
    const auto target_card = [&](int i) {
        return dragging_ && drag_over_.card == i && i != drag_card_ && drag_over_.kind != OverviewHitKind::NewWindow;
    };
    for (size_t i = 0; i < data_.cards.size() && i < rects_.size(); ++i) {
        const WindowOverviewCard& c = data_.cards[i];
        const CardRects& cr = rects_[i];
        const D2D1_RECT_F r = Shift(cr.card, dy);
        if (r.bottom < area_.top || r.top > area_.bottom) continue;
        const float cr_radius = 10.0f * s;
        const bool target = target_card(static_cast<int>(i));
        fill(r, cr_radius, target ? Alpha(theme.accent, 0.10f) : Alpha(theme.text, 0.035f));
        if (target)
            stroke(D2D1::RectF(r.left + s, r.top + s, r.right - s, r.bottom - s), cr_radius, theme.accent, 2.0f * s);
        else
            stroke(r, cr_radius, theme.stroke_card, line);
        // Window color along the top, then dot, name and what it holds.
        fill(D2D1::RectF(r.left + 12.0f * s, r.top + 1.0f * s, r.right - 12.0f * s, r.top + 4.0f * s), 1.5f * s,
             Rgb(c.rgb));
        const float x = r.left + 14.0f * s;
        const float right = (cr.switch_button.right > cr.switch_button.left ? cr.switch_button.left : r.right) - 8.0f * s;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + 5.0f * s, r.top + 23.0f * s), 5.0f * s, 5.0f * s),
                        use(Rgb(c.rgb)));
        float name_right = right;
        if (c.current) {
            // "This window" in a small pill where the switch button would be.
            const float pill_w = MeasureTextWidth(dwrite, f_small_, data_.current) + 16.0f * s;
            const D2D1_RECT_F pill = D2D1::RectF(r.right - 12.0f * s - pill_w, r.top + 13.0f * s, r.right - 12.0f * s,
                                                 r.top + 33.0f * s);
            fill(pill, 10.0f * s, Alpha(theme.accent, 0.14f));
            text(f_center_, data_.current, pill, theme.accent);
            name_right = pill.left - 8.0f * s;
        }
        text(f_name_, c.name, D2D1::RectF(x + 16.0f * s, r.top + 12.0f * s, name_right, r.top + 34.0f * s), theme.text);
        text(f_small_, c.meta, D2D1::RectF(x, r.top + 34.0f * s, r.right - 12.0f * s, r.top + 52.0f * s),
             theme.text_secondary);
        if (cr.switch_button.right > cr.switch_button.left) {
            const D2D1_RECT_F b = Shift(cr.switch_button, dy);
            const bool hot = hover_.kind == OverviewHitKind::Switch && hover_.card == static_cast<int>(i);
            fill(b, 6.0f * s, Alpha(theme.text, hot ? 0.10f : 0.05f));
            stroke(b, 6.0f * s, theme.stroke_card, line);
            text(f_center_, data_.switch_to, b, theme.text);
        }
        for (size_t k = 0; k < c.chips.size() && k < cr.chips.size(); ++k) {
            const WindowOverviewChip& chip = c.chips[k];
            const D2D1_RECT_F cb = Shift(cr.chips[k], dy);
            const bool dragged = dragging_ && drag_card_ == static_cast<int>(i) && drag_chip_ == static_cast<int>(k);
            const bool hot = !dragging_ && hover_.kind == OverviewHitKind::Chip && hover_.card == static_cast<int>(i) &&
                             hover_.chip == static_cast<int>(k);
            const float a = dragged ? 0.35f : 1.0f;
            fill(cb, 6.0f * s, Alpha(theme.text, (chip.active ? 0.09f : 0.05f) * a));
            if (hot) stroke(cb, 6.0f * s, Alpha(theme.text, 0.28f), line);
            if (chip.active)
                fill(D2D1::RectF(cb.left + 3.0f * s, cb.top + 8.0f * s, cb.left + 6.0f * s, cb.bottom - 8.0f * s),
                     1.5f * s, Alpha(theme.accent, a));
            const float w = cb.right - cb.left - 22.0f * s;
            const float title_max = w * 0.58f;
            const float tw = std::min(MeasureTextWidth(dwrite, f_chip_, chip.title) + 2.0f * s, title_max);
            text(f_chip_, chip.title, D2D1::RectF(cb.left + 12.0f * s, cb.top, cb.left + 12.0f * s + tw, cb.bottom),
                 Alpha(theme.text, a));
            const float px = cb.left + 12.0f * s + tw + 10.0f * s;
            const float pw = cb.right - 10.0f * s - px;
            if (pw > 24.0f * s)
                text(f_path_, FitPathTail(dwrite, f_path_, chip.path, pw), D2D1::RectF(px, cb.top, cb.right - 10.0f * s, cb.bottom),
                     Alpha(theme.text_secondary, 0.9f * a));
        }
        if (!c.more.empty()) {
            const bool hot = hover_.kind == OverviewHitKind::More && hover_.card == static_cast<int>(i);
            text(f_small_, c.more, Shift(cr.more, dy), hot ? theme.accent : theme.text_secondary);
        }
    }
    // "New window": a dashed drop target.
    {
        const D2D1_RECT_F r = Shift(new_card_, dy);
        const bool target = dragging_ && drag_over_.kind == OverviewHitKind::NewWindow;
        ID2D1Factory* factory = nullptr;
        ID2D1StrokeStyle* dashed = nullptr;
        dc->GetFactory(&factory);
        if (factory) {
            const D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.0f,
                D2D1_DASH_STYLE_DASH, 0.0f);
            factory->CreateStrokeStyle(props, nullptr, 0, &dashed);
            factory->Release();
        }
        if (target) fill(r, 10.0f * s, Alpha(theme.accent, 0.10f));
        stroke(D2D1::RectF(r.left + s, r.top + s, r.right - s, r.bottom - s), 10.0f * s,
               target ? theme.accent : Alpha(theme.text, 0.30f), (target ? 2.0f : 1.5f) * s, dashed);
        Release(dashed);
        const float cy = (r.top + r.bottom) * 0.5f;
        const D2D1_COLOR_F ink = target ? theme.accent : theme.text_secondary;
        text(f_plus_, L"+", D2D1::RectF(r.left, cy - 34.0f * s, r.right, cy), ink);
        text(f_center_, data_.new_window, D2D1::RectF(r.left + 10.0f * s, cy, r.right - 10.0f * s, cy + 24.0f * s), ink);
    }
    dc->PopAxisAlignedClip();

    // A thin bar when the cards scroll.
    const float view_h = area_.bottom - area_.top;
    if (content_h_ > view_h + 1.0f) {
        const float bar_h = std::max(view_h * view_h / content_h_, 24.0f * s);
        const float bar_y = area_.top + (view_h - bar_h) * (scroll_ / (content_h_ - view_h));
        fill(D2D1::RectF(panel_.right - 8.0f * s, bar_y, panel_.right - 5.0f * s, bar_y + bar_h), 1.5f * s,
             Alpha(theme.text, 0.25f));
    }

    // The dragged tab follows the cursor.
    if (dragging_ && drag_card_ >= 0 && static_cast<size_t>(drag_card_) < data_.cards.size()) {
        const WindowOverviewCard& c = data_.cards[static_cast<size_t>(drag_card_)];
        if (drag_chip_ >= 0 && static_cast<size_t>(drag_chip_) < c.chips.size()) {
            const std::wstring& title = c.chips[static_cast<size_t>(drag_chip_)].title;
            const float w = std::min(MeasureTextWidth(dwrite, f_chip_, title) + 40.0f * s, 280.0f * s);
            const D2D1_RECT_F g = D2D1::RectF(drag_x_ - 14.0f * s, drag_y_ - kChipH * 0.5f * s, drag_x_ - 14.0f * s + w,
                                              drag_y_ + kChipH * 0.5f * s);
            for (int i = 3; i >= 1; --i) {
                const float sh = static_cast<float>(i) * 2.0f * s;
                fill(D2D1::RectF(g.left - sh, g.top - sh + 4.0f * s, g.right + sh, g.bottom + sh + 4.0f * s), 6.0f * s + sh,
                     D2D1::ColorF(0, 0, 0, 0.06f));
            }
            fill(g, 6.0f * s, surface);
            stroke(g, 6.0f * s, theme.accent, 1.5f * s);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(g.left + 13.0f * s, (g.top + g.bottom) * 0.5f), 4.0f * s, 4.0f * s),
                            use(Rgb(c.rgb)));
            text(f_chip_, title, D2D1::RectF(g.left + 24.0f * s, g.top, g.right - 10.0f * s, g.bottom), theme.text);
        }
    }
    brush->Release();
}

}  // namespace pulse::ui
