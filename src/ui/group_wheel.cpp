#include "group_wheel.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "snack_style.h"
#include "command_icons.h"
#include "typography.h"
#include "ui_motion.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

constexpr float kGwPi = 3.14159265f;
constexpr float kStepDeg = 32.0f;    // degrees per row on the cylinder
constexpr float kRadius = 115.0f;    // cylinder radius (DIP)
constexpr float kPerspective = 520.0f;
constexpr float kDragPitch = 60.0f;  // drag distance per row (DIP)
constexpr float kSpringK = 240.0f;   // critically damped roll
constexpr float kFooterDip = 52.0f;  // bottom strip: Cancel / Apply (32 DIP buttons)

D2D1_COLOR_F GwRgb(uint32_t rgb, float a) {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, a);
}

D2D1_COLOR_F GwAlpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

D2D1_COLOR_F GwOpaque(const D2D1_COLOR_F& top, const D2D1_COLOR_F& under) {
    const float a = std::clamp(top.a, 0.0f, 1.0f);
    return D2D1::ColorF(top.r * a + under.r * (1.0f - a), top.g * a + under.g * (1.0f - a),
                        top.b * a + under.b * (1.0f - a), 1.0f);
}

float GwEaseOut(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

template <class T> void GwRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

constexpr uint32_t kSkeletonColors[] = {0x60A5FA, 0xF472B6, 0xFBBF24, 0x34D399,
                                        0xA78BFA, 0xF87171, 0x22D3EE};

// Per-draw state: one brush re-coloured for every primitive.
struct Ink {
    ID2D1DeviceContext* dc = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteFactory* dwrite = nullptr;
    ID2D1SolidColorBrush* Use(const D2D1_COLOR_F& c) { brush->SetColor(c); return brush; }
    void Fill(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Use(c));
    }
    void Stroke(const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& c, float w) {
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Use(c), w);
    }
    void Text(IDWriteTextFormat* f, const std::wstring& s, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
        if (!f || s.empty() || r.right <= r.left) return;
        dc->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r, Use(c),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    float Measure(IDWriteTextFormat* f, const std::wstring& s) const {
        if (!f || !dwrite || s.empty()) return 0.0f;
        IDWriteTextLayout* layout = nullptr;
        float w = 0.0f;
        if (SUCCEEDED(dwrite->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f,
                                               4096.0f, 256.0f, &layout)) && layout) {
            DWRITE_TEXT_METRICS m{};
            if (SUCCEEDED(layout->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
            layout->Release();
        }
        return w;
    }
};

Ink g_ink;  // valid only inside GroupWheel::Draw (UI thread)

} // namespace

void GroupWheel::Open(GroupWheelData data, const D2D1_RECT_F& anchor, const D2D1_RECT_F& client,
                      float scale, bool animate) {
    data_ = std::move(data);
    scale_ = scale;
    animate_ = animate;
    const float s = scale_;
    const float w = std::max(360.0f * s, std::min(480.0f * s, (client.right - client.left) - 24.0f * s));
    const float top = anchor.bottom + 6.0f * s;
    const float h = std::max(232.0f * s, std::min(312.0f * s, client.bottom - top - 12.0f * s));
    const float left = std::clamp(anchor.left, client.left + 12.0f * s,
                                  std::max(client.left + 12.0f * s, client.right - w - 12.0f * s));
    panel_ = D2D1::RectF(left, top, left + w, top + h);
    apply_all_rect_ = {};
    apply_all_hot_ = false;
    cancel_rect_ = ok_rect_ = {};
    footer_hot_ = footer_pressed_ = 0;
    hover_step_ = kNoHover;
    seg_hot_ = -1;
    const int applied = std::clamp(data_.applied, 0, std::max(0, Count() - 1));
    t_ = static_cast<float>(applied);
    p_ = animate_ ? t_ - 0.6f : t_;   // roll in to the current choice
    v_ = 0.0f;
    open_ = true;
    closing_ = false;
    dragging_ = moved_ = false;
    fade_start_ = motion::NowMs();
    last_tick_ = 0;
    shown_ = shown_prev_ = -1;
    shown_at_ = 0;
    fade_value_ = animate_ ? 0.0f : 1.0f;
}

void GroupWheel::Close() {
    if (!open_) return;
    open_ = false;
    dragging_ = false;
    closing_ = animate_;
    fade_start_ = motion::NowMs();
    if (!closing_) fade_value_ = 0.0f;
}

bool GroupWheel::Contains(float x, float y) const noexcept {
    return open_ && x >= panel_.left && x < panel_.right && y >= panel_.top && y < panel_.bottom;
}

bool GroupWheel::HitApplyAll(float x, float y) const noexcept {
    const auto& r = apply_all_rect_;
    return open_ && r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

namespace {
bool InRect(const D2D1_RECT_F& r, float x, float y) noexcept {
    return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
}  // namespace

int GroupWheel::HitFooter(float x, float y) const noexcept {
    if (!open_) return 0;
    if (InRect(ok_rect_, x, y)) return kFooterApply;
    if (InRect(cancel_rect_, x, y)) return kFooterCancel;
    return 0;
}

bool GroupWheel::InDrum(float x, float y) const noexcept {
    return open_ && InRect(DrumRect(), x, y);
}

int GroupWheel::RowStepAt(float x, float y) const noexcept {
    if (!InDrum(x, y)) return kNoHover;
    const D2D1_RECT_F drum = DrumRect();
    const float cy = (drum.top + drum.bottom) * 0.5f;
    const float rel = std::clamp((y - cy) / (kRadius * scale_), -1.0f, 1.0f);
    const int step = static_cast<int>(std::lround(std::asin(rel) * 180.0f / kGwPi / kStepDeg));
    const int target = static_cast<int>(std::lround(Clamp(t_))) + step;
    return step == 0 || (target >= 0 && target < Count()) ? step : kNoHover;
}

bool GroupWheel::Hover(float x, float y) noexcept {
    const bool all = HitApplyAll(x, y);
    const int footer = HitFooter(x, y);
    const int step = dragging_ ? kNoHover : RowStepAt(x, y);
    int seg = -1;
    if (open_ && data_.sort_controls) {
        for (int k = 0; k < 2 && seg < 0; ++k)
            for (int i = 0; i < (k == 0 ? 2 : 3); ++i)
                if (InRect(SegmentRect(k, i), x, y)) { seg = k * 4 + i; break; }
    }
    if (all == apply_all_hot_ && footer == footer_hot_ && step == hover_step_ && seg == seg_hot_) return false;
    seg_hot_ = seg;
    apply_all_hot_ = all;
    footer_hot_ = footer;
    hover_step_ = step;
    return true;
}

float GroupWheel::Clamp(float v) const noexcept {
    return std::clamp(v, 0.0f, static_cast<float>(std::max(0, Count() - 1)));
}

int GroupWheel::CentreIndex() const noexcept {
    return static_cast<int>(std::lround(Clamp(p_)));
}

D2D1_RECT_F GroupWheel::DrumRect() const noexcept {
    // The bottom strip under the drum holds the key hints (DrawFooter).
    return D2D1::RectF(panel_.left, panel_.top,
                       panel_.left + (panel_.right - panel_.left) * 0.56f, panel_.bottom - kFooterDip * scale_);
}

void GroupWheel::Step(int delta) {
    t_ = Clamp(std::round(t_) + static_cast<float>(delta));
    if (!animate_) p_ = t_;
}

void GroupWheel::StepTo(int index) {
    t_ = Clamp(static_cast<float>(index));
    if (!animate_) p_ = t_;
}

bool GroupWheel::PointerDown(float x, float y) {
    if (open_ && data_.sort_controls) {
        for (int k = 0; k < 2; ++k) {
            for (int i = 0; i < (k == 0 ? 2 : 3); ++i) {
                const D2D1_RECT_F r = SegmentRect(k, i);
                if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
                    if (k == 0) data_.desc = i == 1;
                    else data_.folder = i;
                    return false;
                }
            }
        }
    }
    const D2D1_RECT_F drum = DrumRect();
    if (!open_ || x < drum.left || x >= drum.right || y < drum.top || y >= drum.bottom) return false;
    dragging_ = true;
    moved_ = false;
    drag_y0_ = y;
    drag_p0_ = p_;
    v_ = 0.0f;
    return true;
}

void GroupWheel::PointerMove(float y) {
    if (!dragging_) return;
    if (std::abs(y - drag_y0_) > 3.0f * scale_) moved_ = true;
    float np = drag_p0_ - (y - drag_y0_) / (kDragPitch * scale_);
    const float last = static_cast<float>(std::max(0, Count() - 1));
    if (np < 0.0f) np *= 0.35f;                        // rubber band past the ends
    if (np > last) np = last + (np - last) * 0.35f;
    p_ = np;
    if (!animate_) t_ = Clamp(std::round(np));
}

int GroupWheel::PointerUp(float y) {
    if (!dragging_) return -1;
    dragging_ = false;
    if (moved_) {
        t_ = Clamp(std::round(p_));
        if (!animate_) p_ = t_;
        return -1;
    }
    const D2D1_RECT_F drum = DrumRect();
    const float cy = (drum.top + drum.bottom) * 0.5f;
    // Invert the cylinder projection: rows sit at R*sin(angle) from the centre.
    const float rel = std::clamp((y - cy) / (kRadius * scale_), -1.0f, 1.0f);
    const int step = static_cast<int>(std::lround(std::asin(rel) * 180.0f / kGwPi / kStepDeg));
    if (step == 0) return SelectedValue();              // the centred row applies
    Step(step);                                         // a row above/below rolls to it
    return -1;
}

int GroupWheel::SelectedValue() const {
    if (data_.options.empty()) return 0;
    return data_.options[static_cast<size_t>(std::lround(Clamp(t_)))].value;
}

bool GroupWheel::Tick(uint64_t now) {
    if (!Visible()) return false;
    bool active = false;
    const double dt = last_tick_ ? std::min(0.05, static_cast<double>(now - last_tick_) / 1000.0) : 1.0 / 60.0;
    last_tick_ = now;
    if (animate_) {
        const float dur = closing_ ? 110.0f : 160.0f;
        const float u = std::clamp(static_cast<float>(now - fade_start_) / dur, 0.0f, 1.0f);
        fade_value_ = closing_ ? 1.0f - GwEaseOut(u) : GwEaseOut(u);
        if (u < 1.0f) active = true;
        else if (closing_) {
            closing_ = false;
            fade_value_ = 0.0f;
            return false;
        }
    } else {
        fade_value_ = open_ ? 1.0f : 0.0f;
        closing_ = false;
    }
    if (!dragging_) {
        if (animate_) {
            const float c = 2.0f * std::sqrt(kSpringK);
            const int steps = std::max(1, static_cast<int>(std::ceil(dt * 240.0)));
            const float h = static_cast<float>(dt) / static_cast<float>(steps);
            for (int i = 0; i < steps; ++i) {
                v_ += (-kSpringK * (p_ - t_) - c * v_) * h;
                p_ += v_ * h;
            }
            if (std::abs(p_ - t_) < 0.0005f && std::abs(v_) < 0.005f) {
                p_ = t_;
                v_ = 0.0f;
            } else {
                active = true;
            }
        } else {
            p_ = t_;
        }
    }
    const int centre = CentreIndex();
    if (centre != shown_) {
        shown_prev_ = shown_;
        shown_ = centre;
        shown_at_ = now;
    }
    if (animate_ && shown_prev_ >= 0 && now - shown_at_ < 320) active = true;
    return active || dragging_;
}

void GroupWheel::ReleaseFormats() {
    GwRelease(f_name_);
    GwRelease(f_small_);
    GwRelease(f_head_);
    GwRelease(f_kbd_);
    GwRelease(f_seg_);
    GwRelease(stroke_);
    format_scale_ = 0.0f;
}

void GroupWheel::EnsureFormats(IDWriteFactory* dwrite, IDWriteTextFormat* base) {
    // Text follows Settings > Interface font size; the cache key carries it too.
    const float text_scale = scale_ * typography::UiFontScale();
    if (f_name_ && std::abs(format_scale_ - text_scale) < 0.001f) return;
    GwRelease(f_name_);
    GwRelease(f_small_);
    GwRelease(f_head_);
    GwRelease(f_kbd_);
    GwRelease(f_seg_);
    format_scale_ = text_scale;
    wchar_t family[128] = L"Segoe UI";
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"zh-cn";
    if (base) {
        base->GetFontFamilyName(family, 128);
        base->GetLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    }
    auto make = [&](const wchar_t* fam, float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** out) {
        if (FAILED(dwrite->CreateTextFormat(fam, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, size * text_scale, locale, out)) || !*out)
            return;
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        (*out)->SetTrimming(&trim, nullptr);
    };
    make(family, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &f_name_);
    make(family, 12.0f, DWRITE_FONT_WEIGHT_NORMAL, &f_small_);
    make(family, 12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &f_head_);
    make(L"Consolas", 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &f_kbd_);
    if (f_kbd_) f_kbd_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    make(family, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &f_seg_);
    if (f_seg_) f_seg_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

void GroupWheel::DrawRow(ID2D1DeviceContext* dc, const Theme& theme, int index, float alpha) {
    const float s = scale_;
    const float d = static_cast<float>(index) - p_;
    const float deg = d * kStepDeg;
    if (std::abs(deg) >= 88.0f) return;
    const float rad = deg * kGwPi / 180.0f;
    const float radius = kRadius * s, persp = kPerspective * s;
    const float z = radius * (std::cos(rad) - 1.0f);
    const float k = persp / (persp - z);
    const float vs = std::cos(rad) * k;
    const float op = std::max(0.0f, 1.0f - std::abs(d) * 0.22f) * alpha;
    if (op <= 0.01f || vs <= 0.05f) return;
    const D2D1_RECT_F drum = DrumRect();
    const float cx = (drum.left + drum.right) * 0.5f;
    const float cy = (drum.top + drum.bottom) * 0.5f;
    const float yc = cy + radius * std::sin(rad) * k;

    D2D1_MATRIX_3X2_F base{};
    dc->GetTransform(&base);
    dc->SetTransform(D2D1::Matrix3x2F::Scale(k, vs, D2D1::Point2F(cx, 0.0f)) *
                     D2D1::Matrix3x2F::Translation(0.0f, yc) * base);

    const auto& o = data_.options[static_cast<size_t>(index)];
    const bool cur = index == CentreIndex();
    const float left = drum.left + 24.0f * s, right = drum.right - 24.0f * s;
    // Icon badge.
    const D2D1_RECT_F badge = D2D1::RectF(left, -16.0f * s, left + 32.0f * s, 16.0f * s);
    g_ink.Fill(badge, 8.0f * s, GwAlpha(theme.text, 0.06f * op));
    const D2D1_COLOR_F icon_color = GwAlpha(cur ? theme.accent : theme.text_secondary, op);
    command_icons::Draw(dc, g_ink.Use(icon_color), stroke_, static_cast<command_icons::Icon>(o.icon),
                        command_icons::CenteredBounds(badge, 18.0f * s));
    // Pill on the right.
    // Sort picker: no pill - the Order segment already shows the direction,
    // and the row needs the width for the name and its explanation.
    // The pointer on the centre card: the pill says a click applies (both pickers).
    const bool go = cur && hover_step_ == 0 && !dragging_ && std::abs(p_ - t_) < 0.05f &&
                    !data_.click_apply_text.empty();
    const std::wstring pill_text = go ? data_.click_apply_text
                                 : data_.sort_controls ? std::wstring() : o.pill;
    const float pill_w = g_ink.Measure(f_small_, pill_text) + 16.0f * s;
    const D2D1_RECT_F pill = D2D1::RectF(right - pill_w, -10.0f * s, right, 10.0f * s);
    if (!pill_text.empty()) {
        g_ink.Fill(pill, 10.0f * s, go ? GwAlpha(theme.accent, op)
                                 : cur ? GwAlpha(theme.accent, 0.14f * op) : GwAlpha(theme.text, 0.07f * op));
        g_ink.Text(f_small_, pill_text, D2D1::RectF(pill.left + 8.0f * s, pill.top, pill.right, pill.bottom),
                   GwAlpha(go ? theme.accent_text : cur ? theme.accent : theme.text_secondary, op));
    }
    // Name and meta.
    const float tx = left + 44.0f * s, tr = pill_text.empty() ? right : pill.left - 10.0f * s;
    g_ink.Text(f_name_, o.name, D2D1::RectF(tx, -21.0f * s, tr, 0.0f), GwAlpha(theme.text, op));
    std::wstring meta = o.meta;
    if (index == data_.applied && !data_.current_text.empty())
        meta = meta.empty() ? data_.current_text : data_.current_text + L" \x00B7 " + meta;
    g_ink.Text(f_small_, meta, D2D1::RectF(tx, 1.0f * s, tr, 19.0f * s), GwAlpha(theme.text_secondary, op));
    dc->SetTransform(base);
}

void GroupWheel::DrawPreview(ID2D1DeviceContext* dc, const Theme& theme, const D2D1_RECT_F& pv,
                             int index, float alpha) {
    if (index < 0 || index >= Count() || alpha <= 0.01f) return;
    const auto& o = data_.options[static_cast<size_t>(index)];
    const float w = pv.right - pv.left, h = pv.bottom - pv.top;
    const bool dark = theme.bg.r + theme.bg.g + theme.bg.b < 1.5f;
    const float wash = dark ? 0.55f : 0.35f;
    // Colour wash: two soft radial blobs (the mockup's blurred orb).
    auto blob = [&](float fx, float fy, float rx, float ry, uint32_t rgb) {
        D2D1_GRADIENT_STOP stops[2] = {{0.0f, GwRgb(rgb, wash * alpha)}, {1.0f, GwRgb(rgb, 0.0f)}};
        ID2D1GradientStopCollection* coll = nullptr;
        if (FAILED(dc->CreateGradientStopCollection(stops, 2, &coll)) || !coll) return;
        ID2D1RadialGradientBrush* brush = nullptr;
        const D2D1_POINT_2F c = D2D1::Point2F(pv.left + w * fx, pv.top + h * fy);
        if (SUCCEEDED(dc->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(c, D2D1::Point2F(0, 0), w * rx, h * ry), coll, &brush)) && brush) {
            // Follow the preview box's rounded corners (a plain rect squares them off).
            dc->FillRoundedRectangle(D2D1::RoundedRect(pv, 12.0f * scale_, 12.0f * scale_), brush);
            brush->Release();
        }
        coll->Release();
    };
    blob(0.30f, 0.32f, 0.62f, 0.72f, o.hue1);
    blob(0.76f, 0.80f, 0.55f, 0.62f, o.hue2);
}

void GroupWheel::DrawFooter(ID2D1DeviceContext* dc, fluent::Painter* painter) {
    apply_all_rect_ = {};
    cancel_rect_ = ok_rect_ = {};
    if (!painter) return;
    const float s = scale_;
    const float a = animate_ ? fade_value_ : 1.0f;
    const float mid = panel_.bottom - kFooterDip * 0.5f * s;
    const float top = mid - 16.0f * s, bottom = mid + 16.0f * s;
    // The shared buttons draw opaque: fade them with the panel.
    const bool layered = a < 0.999f;
    if (layered)
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), a), nullptr);
    // Right: Cancel, Apply (the keys that do the same inside them).
    static constexpr wchar_t kEnter[] = L"Enter";
    static constexpr wchar_t kEsc[] = L"Esc";
    const float pad = 8.0f * s;   // wider than the default 8 DIP sides
    const float ok_w = painter->MeasureButtonWidth(data_.apply_text, {}, false, kEnter) + pad;
    const float cancel_w = painter->MeasureButtonWidth(data_.cancel_text, {}, false, kEsc) + pad;
    const float right = panel_.right - 16.0f * s;
    ok_rect_ = D2D1::RectF(right - ok_w, top, right, bottom);
    cancel_rect_ = D2D1::RectF(ok_rect_.left - 8.0f * s - cancel_w, top, ok_rect_.left - 8.0f * s, bottom);
    auto button = [&](const D2D1_RECT_F& r, const std::wstring& text, const wchar_t* key,
                      fluent::ButtonKind kind, int id) {
        fluent::ButtonSpec spec;
        spec.bounds = r;
        spec.text = text;
        spec.keycap = key;
        spec.kind = kind;
        spec.state.hovered = footer_hot_ == id;
        spec.state.pressed = footer_pressed_ == id && footer_hot_ == id;
        painter->DrawButton(spec);
    };
    button(cancel_rect_, data_.cancel_text, kEsc, fluent::ButtonKind::Standard, kFooterCancel);
    button(ok_rect_, data_.apply_text, kEnter, fluent::ButtonKind::Primary, kFooterApply);
    // Left: "apply to all folders" (not for session-only groups).
    const bool apply_all = data_.options.empty() ||
        data_.options[static_cast<size_t>(std::lround(Clamp(t_)))].apply_all;
    if (!data_.apply_all_text.empty() && apply_all) {
        const float left = panel_.left + 12.0f * s;
        const float w = std::min(painter->MeasureButtonWidth(data_.apply_all_text) + pad,
                                 cancel_rect_.left - 12.0f * s - left);
        if (w > 40.0f * s) {
            apply_all_rect_ = D2D1::RectF(left, top, left + w, bottom);
            fluent::ButtonSpec spec;
            spec.bounds = apply_all_rect_;
            spec.text = data_.apply_all_text;
            spec.kind = fluent::ButtonKind::Transparent;
            spec.state.hovered = apply_all_hot_;
            painter->DrawButton(spec);
        }
    }
    if (layered) dc->PopLayer();
}

void GroupWheel::Draw(ID2D1DeviceContext* dc, IDWriteFactory* dwrite, IDWriteTextFormat* base_format,
                      const Theme& theme, fluent::Painter* painter) {
    if (!dc || !dwrite || !Visible() || data_.options.empty()) return;
    const float a = animate_ ? fade_value_ : 1.0f;
    if (a <= 0.001f) return;
    EnsureFormats(dwrite, base_format);
    if (!stroke_) command_icons::CreateStrokeStyle(dc, &stroke_);
    ID2D1SolidColorBrush* brush = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &brush)) || !brush) return;
    g_ink = Ink{dc, brush, dwrite};
    const float s = scale_;

    D2D1_MATRIX_3X2_F base{};
    dc->GetTransform(&base);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, (1.0f - a) * -6.0f * s) * base);

    // Flyout shadow, as CSS 0 6px 18px + 0 1px 3px: Gaussian, so it fades out
    // instead of ending in a band; dark themes need it darker to show at all.
    const float radius = 14.0f * s;
    const bool dark = theme.bg.r + theme.bg.g + theme.bg.b < 1.5f;
    DrawElevationShadow(dc, panel_, radius, 9.0f * s, 6.0f * s, D2D1::ColorF(0, 0, 0, dark ? 0.38f : 0.10f), a);
    DrawElevationShadow(dc, panel_, radius, 1.5f * s, 1.0f * s, D2D1::ColorF(0, 0, 0, dark ? 0.30f : 0.06f), a);
    const D2D1_COLOR_F surface = GwOpaque(theme.surface_flyout, theme.bg);
    g_ink.Fill(panel_, radius, GwAlpha(surface, a));
    g_ink.Stroke(panel_, radius, GwAlpha(theme.stroke_card, a), std::max(1.0f, s));

    // Drum: highlight card at the centre, rows back to front.
    const D2D1_RECT_F drum = DrumRect();
    dc->PushAxisAlignedClip(drum, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float cy = (drum.top + drum.bottom) * 0.5f;
    const D2D1_RECT_F card = D2D1::RectF(drum.left + 12.0f * s, cy - 29.0f * s, drum.right - 12.0f * s, cy + 29.0f * s);
    const bool card_hot = hover_step_ == 0 && !dragging_;
    g_ink.Fill(card, 10.0f * s, GwAlpha(theme.accent, (card_hot ? 0.16f : 0.10f) * a));
    // A row above or below under the pointer: a click rolls to it.
    if (hover_step_ != 0 && hover_step_ != kNoHover && !dragging_) {
        const float row = std::round(Clamp(t_)) + static_cast<float>(hover_step_);
        const float rad = (row - p_) * kStepDeg * kGwPi / 180.0f;
        if (std::abs(rad) < 1.5f) {
            const float radius_px = kRadius * s, persp = kPerspective * s;
            const float k = persp / (persp - radius_px * (std::cos(rad) - 1.0f));
            const float yc = cy + radius_px * std::sin(rad) * k;
            const float half = 21.0f * s * std::cos(rad) * k;
            g_ink.Fill(D2D1::RectF(card.left, yc - half, card.right, yc + half), 8.0f * s,
                       GwAlpha(theme.text, 0.045f * a));
        }
    }
    g_ink.Stroke(card, 10.0f * s, GwAlpha(theme.accent, 0.38f * a), std::max(1.0f, s));
    g_ink.Fill(D2D1::RectF(card.left - 1.0f * s, card.top + 14.0f * s, card.left + 3.0f * s, card.bottom - 14.0f * s),
               2.0f * s, GwAlpha(theme.accent, a));
    std::vector<int> order(static_cast<size_t>(Count()));
    for (int i = 0; i < Count(); ++i) order[static_cast<size_t>(i)] = i;
    std::sort(order.begin(), order.end(), [&](int l, int r) {
        return std::abs(static_cast<float>(l) - p_) > std::abs(static_cast<float>(r) - p_);
    });
    for (int i : order) DrawRow(dc, theme, i, a);
    dc->PopAxisAlignedClip();

    // Preview: colour wash cross-fades; headers and skeleton rows fade out then in.
    const D2D1_RECT_F pv = D2D1::RectF(drum.right + 22.0f * s, panel_.top + 22.0f * s,
                                       panel_.right - 22.0f * s, panel_.bottom - (kFooterDip + 4.0f) * s - ControlsHeight());
    if (pv.right - pv.left > 40.0f * s && pv.bottom - pv.top > 40.0f * s) {
        g_ink.Fill(pv, 12.0f * s, GwAlpha(theme.text, 0.035f * a));
        dc->PushAxisAlignedClip(pv, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const uint64_t now = motion::NowMs();
        const float elapsed = static_cast<float>(now - shown_at_);
        const bool fading = animate_ && shown_prev_ >= 0 && elapsed < 320.0f;
        const float wash_in = fading ? std::clamp(elapsed / 300.0f, 0.0f, 1.0f) : 1.0f;
        if (fading) DrawPreview(dc, theme, pv, shown_prev_, a * (1.0f - wash_in));
        DrawPreview(dc, theme, pv, shown_, a * wash_in);
        // Stage content.
        auto stage = [&](int index, float alpha) {
            if (index < 0 || index >= Count() || alpha <= 0.01f) return;
            const auto& o = data_.options[static_cast<size_t>(index)];
            const float x = pv.left + 14.0f * s, w = pv.right - pv.left - 28.0f * s;
            float y = pv.top + 12.0f * s;
            auto bar = [&](float pct, uint32_t rgb) {
                const float bw = w * pct;
                g_ink.Fill(D2D1::RectF(x, y, x + bw, y + 15.0f * s), 4.0f * s, GwAlpha(theme.text, 0.08f * alpha));
                g_ink.Fill(D2D1::RectF(x + 4.0f * s, y + 3.5f * s, x + 12.0f * s, y + 11.5f * s), 2.0f * s, GwRgb(rgb, alpha));
                y += 20.0f * s;
            };
            if (data_.sort_controls) {
                const auto& rows = o.files[static_cast<size_t>((data_.desc ? 3 : 0) + std::clamp(data_.folder, 0, 2))];
                for (const auto& row : rows) {
                    if (y + 20.0f * s > pv.bottom - 6.0f * s) break;
                    g_ink.Fill(D2D1::RectF(x, y + 5.0f * s, x + 11.0f * s, y + 16.0f * s), 3.0f * s, GwRgb(row.rgb, alpha));
                    const float vw = std::min(g_ink.Measure(f_small_, row.value), w * 0.55f);
                    g_ink.Text(f_small_, row.value, D2D1::RectF(x + w - vw, y, x + w + 2.0f * s, y + 21.0f * s),
                               GwAlpha(theme.accent, alpha));
                    g_ink.Text(f_small_, row.name, D2D1::RectF(x + 18.0f * s, y, x + w - vw - 8.0f * s, y + 21.0f * s),
                               GwAlpha(theme.text, alpha));
                    y += 22.0f * s;
                }
                return;
            }
            if (o.groups.empty()) {
                for (int r = 0; r < 11 && y + 15.0f * s < pv.bottom - 8.0f * s; ++r)
                    bar(static_cast<float>(58 + (r * 37) % 38) / 100.0f, kSkeletonColors[r % 7]);
                return;
            }
            int gi = 0;
            for (const auto& [label, count] : o.groups) {
                if (y + 24.0f * s > pv.bottom - 8.0f * s) break;
                const float lw = g_ink.Measure(f_head_, label);
                g_ink.Text(f_head_, label, D2D1::RectF(x, y, x + std::min(lw + 2.0f * s, w), y + 20.0f * s),
                           GwAlpha(theme.accent, alpha));
                const std::wstring n = std::to_wstring(count);
                const float nw = g_ink.Measure(f_small_, n);
                const float nx = x + lw + 6.0f * s;
                g_ink.Text(f_small_, n, D2D1::RectF(nx, y, nx + nw + 2.0f * s, y + 20.0f * s),
                           GwAlpha(theme.text_secondary, alpha));
                const float lx = nx + nw + 8.0f * s;
                if (lx < x + w)
                    g_ink.Fill(D2D1::RectF(lx, y + 10.0f * s, x + w, y + 10.0f * s + std::max(1.0f, s)), 0.0f,
                               GwAlpha(theme.accent, 0.30f * alpha));
                y += 24.0f * s;
                for (int r = 0; r < std::min(count, 2) && y + 15.0f * s < pv.bottom - 8.0f * s; ++r)
                    bar(static_cast<float>(52 + (gi * 13 + r * 29) % 42) / 100.0f,
                        kSkeletonColors[(gi * 2 + r) % 7]);
                y += 4.0f * s;
                ++gi;
            }
        };
        if (fading && elapsed < 120.0f) stage(shown_prev_, a * (1.0f - elapsed / 120.0f));
        else stage(shown_, fading ? a * std::clamp((elapsed - 120.0f) / 150.0f, 0.0f, 1.0f) : a);
        dc->PopAxisAlignedClip();
    }
    if (data_.sort_controls) DrawControls(theme, a);
    DrawFooter(dc, painter);

    dc->SetTransform(base);
    g_ink = Ink{};
    brush->Release();
}

// Sort segments sit above the footer, right column: label, bar, label, bar.
float GroupWheel::ControlsHeight() const noexcept {
    return data_.sort_controls ? 98.0f * scale_ : 0.0f;
}

D2D1_RECT_F GroupWheel::SegmentRect(int k, int i) const noexcept {
    const float s = scale_;
    const D2D1_RECT_F drum = DrumRect();
    const float x0 = drum.right + 22.0f * s, x1 = panel_.right - 22.0f * s;
    const float top = panel_.bottom - (kFooterDip + 4.0f) * s - 88.0f * s + (k == 0 ? 16.0f : 64.0f) * s;
    const D2D1_RECT_F bar = D2D1::RectF(x0, top, x1, top + 24.0f * s);
    if (i < 0) return bar;
    const int n = k == 0 ? 2 : 3;
    const float w = (x1 - x0) / static_cast<float>(n);
    return D2D1::RectF(x0 + w * static_cast<float>(i) + 2.0f * s, top + 2.0f * s,
                       x0 + w * static_cast<float>(i + 1) - 2.0f * s, top + 22.0f * s);
}

void GroupWheel::DrawControls(const Theme& theme, float alpha) {
    const float s = scale_;
    const int centre = CentreIndex();
    const auto& o = data_.options[static_cast<size_t>(std::clamp(centre, 0, Count() - 1))];
    for (int k = 0; k < 2; ++k) {
        const D2D1_RECT_F bar = SegmentRect(k, -1);
        const std::wstring& label = k == 0 ? data_.direction_text : data_.folder_text;
        g_ink.Text(f_small_, label, D2D1::RectF(bar.left + 2.0f * s, bar.top - 17.0f * s, bar.right, bar.top - 1.0f * s),
                   GwAlpha(theme.text_secondary, alpha));
        g_ink.Fill(bar, 8.0f * s, GwAlpha(theme.text, 0.05f * alpha));
        const int n = k == 0 ? 2 : 3;
        const int selected = k == 0 ? (data_.desc ? 1 : 0) : std::clamp(data_.folder, 0, 2);
        for (int i = 0; i < n; ++i) {
            const D2D1_RECT_F r = SegmentRect(k, i);
            const bool on = i == selected;
            if (on) g_ink.Fill(r, 6.0f * s, GwAlpha(theme.accent, 0.18f * alpha));
            else if (seg_hot_ == k * 4 + i) g_ink.Fill(r, 6.0f * s, GwAlpha(theme.text, 0.06f * alpha));
            std::wstring t = k == 0 ? (i == 0 ? L"\x2191 " : L"\x2193 ") + o.directions[static_cast<size_t>(i)]
                                    : data_.folder_names[static_cast<size_t>(i)];
            g_ink.Text(f_seg_, t, r, GwAlpha(on ? theme.accent : theme.text_secondary, alpha));
        }
    }
}

} // namespace pulse::ui
