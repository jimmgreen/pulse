// ai_panel_conversation.cpp — the Pulse 助手 conversation: user bubbles,
// thinking, tool steps, plan / confirm / result cards, the streamed answer.
//
// Sizes, margins and motion follow the design's "messages" … "result card"
// styles; CSS pixels are DIPs. Blocks are laid
// out top to bottom with CSS margin collapsing, so spacing matches the page.
#include "ai_panel_view.h"
#include "ai_panel_draw.h"
#include "ai_panel_rich.h"

#include "ui_motion.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
using namespace ai_draw;
namespace {

constexpr float kPi = 3.14159265358979f;

// CSS cubic-bezier(x1, y1, x2, y2) at t.
float CubicBezier(float x1, float y1, float x2, float y2, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float u = t;
    for (int i = 0; i < 8; ++i) {
        const float x = 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u - t;
        const float dx = 3 * (1 - u) * (1 - u) * x1 + 6 * (1 - u) * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (std::fabs(dx) < 1e-5f) break;
        u = std::clamp(u - x / dx, 0.0f, 1.0f);
    }
    return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u;
}

// Negative and positive vertical margins collapse the CSS way.
float Collapse(float a, float b) {
    return (std::max)({a, b, 0.0f}) + (std::min)({a, b, 0.0f});
}

ID2D1Factory* FactoryOf(ID2D1DeviceContext* dc) {
    ID2D1Factory* f = nullptr;
    dc->GetFactory(&f);
    return f;
}

// A rounded rect with its own radius per corner (.user .bub: 16 16 4 16).
void FillCorners(const Ctx& c, const D2D1_RECT_F& r, float tl, float tr, float br, float bl, D2D1_COLOR_F color) {
    ID2D1Factory* f = FactoryOf(c.dc);
    if (!f) return;
    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(f->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* k = nullptr;
        if (SUCCEEDED(g->Open(&k))) {
            const auto arc = [&](float x, float y, float rad) {
                k->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(rad, rad), 0.0f,
                                           D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            };
            k->BeginFigure(D2D1::Point2F(r.left + tl, r.top), D2D1_FIGURE_BEGIN_FILLED);
            k->AddLine(D2D1::Point2F(r.right - tr, r.top));
            if (tr > 0.0f) arc(r.right, r.top + tr, tr);
            k->AddLine(D2D1::Point2F(r.right, r.bottom - br));
            if (br > 0.0f) arc(r.right - br, r.bottom, br);
            k->AddLine(D2D1::Point2F(r.left + bl, r.bottom));
            if (bl > 0.0f) arc(r.left, r.bottom - bl, bl);
            k->AddLine(D2D1::Point2F(r.left, r.top + tl));
            if (tl > 0.0f) arc(r.left + tl, r.top, tl);
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
            k->Close();
            k->Release();
            c.dc->FillGeometry(g, c.B(color));
        }
        g->Release();
    }
    f->Release();
}

void Arc(const Ctx& c, D2D1_POINT_2F mid, float r, float a0, float a1, D2D1_COLOR_F color, float width) {
    ID2D1Factory* f = FactoryOf(c.dc);
    if (!f) return;
    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(f->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* k = nullptr;
        if (SUCCEEDED(g->Open(&k))) {
            k->BeginFigure(D2D1::Point2F(mid.x + r * std::cos(a0), mid.y + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
            k->AddArc(D2D1::ArcSegment(D2D1::Point2F(mid.x + r * std::cos(a1), mid.y + r * std::sin(a1)),
                                       D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            k->EndFigure(D2D1_FIGURE_END_OPEN);
            k->Close();
            k->Release();
            c.dc->DrawGeometry(g, c.B(color), width);
        }
        g->Release();
    }
    f->Release();
}

// .spin: a 2 px ring in accent-soft with an accent quarter, .8 s a turn.
void Spinner(const Ctx& c, D2D1_POINT_2F mid, float d, D2D1_COLOR_F track, D2D1_COLOR_F accent, uint64_t now) {
    const float r = d * 0.5f - c.s;
    c.dc->DrawEllipse(D2D1::Ellipse(mid, r, r), c.B(track), 2.0f * c.s);
    const float a = static_cast<float>(now % 800) / 800.0f * 2.0f * kPi - kPi * 0.5f;
    Arc(c, mid, r, a - kPi * 0.25f, a + kPi * 0.25f, accent, 2.0f * c.s);
}

// @keyframes spark, 1.6 s, cubic-bezier(.5,0,.5,1) per half.
AiIconPose SparkPose(uint64_t now) {
    const float t = static_cast<float>(now % 1600) / 1600.0f;
    const float e = CubicBezier(0.5f, 0.0f, 0.5f, 1.0f, t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f);
    AiIconPose p;
    if (t < 0.5f) {
        p.rotate_deg = 90.0f * e;
        p.scale = 1.0f - 0.22f * e;
    } else {
        p.rotate_deg = 90.0f + 90.0f * e;
        p.scale = 0.78f + 0.22f * e;
    }
    return p;
}

ID2D1LinearGradientBrush* Gradient(ID2D1DeviceContext* dc, D2D1_POINT_2F a, D2D1_POINT_2F b,
                                   const D2D1_GRADIENT_STOP* stops, UINT32 n) {
    ID2D1GradientStopCollection* col = nullptr;
    if (FAILED(dc->CreateGradientStopCollection(stops, n, &col))) return nullptr;
    ID2D1LinearGradientBrush* brush = nullptr;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), col, &brush);
    col->Release();
    return brush;
}

D2D1_RECT_F Intersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return D2D1::RectF((std::max)(a.left, b.left), (std::max)(a.top, b.top), (std::min)(a.right, b.right),
                       (std::min)(a.bottom, b.bottom));
}

// Keys for the second batch's many tweens: apart from id * 8 + slot.
uint64_t XKey(uint32_t id, int slot) {
    return (static_cast<uint64_t>(id) << 32) | (0x10000u + static_cast<uint32_t>(slot));
}

std::wstring Fixed1(float v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f", static_cast<double>(v));
    return buf;
}

// "221.0" -> "221" (the disk card's used figure).
std::wstring Trim0(std::wstring v) {
    if (v.size() > 2 && v.compare(v.size() - 2, 2, L".0") == 0) v.resize(v.size() - 2);
    return v;
}

// 6842 -> "6,842" (the funnel's counts).
std::wstring Thousands(uint64_t v) {
    std::wstring d = std::to_wstring(v), out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out.push_back(L',');
        out.push_back(d[i]);
    }
    return out;
}

// Clips what follows to a rounded rect (overflow: hidden); pop with PopLayer.
bool PushRounded(const Ctx& c, const D2D1_RECT_F& r, float radius) {
    ID2D1Factory* f = FactoryOf(c.dc);
    if (!f) return false;
    ID2D1RoundedRectangleGeometry* g = nullptr;
    const bool ok = SUCCEEDED(f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &g)) && g;
    f->Release();
    if (!ok) return false;
    c.dc->PushLayer(D2D1::LayerParameters1(r, g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f,
                                           nullptr, D2D1_LAYER_OPTIONS1_NONE),
                    nullptr);
    g->Release();
    return true;
}

// repeating-linear-gradient(135deg, color 0 band, color 35% band 2*band):
// the 回收站中 part of a disk bar.
void Hatch(const Ctx& c, const D2D1_RECT_F& r, D2D1_COLOR_F color, float band) {
    c.dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    c.Fill(r, 0.0f, Alpha(color, 0.35f));
    const float period = band * 2.0f * 1.41421356f;
    const float h = r.bottom - r.top;
    for (float x = r.left - h - period; x < r.right + period; x += period)
        c.dc->DrawLine(D2D1::Point2F(x, r.bottom), D2D1::Point2F(x + h, r.top), c.B(color), band);
    c.dc->PopAxisAlignedClip();
}

} // namespace

float AiPanelView::TweenTo(uint64_t key, float target, uint32_t ms, uint64_t now_ms) {
    for (auto& [k, t] : tweens_) {
        if (k != key) continue;
        t.used = true;
        const float p = t.ms ? std::clamp(static_cast<float>(now_ms - t.start) / static_cast<float>(t.ms), 0.0f, 1.0f)
                             : 1.0f;
        const float value = t.from + (t.to - t.from) * EaseDesign(p);
        if (target != t.to) {
            t.from = value;
            t.to = target;
            t.start = now_ms;
            t.ms = motion::SystemAnimationsEnabled() ? ms : 0;
            if (t.ms) conv_anim_ = true;
            return value;
        }
        if (p < 1.0f) conv_anim_ = true;
        return value;
    }
    // First sight: start settled.
    Tween t;
    t.from = t.to = target;
    t.start = now_ms;
    t.used = true;
    tweens_.push_back({key, t});
    return target;
}

void AiPanelView::ScrollBy(float dy, bool animate) {
    const float max_scroll = (std::max)(0.0f, content_h_ - view_h_);
    const float base = scroll_anim_ ? scroll_to_ : scroll_;
    const float to = std::clamp(base + dy, 0.0f, max_scroll);
    scroll_from_ = scroll_;
    scroll_to_ = to;
    scroll_start_ = motion::NowMs();
    scroll_anim_ = animate && motion::SystemAnimationsEnabled();
    if (!scroll_anim_) scroll_ = to;
    follow_ = to >= max_scroll - 1.0f;
}

void AiPanelView::ScrollToEnd() {
    follow_ = true;
    scroll_anim_ = false;
}

void AiPanelView::DrawConversation(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                                   const D2D1_RECT_F& area, float s, uint64_t now_ms) {
    Ctx c{compositor, compositor->Dc(), brush_, s};
    ID2D1DeviceContext* dc = c.dc;
    const bool motion_on = motion::SystemAnimationsEnabled();
    was_empty_ = false;
    for (auto& kv : tweens_) kv.second.used = false;
    EnsureRichBrushes(dc, pal, vm.dark);
    BeginRichFrame(vm);

    // Scroll position for this frame (content height is last frame's; a
    // following view snaps to the end below once this frame is measured).
    view_h_ = area.bottom - area.top;
    if (scroll_anim_) {
        const float p = std::clamp(static_cast<float>(now_ms - scroll_start_) / 160.0f, 0.0f, 1.0f);
        scroll_ = scroll_from_ + (scroll_to_ - scroll_from_) * EaseDesign(p);
        if (p >= 1.0f) scroll_anim_ = false;
        else conv_anim_ = true;
    }
    if (follow_) scroll_ = (std::max)(0.0f, content_h_ - view_h_);

    // .conv padding 16 18 10.
    const float x0 = area.left + 18.0f * s;
    const float x1 = area.right - 18.0f * s;
    const float top = area.top - scroll_;   // content origin on screen

    const auto hot = [&](AiPanelPart part, int i) { return vm.hover.part == part && vm.hover.index == i; };
    const auto down = [&](AiPanelPart part, int i) { return hot(part, i) && vm.pressed.part == part; };
    const auto hit = [&](const D2D1_RECT_F& r, AiPanelPart part, int i) {
        const D2D1_RECT_F v = Intersect(r, area);
        if (v.right > v.left && v.bottom > v.top) conv_hits_.push_back({v, {part, i}});
    };
    const auto visible = [&](float y0, float y1) { return y1 > area.top && y0 < area.bottom; };

    // Text helpers. Plain single line in a box (vertically centred).
    const auto line = [&](const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float right, float h,
                          D2D1_COLOR_F color) {
        if (right > x) c.Text(text, fmt, D2D1::RectF(x, y, right, y + h), color);
    };
    const auto wrapped_h = [&](const std::wstring& text, IDWriteTextFormat* fmt, float width, float lh) {
        return text.empty() ? 0.0f : c.MeasureWrapped(text, fmt, width, lh);
    };
    const auto wrapped = [&](const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float width, float lh,
                             D2D1_COLOR_F color) {
        TextOptions o;
        o.wrap = true;
        o.line_height = lh;
        c.Text(text, fmt, D2D1::RectF(x, y, x + width, y + 1.0f), color, o);
    };
    // .shimmer: text2 with an accent band sweeping across every 1.8 s.
    const auto shimmer = [&](const std::wstring& text, IDWriteTextFormat* fmt, float x, float y, float h) {
        const float tw = c.Measure(text, fmt);
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + tw + 2.0f, y + h);
        c.Text(text, fmt, r, pal.text2);
        if (!motion_on || tw <= 0.0f) return;
        conv_decor_ = true;
        const float t = static_cast<float>(now_ms % 1800) / 1800.0f;
        const float gpos = 1.0f - 2.2f * t;        // background-position 100% → -120%
        const float gx = x - 1.2f * tw * gpos;
        const D2D1_COLOR_F a0 = Alpha(pal.accent, 0.0f), a1 = pal.accent;
        const D2D1_GRADIENT_STOP stops[] = {{0.35f, a0}, {0.5f, a1}, {0.65f, a0}};
        ID2D1LinearGradientBrush* mask =
            Gradient(dc, D2D1::Point2F(gx, 0.0f), D2D1::Point2F(gx + 2.2f * tw, 0.0f), stops, 3);
        if (!mask) return;
        dc->PushLayer(D2D1::LayerParameters1(r, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                             1.0f, mask, D2D1_LAYER_OPTIONS1_NONE),
                      nullptr);
        c.Text(text, fmt, r, pal.accent);
        dc->PopLayer();
        mask->Release();
    };
    // msg-in: opacity 0 → 1 and translateY(8) → 0, .35 s.
    struct Entrance {
        float opacity = 1.0f;
        float dy = 0.0f;
    };
    const auto entrance = [&](uint64_t born, float dist, uint32_t ms) {
        Entrance e;
        if (!born || !motion_on) return e;
        if (now_ms < born) {
            e.opacity = 0.0f;
            e.dy = dist;
            conv_anim_ = true;
            return e;
        }
        const float t = static_cast<float>(now_ms - born) / static_cast<float>(ms);
        if (t >= 1.0f) return e;
        conv_anim_ = true;
        const float k = CubicBezier(0.3f, 0.7f, 0.2f, 1.0f, t);
        e.opacity = k;
        e.dy = dist * (1.0f - k);
        return e;
    };
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    const auto push_entrance = [&](const Entrance& e) {
        if (e.opacity >= 1.0f && e.dy == 0.0f) return false;
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, e.dy) * saved);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), e.opacity, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                      nullptr);
        return true;
    };
    const auto pop_entrance = [&](bool pushed) {
        if (!pushed) return;
        dc->PopLayer();
        dc->SetTransform(saved);
    };
    // .b2 button (h 32, padding 0 14, radius 8); returns its width.
    // style: 0 normal, 1 red primary (.confirm.danger), 2 disabled primary,
    // 3 red text on a plain button (.b2.dz).
    const auto button = [&](float x_right, float y, const std::wstring& text, bool primary, AiIcon icon, bool with_icon,
                            AiPanelPart part, int index, float max_w = 0.0f, int style = 0,
                            const std::wstring& kbd = {}) {
        IDWriteTextFormat* fmt = primary ? formats_.body13b : formats_.body13;
        const float tw = c.Measure(text, fmt);
        // kbd: the key after the words in a lighter ink (取消 Esc).
        const float kw = kbd.empty() ? 0.0f : c.Measure(kbd, formats_.caption) + 7.0f * s;
        float bw = 28.0f * s + tw + kw + (with_icon ? 20.0f * s : 0.0f);
        if (max_w > 0.0f && bw > max_w) bw = max_w;   // a narrow panel: the text ends in …
        const D2D1_RECT_F r = D2D1::RectF(x_right - bw, y, x_right, y + 32.0f * s);
        const bool off = style == 2;
        const bool h = !off && hot(part, index), d = !off && down(part, index);
        D2D1_MATRIX_3X2_F before;
        dc->GetTransform(&before);
        if (d && motion_on) {
            const D2D1_POINT_2F mid = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * before);
        }
        const D2D1_COLOR_F fill = style == 1 ? pal.red : pal.accent;
        if (off) {
            c.Fill(r, 8.0f * s, pal.hover);
        } else if (primary) {
            c.Fill(r, 8.0f * s, h ? Mix(fill, pal.text, 0.12f) : fill);
        } else {
            c.Fill(r, 8.0f * s, h ? pal.hover : Mix(pal.card, pal.pane, 0.3f));
            c.Stroke(r, 8.0f * s, pal.line);
        }
        const D2D1_COLOR_F ink = off ? pal.text3 : style == 1 ? Hex(0xffffff) : style == 3 ? pal.red
                                 : primary ? pal.on_accent : pal.text;
        float tx = r.left + 14.0f * s;
        if (with_icon) {
            c.Icon(icon, tx, y + 9.0f * s, 14.0f * s, ink);
            tx += 20.0f * s;
        }
        line(text, fmt, tx, y, r.right - 10.0f * s - kw, 32.0f * s, ink);
        if (kw > 0.0f)
            line(kbd, formats_.caption, r.right - 14.0f * s - kw + 7.0f * s, y, r.right - 10.0f * s, 32.0f * s,
                 Alpha(ink, primary ? 0.75f : 0.6f));
        dc->SetTransform(before);
        if (!off) hit(r, part, index);
        return bw;
    };
    const auto chevron = [&](float x, float cy, float deg, D2D1_COLOR_F color, AiIcon icon) {
        AiIconPose pose;
        pose.rotate_deg = deg;
        c.Icon(icon, x, cy - 6.5f * s, 13.0f * s, color, pose);
    };
    const auto card = [&](const D2D1_RECT_F& r, D2D1_COLOR_F border) {
        c.Fill(r, 12.0f * s, pal.card);
        c.Stroke(r, 12.0f * s, border);
    };
    // .okc: 18 px ok circle with a white check, pops in .35 s
    // (cubic-bezier(.3,1.6,.5,1)).
    const auto ok_check = [&](D2D1_POINT_2F mid, uint64_t done_ms) {
        float k = 1.0f, a = 1.0f;
        if (done_ms && motion_on && now_ms - done_ms < 350) {
            const float t = static_cast<float>(now_ms - done_ms) / 350.0f;
            const float e = CubicBezier(0.3f, 1.6f, 0.5f, 1.0f, t);
            k = 0.3f + 0.7f * e;
            a = std::clamp(e, 0.0f, 1.0f);
            conv_anim_ = true;
        }
        const float r = 9.0f * s * k;
        c.dc->FillEllipse(D2D1::Ellipse(mid, r, r), c.B(Alpha(pal.ok, a)));
        const float is = 12.0f * s * k;
        c.Icon(AiIcon::Check, mid.x - is * 0.5f, mid.y - is * 0.5f, is, Alpha(pal.on_accent, a));
    };

    // Second batch: a value easing in over `ms` from `start` (0: settled),
    // and the .bump pulse (scale 1 → 1.08 → 1, .45 s).
    const auto Count = [&](uint64_t start, uint32_t ms) {
        if (!start || !motion_on) return 1.0f;
        if (now_ms < start) { conv_anim_ = true; return 0.0f; }
        const float t = static_cast<float>(now_ms - start) / static_cast<float>(ms);
        if (t >= 1.0f) return 1.0f;
        conv_anim_ = true;
        return CubicBezier(0.4f, 0.0f, 0.2f, 1.0f, t);
    };
    // .swap / .flash: an inset 2 px accent ring, in by 25 %, out after 70 % of 1.2 s.
    const auto Flash = [&](uint64_t start) {
        if (!start || !motion_on || now_ms < start || now_ms - start >= 1200) return 0.0f;
        conv_anim_ = true;
        const float t = static_cast<float>(now_ms - start) / 1200.0f;
        return t < 0.25f ? t / 0.25f : t < 0.7f ? 1.0f : (1.0f - t) / 0.3f;
    };
    const auto Bump = [&](uint64_t start) {
        if (!start || !motion_on || now_ms < start || now_ms - start >= 450) return 0.0f;
        conv_anim_ = true;
        const float t = static_cast<float>(now_ms - start) / 450.0f;
        return t < 0.4f ? t / 0.4f : (1.0f - t) / 0.6f;
    };
    // .card-h 40: icon 16 accent, title 13/600, .mu (12 text3, an optional
    // 12 px accent icon, gap 5), chevron; hover fills the header.
    const auto card_head = [&](const D2D1_RECT_F& r, const D2D1_RECT_F& hr, AiIcon icon, const std::wstring& label,
                               const std::wstring& meta, AiIcon meta_icon, bool collapsed, AiPanelPart part, int index,
                               uint64_t key) {
        if (hot(part, index)) {
            dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
            c.Fill(Inflate(r, -1.0f), 11.0f * s, Alpha(pal.hover, 0.6f));
            dc->PopAxisAlignedClip();
        }
        const float cy = hr.top + 20.0f * s;
        c.Icon(icon, hr.left + 12.0f * s, cy - 8.0f * s, 16.0f * s, pal.accent);
        float hx = hr.left + 36.0f * s;
        const float tw = c.Measure(label, formats_.body13b);
        line(label, formats_.body13b, hx, hr.top, hx + tw + 1.0f, 40.0f * s, pal.text);
        hx += tw + 8.0f * s;
        if (meta_icon != AiIcon::Count && !meta.empty()) {
            c.Icon(meta_icon, hx, cy - 6.0f * s, 12.0f * s, pal.accent);
            hx += 17.0f * s;
        }
        line(meta, formats_.caption, hx, hr.top, hr.right - 33.0f * s, 40.0f * s, pal.text3);
        const float col_k = TweenTo(key + 1, collapsed ? 1.0f : 0.0f, 250, now_ms);
        chevron(hr.right - 25.0f * s, cy, -90.0f * col_k, pal.text3, AiIcon::ChevronDown);
    };
    // .ck: r 4, 1.5 px text3 border; on: accent with a white check; pressed
    // scale .85.
    const auto check_box = [&](const D2D1_RECT_F& r, bool on, bool hover, bool pressed, float check) {
        D2D1_MATRIX_3X2_F before;
        dc->GetTransform(&before);
        if (pressed && motion_on) {
            const D2D1_POINT_2F mid = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(0.85f, 0.85f, mid) * before);
        }
        if (on) {
            c.Fill(r, 4.0f * s, hover ? Mix(pal.accent, pal.text, 0.12f) : pal.accent);
            c.Icon(AiIcon::Check, (r.left + r.right - check) * 0.5f, (r.top + r.bottom - check) * 0.5f, check, pal.on_accent);
        } else {
            c.Stroke(Inflate(r, -0.75f * s), 3.25f * s, hover ? pal.text2 : pal.text3, 1.5f * s);
        }
        dc->SetTransform(before);
    };

    float y = 16.0f * s;     // content y of the next block's top edge (before margins)
    float pending = 0.0f;    // the previous block's bottom margin
    bool first = true;
    const auto place = [&](float margin_top) {
        y += first ? (std::max)(0.0f, margin_top) : Collapse(pending, margin_top);
        first = false;
    };

    for (size_t bi = 0; bi < vm.blocks.size(); ++bi) {
        const AiBlockView& b = vm.blocks[bi];
        const int index = static_cast<int>(bi);
        const uint64_t key = static_cast<uint64_t>(b.id) * 8u;
        Entrance enter = entrance(b.born_ms, 8.0f * s, 350);
        if (b.kind == AiBlockKind::Acts) {
            // .acts fades in (opacity .3s), no slide.
            enter = entrance(b.born_ms, 0.0f, 300);
        }
        // Editing a message: what comes after it is answered again (.42).
        if (vm.edit_block >= 0 && index > vm.edit_block) enter.opacity *= 0.42f;
        if (b.gone_ms && motion_on) {
            // .err.gone: opacity 0, translateY(-4px) over .25 s (then removed).
            const float t = now_ms <= b.gone_ms ? 0.0f
                                                : std::clamp(static_cast<float>(now_ms - b.gone_ms) / 250.0f, 0.0f, 1.0f);
            enter.opacity *= 1.0f - t;
            enter.dy -= 4.0f * s * t;
            conv_anim_ = true;
        }
        const bool pushed = push_entrance(enter);
        switch (b.kind) {
        case AiBlockKind::User: {
            // A new turn: .turn margin-bottom 12 collapses with the user's 4.
            if (!first) pending = (std::max)(pending, 12.0f * s);
            place(4.0f * s);
            const float turn_top = top + y;
            if (index == vm.edit_block) {
                // .editbox: full width, card, 1 px accent border and a 3 px
                // accent ring (16%), radius 14, padding 10 12 8; text 13.5 / 21
                // (min 2 lines); the row: info 14 + note 12 text3, 取消 · 发送.
                const float elh = 21.0f * s;
                const float ta = static_cast<float>((std::max)(2, vm.edit_lines)) * elh;
                // A narrow panel: the note takes its own line above the buttons
                // rather than ending in … beside them.
                const float btns_w = (28.0f * 2.0f + 7.0f * 2.0f + 8.0f) * s + c.Measure(vm.edit_send, formats_.body13b) +
                                     c.Measure(L"Enter", formats_.caption) + c.Measure(vm.edit_cancel, formats_.body13) +
                                     c.Measure(L"Esc", formats_.caption);
                const float note_w = 20.0f * s + c.Measure(vm.edit_warn, formats_.caption);
                const bool note_above = note_w + 8.0f * s + btns_w > (x1 - x0) - 24.0f * s;
                const float note_h = note_above ? 20.0f * s : 0.0f;
                const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + 10.0f * s + ta + 8.0f * s + note_h + 32.0f * s + 8.0f * s);
                if (visible(r.top - 3.0f * s, r.bottom + 3.0f * s)) {
                    c.Stroke(Inflate(r, 3.0f * s), 17.0f * s, Alpha(pal.accent, 0.16f), 3.0f * s);
                    c.Fill(r, 14.0f * s, pal.card);
                    c.Stroke(r, 14.0f * s, pal.accent, 1.0f * s);
                }
                turn_edit_rect_ = D2D1::RectF(r.left + 12.0f * s, r.top + 10.0f * s, r.right - 12.0f * s, r.top + 10.0f * s + ta);
                turn_edit_placed_ = turn_edit_rect_.top >= area.top && turn_edit_rect_.bottom <= area.bottom;
                if (!turn_edit_placed_ && visible(turn_edit_rect_.top, turn_edit_rect_.bottom)) {
                    // Scrolled partly out: the panel draws the words, a click brings the EDIT.
                    float ly = turn_edit_rect_.top;
                    size_t from = 0;
                    while (from <= vm.edit_text.size() && ly < turn_edit_rect_.bottom) {
                        const size_t nl = vm.edit_text.find(L'\n', from);
                        const std::wstring seg = vm.edit_text.substr(from, nl == std::wstring::npos ? std::wstring::npos : nl - from);
                        line(seg, formats_.body, turn_edit_rect_.left, ly, turn_edit_rect_.right, elh, pal.text);
                        ly += elh;
                        if (nl == std::wstring::npos) break;
                        from = nl + 1;
                    }
                    hit(turn_edit_rect_, AiPanelPart::EditBox, index);
                }
                const float nx = r.left + 12.0f * s;
                if (note_above && visible(turn_edit_rect_.bottom + 6.0f * s, turn_edit_rect_.bottom + 26.0f * s)) {
                    const float ny = turn_edit_rect_.bottom + 6.0f * s;
                    c.Icon(AiIcon::Info, nx, ny + 3.0f * s, 14.0f * s, pal.text3);
                    line(vm.edit_warn, formats_.caption, nx + 20.0f * s, ny, r.right - 12.0f * s, 20.0f * s, pal.text3);
                }
                const float ry = turn_edit_rect_.bottom + 8.0f * s + note_h;
                if (visible(ry, ry + 32.0f * s)) {
                    float bx = r.right - 12.0f * s;
                    bx -= button(bx, ry, vm.edit_send, true, AiIcon::Send, false, AiPanelPart::EditSend, index, 0.0f, 0, L"Enter");
                    bx -= 8.0f * s;
                    bx -= button(bx, ry, vm.edit_cancel, false, AiIcon::Close, false, AiPanelPart::EditCancel, index, 0.0f, 0, L"Esc");
                    if (!note_above && bx - 8.0f * s - nx > 40.0f * s) {
                        c.Icon(AiIcon::Info, nx, ry + 9.0f * s, 14.0f * s, pal.text3);
                        line(vm.edit_warn, formats_.caption, nx + 20.0f * s, ry, bx - 8.0f * s, 32.0f * s, pal.text3);
                    }
                }
                y += r.bottom - r.top;
                pending = 8.0f * s;
                break;
            }
            if (!b.context.empty()) {
                // .ctxp: h 24, padding 0 9 0 7, radius 12, folder 13, gap 6.
                const float tw = c.Measure(b.context, formats_.caption);
                const float cw = (7.0f + 13.0f + 6.0f + 9.0f) * s + tw;
                const D2D1_RECT_F r = D2D1::RectF(x1 - cw, top + y, x1, top + y + 24.0f * s);
                if (visible(r.top, r.bottom)) {
                    c.Fill(r, 12.0f * s, pal.card);
                    c.Stroke(r, 12.0f * s, pal.line);
                    c.Icon(AiIcon::FolderFilled, r.left + 7.0f * s, r.top + 5.5f * s, 13.0f * s, pal.folder_icon);
                    line(b.context, formats_.caption, r.left + 26.0f * s, r.top, r.right, 24.0f * s, pal.text2);
                }
                y += 30.0f * s;
            }
            // .bub: max-width 300, padding 10 14, radius 16 16 4 16, 13.5 / 21.
            // Its text is selectable like an answer's (laid out once).
            const float max_text = (std::min)(300.0f * s, x1 - x0) - 28.0f * s;
            const AiRichDoc* rd = RichFor(compositor, b, max_text, s);
            const float tw = rd ? rd->text_w : 0.0f;
            const float th = rd ? (std::max)(21.0f * s, rd->height) : 21.0f * s;
            const float bw = tw + 28.0f * s;
            const D2D1_RECT_F r = D2D1::RectF(x1 - bw, top + y, x1, top + y + th + 20.0f * s);
            const bool shown = visible(r.top, r.bottom);
            if (shown) FillCorners(c, r, 16.0f * s, 16.0f * s, 4.0f * s, 16.0f * s, pal.accent_soft);
            if (rd) DrawRich(compositor, *rd, b, static_cast<int>(bi), r.left + 14.0f * s, r.top + 10.0f * s, pal, area, vm, s, now_ms, shown);
            y += th + 20.0f * s;
            if (b.edited) {
                // .edited: 11.5 text3, margin 3 4 0, under the bubble's right end.
                y += 3.0f * s;
                const float ew = c.Measure(vm.edited_label, formats_.meta);
                if (visible(top + y, top + y + 16.0f * s))
                    line(vm.edited_label, formats_.meta, x1 - 4.0f * s - ew - 1.0f, top + y, x1 - 4.0f * s + 1.0f, 16.0f * s, pal.text3);
                y += 16.0f * s;
            }
            // .uacts: margin-top 4, 复制 · 编辑 (28, gap 2) at the right; they
            // fade in (.15 s) while the pointer is over the turn.
            y += 4.0f * s;
            const bool acts_on = vm.hover_turn == index || hot(AiPanelPart::UserCopy, index) || hot(AiPanelPart::UserEdit, index);
            const float acts_k = TweenTo(key + 5, acts_on ? 1.0f : 0.0f, 150, now_ms);
            const float ay = top + y;
            if (acts_k > 0.01f && visible(ay, ay + 28.0f * s)) {
                const struct { AiPanelPart part; AiIcon icon; } items[] = {
                    {AiPanelPart::UserCopy, AiIcon::Copy},
                    {AiPanelPart::UserEdit, AiIcon::Edit},
                };
                float bx = x1 - (28.0f * 2.0f + 2.0f) * s;
                for (const auto& it : items) {
                    const D2D1_RECT_F br = D2D1::RectF(bx, ay, bx + 28.0f * s, ay + 28.0f * s);
                    const bool h = hot(it.part, index);
                    if (h) c.Fill(br, 7.0f * s, Alpha(down(it.part, index) ? Mix(pal.hover, pal.text, 0.06f) : pal.hover, acts_k));
                    c.Icon(it.icon, bx + 6.5f * s, ay + 6.5f * s, 15.0f * s, Alpha(h ? pal.text : pal.text3, acts_k));
                    if (acts_on) hit(br, it.part, index);
                    bx += 30.0f * s;
                }
            }
            y += 28.0f * s;
            // The turn shows its buttons while the pointer is anywhere on it.
            {
                const D2D1_RECT_F turn_vis = D2D1::RectF(x0, (std::max)(turn_top, area.top), x1, (std::min)(top + y, area.bottom));
                if (turn_vis.bottom > turn_vis.top) turn_rects_.push_back({turn_vis, index});
            }
            pending = 8.0f * s;
            break;
        }
        case AiBlockKind::Thinking: {
            place(0.0f);
            // .think-h: h 28, padding 0 10 0 8, margin-left -8, gap 7.
            const float hx = x0 - 8.0f * s;
            const float hy = top + y;
            const float label_w = c.Measure(b.label, formats_.body13);
            const float meta_w = b.meta.empty() ? 0.0f : c.Measure(b.meta, formats_.caption);
            const float sec_w = b.seconds.empty() ? 0.0f : c.Measure(b.seconds, formats_.caption);
            float hw = (8.0f + 15.0f + 7.0f) * s + label_w + 7.0f * s;
            if (meta_w > 0.0f) hw += meta_w + 7.0f * s;
            if (sec_w > 0.0f) hw += sec_w + 7.0f * s;
            hw += (13.0f + 10.0f) * s;
            const D2D1_RECT_F hr = D2D1::RectF(hx, hy, hx + hw, hy + 28.0f * s);
            const float open_k = TweenTo(key + 1, b.open ? 1.0f : 0.0f, 250, now_ms);
            if (visible(hr.top, hr.bottom + 200.0f * s)) {
                if (hot(AiPanelPart::ThinkToggle, index)) c.Fill(hr, 8.0f * s, pal.hover);
                const float cy = hy + 14.0f * s;
                float x = hx + 8.0f * s;
                AiIconPose pose;
                if (b.running && motion_on) { pose = SparkPose(now_ms); conv_decor_ = true; }
                c.Icon(AiIcon::Spark, x, cy - 7.5f * s, 15.0f * s, pal.accent, pose);
                x += 22.0f * s;
                if (b.running) shimmer(b.label, formats_.body13, x, hy, 28.0f * s);
                else line(b.label, formats_.body13, x, hy, x + label_w + 2.0f, 28.0f * s, pal.text2);
                x += label_w + 7.0f * s;
                if (meta_w > 0.0f) {
                    line(b.meta, formats_.caption, x, hy, x + meta_w + 2.0f, 28.0f * s, pal.text3);
                    x += meta_w + 7.0f * s;
                }
                if (sec_w > 0.0f) {
                    line(b.seconds, formats_.caption, x, hy, x + sec_w + 2.0f, 28.0f * s, pal.text3);
                    x += sec_w + 7.0f * s;
                }
                chevron(x, cy, 90.0f * open_k, pal.text3, AiIcon::ChevronRight);
            }
            if (!b.running) hit(hr, AiPanelPart::ThinkToggle, index);
            y += 28.0f * s;
            // .think-b: margin 2 0 0 7, border-left 2, padding-left 12;
            // text 12.5 / 20, padding 2 0 4. Running shows the last 70 px
            // under a 22 px fade; open shows up to 200 px.
            y += 2.0f * s;
            const float bx = x0 + 7.0f * s;
            const float tx = bx + 14.0f * s;
            const float text_w = x1 - tx;
            const float th = b.text.empty() ? 0.0f : wrapped_h(b.text, formats_.small125, text_w, 20.0f * s) + 6.0f * s;
            const float target = b.running ? (std::min)(70.0f * s, th) : b.open ? (std::min)(200.0f * s, th) : 0.0f;
            const float hv = TweenTo(key + 2, target, 450, now_ms);
            const float alpha = TweenTo(key + 3, (b.running || b.open) ? 1.0f : 0.0f, 200, now_ms);
            if (hv > 0.5f && alpha > 0.0f && visible(top + y, top + y + hv)) {
                const D2D1_RECT_F clip = D2D1::RectF(bx, top + y, x1 + 1.0f, top + y + hv);
                dc->PushAxisAlignedClip(Intersect(clip, area), D2D1_ANTIALIAS_MODE_ALIASED);
                c.Fill(D2D1::RectF(bx, clip.top, bx + 2.0f * s, clip.bottom), 0.0f, Alpha(pal.line, alpha));
                float ty = clip.top + 2.0f * s;
                if (b.running && th > hv) ty = clip.bottom - th + 2.0f * s;
                ID2D1LinearGradientBrush* fade = nullptr;
                if (b.running && th > hv) {
                    const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0, 0, 0, 0)}, {1.0f, D2D1::ColorF(0, 0, 0, 1)}};
                    fade = Gradient(dc, D2D1::Point2F(0.0f, clip.top), D2D1::Point2F(0.0f, clip.top + 22.0f * s), stops, 2);
                }
                if (fade || alpha < 1.0f) {
                    dc->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                         D2D1::IdentityMatrix(), alpha, fade, D2D1_LAYER_OPTIONS1_NONE),
                                  nullptr);
                }
                wrapped(b.text, formats_.small125, tx, ty, text_w, 20.0f * s, pal.text3);
                if (fade || alpha < 1.0f) dc->PopLayer();
                if (fade) fade->Release();
                dc->PopAxisAlignedClip();
            }
            y += hv;
            pending = 10.0f * s;
            break;
        }
        case AiBlockKind::Text: {
            // The answer's Markdown (ai_panel_rich.cpp), laid out once per
            // text and width: p 13.5 / 22, lists, code, tables, path chips.
            const AiRichDoc* rd = RichFor(compositor, b, x1 - x0, s);
            if (!rd || (rd->pieces.empty() && rd->decos.empty() && !b.streaming)) break;
            const float lh = 22.0f * s;
            place(rd->pieces.empty() && rd->decos.empty() ? 2.0f * s : rd->margin_top);
            const float by = top + y;
            const float h = rd->height;
            DrawRich(compositor, *rd, b, static_cast<int>(bi), x0, by, pal, area, vm, s, now_ms, visible(by, by + h));
            if (b.streaming) {
                float caret_x = x0, caret_y = by, caret_lh = lh;
                if (!RichCaret(*rd, b.id, x0, by, caret_x, caret_y, caret_lh)) {
                    caret_x = x0;
                    caret_y = by;
                }
                const bool empty = rd->pieces.empty() && rd->decos.empty();
                // .caret: 7 × 15 accent, blinking at 1 s steps(2, start).
                conv_decor_ = true;
                if (!motion_on || now_ms % 1000 < 500) {
                    const float cx = caret_x + 1.0f * s;
                    const float cy = caret_y + (caret_lh - 15.0f * s) * 0.5f + 2.0f * s;
                    c.Fill(D2D1::RectF(cx, cy, cx + 7.0f * s, cy + 15.0f * s), 1.0f * s, pal.accent);
                }
                y += empty ? lh : h;
                pending = empty ? 10.0f * s : rd->margin_bottom;
            } else {
                y += h;
                pending = rd->margin_bottom;
            }
            break;
        }
        case AiBlockKind::Steps: {
            place(0.0f);
            // Heights first: header 38 (+1 border when open), body padding 6,
            // each step padding 8 12 around max(28, text, status).
            std::vector<float> rows;
            float body = 12.0f * s;
            for (const AiStepView& st : b.steps) {
                float mn = 18.0f * s;
                if (!st.sub.empty()) mn += 20.0f * s;
                if (st.progress >= 0.0f && st.state == AiStepState::Running) mn += 10.0f * s;
                const float rh = 16.0f * s + (std::max)({28.0f * s, mn, 23.0f * s});
                rows.push_back(rh);
                body += rh;
            }
            const bool show_body = !b.collapsed || b.open;
            float h = 2.0f * s;
            if (b.collapsed) h += 38.0f * s + (b.open ? 1.0f * s : 0.0f);
            if (show_body) h += body;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const float open_k = TweenTo(key + 1, b.open ? 1.0f : 0.0f, 250, now_ms);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                float yy = r.top + 1.0f * s;
                if (b.collapsed) {
                    const D2D1_RECT_F hr = D2D1::RectF(r.left + 1.0f, yy, r.right - 1.0f, yy + 38.0f * s);
                    if (hot(AiPanelPart::StepsToggle, index)) {
                        dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
                        c.Fill(Inflate(r, -1.0f), 11.0f * s, Alpha(pal.hover, 0.6f));
                        dc->PopAxisAlignedClip();
                    }
                    // Nothing done (没有改动): a muted skip mark.
                    c.Icon(b.partial ? AiIcon::Skip : AiIcon::Check, hr.left + 12.0f * s, yy + 11.5f * s, 15.0f * s,
                           b.partial ? pal.text3 : pal.ok);
                    line(b.label, formats_.body13, hr.left + 35.0f * s, yy, hr.right - 33.0f * s, 38.0f * s, pal.text2);
                    chevron(hr.right - 25.0f * s, yy + 19.0f * s, 180.0f * open_k, pal.text3, AiIcon::ChevronDown);
                    hit(hr, AiPanelPart::StepsToggle, index);
                    yy += 38.0f * s;
                    if (b.open) {
                        c.Fill(D2D1::RectF(r.left + 1.0f, yy, r.right - 1.0f, yy + 1.0f * s), 0.0f, pal.line2);
                        yy += 1.0f * s;
                    }
                }
                if (show_body) {
                    yy += 6.0f * s;
                    const float il = r.left + 1.0f * s;
                    for (size_t i = 0; i < b.steps.size(); ++i) {
                        const AiStepView& st = b.steps[i];
                        const float rh = rows[i];
                        const Entrance se = entrance(st.born_ms, 8.0f * s, 350);
                        const bool sp = push_entrance(se);
                        if (i > 0) {
                            // .step + .step::before: left 25, top -8, h 14, w 2.
                            c.Fill(D2D1::RectF(il + 25.0f * s, yy - 8.0f * s, il + 27.0f * s, yy + 6.0f * s), 0.0f,
                                   pal.line);
                        }
                        const bool run = st.state == AiStepState::Running;
                        const float ix = il + 12.0f * s, iy = yy + 8.0f * s;
                        const D2D1_RECT_F tile = D2D1::RectF(ix, iy, ix + 28.0f * s, iy + 28.0f * s);
                        c.Fill(tile, 8.0f * s, run ? pal.accent_soft : pal.hover);
                        c.Icon(st.icon, ix + 6.5f * s, iy + 6.5f * s, 15.0f * s, run ? pal.accent : pal.text2);
                        const float mx = ix + 38.0f * s;
                        const float mr = r.right - 1.0f * s - 12.0f * s - 18.0f * s - 10.0f * s;
                        float tw = c.Measure(st.title, formats_.body13b);
                        tw = (std::min)(tw, mr - mx);
                        line(st.title, formats_.body13b, mx, iy, mx + tw + 1.0f, 18.0f * s, pal.text);
                        if (!st.arg.empty()) {
                            // .arg: margin-left 6, padding 0 6, radius 5, 12 text2.
                            const float ax = mx + tw + 6.0f * s;
                            const float aw = (std::min)(c.Measure(st.arg, formats_.caption) + 12.0f * s, mr - ax);
                            if (aw > 16.0f * s) {
                                const D2D1_RECT_F ar = D2D1::RectF(ax, iy + 1.0f * s, ax + aw, iy + 17.0f * s);
                                c.Fill(ar, 5.0f * s, pal.hover);
                                line(st.arg, formats_.caption, ax + 6.0f * s, ar.top, ar.right - 4.0f * s, 16.0f * s,
                                     pal.text2);
                            }
                        }
                        float sy = iy + 18.0f * s;
                        if (!st.sub.empty()) {
                            sy += 2.0f * s;
                            line(st.sub, formats_.caption, mx, sy, mr, 18.0f * s, run ? pal.accent : pal.text3);
                            sy += 18.0f * s;
                        }
                        if (st.progress >= 0.0f && run) {
                            // .pg: 4 px, radius 2, margin-top 6.
                            const float py = sy + 6.0f * s;
                            const float pw = mr - mx;
                            const float pv = TweenTo(key + 4, std::clamp(st.progress, 0.0f, 1.0f), 250, now_ms);
                            c.Fill(D2D1::RectF(mx, py, mx + pw, py + 4.0f * s), 2.0f * s, pal.line);
                            if (pv > 0.0f)
                                c.Fill(D2D1::RectF(mx, py, mx + pw * pv, py + 4.0f * s), 2.0f * s, pal.accent);
                        }
                        // .stt: 18 × 18, margin-top 5.
                        const D2D1_POINT_2F smid =
                            D2D1::Point2F(r.right - 1.0f * s - 12.0f * s - 9.0f * s, yy + 8.0f * s + 5.0f * s + 9.0f * s);
                        if (run) {
                            Spinner(c, smid, 16.0f * s, pal.accent_soft, pal.accent, now_ms);
                            conv_decor_ = true;
                        } else if (st.state == AiStepState::Done) {
                            ok_check(smid, st.done_ms);
                        } else {
                            c.dc->FillEllipse(D2D1::Ellipse(smid, 9.0f * s, 9.0f * s), c.B(pal.amber_bg));
                            c.Icon(AiIcon::Close, smid.x - 6.0f * s, smid.y - 6.0f * s, 12.0f * s, pal.amber);
                        }
                        pop_entrance(sp);
                        yy += rh;
                    }
                }
            } else if (b.collapsed) {
                hit(D2D1::RectF(r.left, r.top, r.right, r.top + 39.0f * s), AiPanelPart::StepsToggle, index);
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Plan: {
            place(0.0f);
            // .card-h 40; .card-b border-top + padding 6; .grp padding 7 12
            // around max(26, 18 + 18).
            const float gh = 50.0f * s;
            float h = 2.0f * s + 40.0f * s;
            if (!b.collapsed) h += 1.0f * s + 12.0f * s + gh * static_cast<float>(b.groups.size());
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const float col_k = TweenTo(key + 1, b.collapsed ? 1.0f : 0.0f, 250, now_ms);
            const D2D1_RECT_F hr = D2D1::RectF(r.left + 1.0f, r.top + 1.0f, r.right - 1.0f, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                if (hot(AiPanelPart::PlanToggle, index)) {
                    dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
                    c.Fill(Inflate(r, -1.0f), 11.0f * s, Alpha(pal.hover, 0.6f));
                    dc->PopAxisAlignedClip();
                }
                const float cy = hr.top + 20.0f * s;
                c.Icon(AiIcon::Move, hr.left + 12.0f * s, cy - 8.0f * s, 16.0f * s, pal.accent);
                float x = hr.left + 36.0f * s;
                const float tw = c.Measure(b.label, formats_.body13b);
                line(b.label, formats_.body13b, x, hr.top, x + tw + 1.0f, 40.0f * s, pal.text);
                x += tw + 8.0f * s;
                line(b.meta, formats_.caption, x, hr.top, hr.right - 33.0f * s, 40.0f * s, pal.text3);
                chevron(hr.right - 25.0f * s, cy, -90.0f * col_k, pal.text3, AiIcon::ChevronDown);
                if (!b.collapsed) {
                    float yy = hr.bottom;
                    c.Fill(D2D1::RectF(r.left + 1.0f, yy, r.right - 1.0f, yy + 1.0f * s), 0.0f, pal.line2);
                    yy += 1.0f * s + 6.0f * s;
                    for (const AiGroupView& g : b.groups) {
                        // .grp: opacity 0 + translateY(4) until .in (.3 s).
                        float ga = 0.0f, gdy = 4.0f * s;
                        if (g.in_ms) {
                            const float t = motion_on ? std::clamp(static_cast<float>(now_ms - g.in_ms) / 300.0f, 0.0f, 1.0f)
                                                      : 1.0f;
                            if (t < 1.0f) conv_anim_ = true;
                            const float e = CubicBezier(0.25f, 0.1f, 0.25f, 1.0f, t);
                            ga = e;
                            gdy = 4.0f * s * (1.0f - e);
                        }
                        if (ga > 0.0f) {
                            const bool gp = push_entrance({ga, gdy});
                            const float gx = r.left + 1.0f * s + 12.0f * s;
                            const float gy = yy + 7.0f * s;
                            const int ti = static_cast<int>(g.tint);
                            c.Fill(D2D1::RectF(gx, gy, gx + 26.0f * s, gy + 26.0f * s), 7.0f * s, pal.tint_bg[ti]);
                            c.Icon(g.icon, gx + 5.0f * s, gy + 5.0f * s, 16.0f * s, pal.tint_fg[ti]);
                            const float mx = gx + 36.0f * s;
                            const float mr = r.right - 1.0f * s - 12.0f * s;
                            float nx = mx;
                            const float nw = c.Measure(g.name, formats_.body13b);
                            line(g.name, formats_.body13b, nx, gy, nx + nw + 1.0f, 18.0f * s, pal.text);
                            nx += nw + 6.0f * s;
                            const float cw = c.Measure(g.count, formats_.caption);
                            line(g.count, formats_.caption, nx, gy, nx + cw + 1.0f, 18.0f * s, pal.text3);
                            nx += cw;
                            if (!g.badge.empty()) {
                                // .newp: 10.5 semibold, padding 0 6, radius 8, lh 17.
                                nx += 6.0f * s;
                                const float bw = c.Measure(g.badge, formats_.badge) + 12.0f * s;
                                const D2D1_RECT_F br = D2D1::RectF(nx, gy + 0.5f * s, nx + bw, gy + 17.5f * s);
                                c.Fill(br, 8.0f * s, pal.accent_soft);
                                line(g.badge, formats_.badge, br.left + 6.0f * s, br.top, br.right, 17.0f * s, pal.accent);
                            }
                            line(g.list, formats_.caption, mx, gy + 18.0f * s, mr, 18.0f * s, pal.text2);
                            pop_entrance(gp);
                        }
                        yy += gh;
                    }
                }
            }
            hit(hr, AiPanelPart::PlanToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Confirm: {
            place(0.0f);
            const float il = x0 + 1.0f * s;
            const float ir = x1 - 1.0f * s;
            if (b.done) {
                // .confirm.done: padding 9 12, icon 24 accent-soft / ok,
                // title 13 text2 lh 24.
                const float h = 2.0f * s + 42.0f * s;
                const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
                if (visible(r.top, r.bottom)) {
                    card(r, pal.line);
                    const float iy = r.top + 1.0f * s + 9.0f * s;
                    // Declined or stopped: a muted ✕ - nothing was done, so no ✓.
                    c.Fill(D2D1::RectF(il + 12.0f * s, iy, il + 36.0f * s, iy + 24.0f * s), 8.0f * s,
                           b.declined ? pal.hover : pal.accent_soft);
                    c.Icon(b.declined ? AiIcon::Close : b.danger ? AiIcon::Check : AiIcon::Shield, il + 17.0f * s,
                           iy + 5.0f * s, 14.0f * s, b.declined ? pal.text3 : pal.ok);
                    line(b.title, formats_.body13, il + 46.0f * s, iy, ir - 12.0f * s, 24.0f * s, pal.text2);
                }
                y += h;
                pending = 12.0f * s;
                break;
            }
            // .cf-top padding 12 12 4, icon 30; .cf-hint padding 6 12 0 52;
            // .cf-b padding 12, buttons 32.
            const float tx = il + 52.0f * s - 1.0f * s;
            const float text_w = ir - 12.0f * s - tx;
            // .tg.r after the title (删除类): inline when it fits, else below.
            const float tag_w = b.tag.empty() ? 0.0f : c.Measure(b.tag, formats_.badge) + 12.0f * s;
            const float title_w1 = c.Measure(b.title, formats_.body_b);
            const bool tag_inline = !b.tag.empty() && title_w1 + 6.0f * s + tag_w <= text_w;
            float title_h = (std::max)(20.0f * s, wrapped_h(b.title, formats_.body_b, text_w, 20.0f * s));
            if (!b.tag.empty() && !tag_inline) title_h += 20.0f * s;
            const float sub_h = b.sub.empty() ? 0.0f : 2.0f * s + wrapped_h(b.sub, formats_.caption, text_w, 18.0f * s);
            const float top_h = 12.0f * s + (std::max)(30.0f * s, title_h + sub_h) + 4.0f * s;
            const float hint_w = ir - 12.0f * s - (tx + 19.0f * s);
            const float hint0_h = b.hint.empty() ? 0.0f : 6.0f * s + wrapped_h(b.hint, formats_.caption, hint_w, 18.0f * s);
            const float hint1_h = b.hint1.empty() ? 0.0f : (b.hint.empty() ? 6.0f * s : 3.0f * s) +
                                                               wrapped_h(b.hint1, formats_.caption, hint_w, 18.0f * s);
            const bool any_hint = !b.hint.empty() || !b.hint1.empty();
            const float hint2_h = b.hint2.empty() ? 0.0f : (any_hint ? 3.0f * s : 6.0f * s) +
                                                               wrapped_h(b.hint2, formats_.caption, hint_w, 18.0f * s);
            const float hint_h = hint0_h + hint1_h + hint2_h;
            // P3 .cf-file: margin 8 12 0 52, padding 7 10, r 8, pane, line2
            // border; file icon 15, the name 12.5, the note text3 at the right.
            const float file_h = b.file.empty() ? 0.0f : 8.0f * s + 34.0f * s;
            // P4c .cf-rows: margin 8 12 0 52, line2 border r8, pane; rows
            // padding 7 10 at 12.5 with a line2 rule between, .to text3 12 at
            // the right; .more text3. .cf-bin: margin-top 8, accent 12.5, check 14.
            const size_t cf_n = b.cf_rows.size() + (b.cf_more.empty() ? 0 : 1);
            const float rows_h = cf_n ? 8.0f * s + 2.0f * s + static_cast<float>(cf_n) * 32.0f * s : 0.0f;
            const float bin_h = b.cf_bin.empty() ? 0.0f : 8.0f * s + wrapped_h(b.cf_bin, formats_.small125, text_w - 20.0f * s, 18.0f * s);
            // .lktip: max-height 0 → 40 and opacity while the link is hovered
            // (.3 s / .25 s); padding 0 12 10 52, margin-top -4.
            const float tip_k = b.extra_tip.empty() ? 0.0f
                                                    : TweenTo(key + 2, hot(AiPanelPart::ConfirmExtra, index) ? 1.0f : 0.0f, 300, now_ms);
            const float tip_h = tip_k * (18.0f * s + 10.0f * s - 4.0f * s);
            const float h = 2.0f * s + top_h + file_h + rows_h + bin_h + hint_h + 56.0f * s + tip_h;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            if (visible(r.top - 4.0f * s, r.bottom + 4.0f * s)) {
                // box-shadow 0 0 0 3px amber 12% (.danger: red).
                c.Fill(Inflate(r, 3.0f * s), 15.0f * s, Alpha(b.danger ? pal.red : pal.amber, 0.12f));
                card(r, b.danger ? pal.red_line : pal.amber_line);
                const float iy = r.top + 1.0f * s + 12.0f * s;
                c.Fill(D2D1::RectF(il + 12.0f * s, iy, il + 42.0f * s, iy + 30.0f * s), 8.0f * s, b.danger ? pal.red_bg : pal.amber_bg);
                c.Icon(b.danger ? AiIcon::Trash : b.cf_icon, il + 18.5f * s, iy + 6.5f * s, 17.0f * s, b.danger ? pal.red : pal.amber);
                wrapped(b.title, formats_.body_b, tx, iy, text_w, 20.0f * s, pal.text);
                if (!b.tag.empty()) {
                    const float gx = tag_inline ? tx + title_w1 + 6.0f * s : tx;
                    const float gy = tag_inline ? iy + 1.5f * s : iy + title_h - 20.0f * s + 1.5f * s;
                    const D2D1_RECT_F gr = D2D1::RectF(gx, gy, gx + tag_w, gy + 17.0f * s);
                    c.Fill(gr, 8.0f * s, b.tag_amber ? pal.amber_bg : pal.red_bg);
                    line(b.tag, formats_.badge, gr.left + 6.0f * s, gr.top, gr.right - 4.0f * s, 17.0f * s,
                         b.tag_amber ? pal.amber : pal.red);
                }
                if (!b.sub.empty()) wrapped(b.sub, formats_.caption, tx, iy + title_h + 2.0f * s, text_w, 18.0f * s, pal.text2);
                float yy = r.top + 1.0f * s + top_h;
                if (!b.file.empty()) {
                    yy += 8.0f * s;
                    const D2D1_RECT_F fr = D2D1::RectF(tx, yy, ir - 12.0f * s, yy + 34.0f * s);
                    c.Fill(fr, 8.0f * s, pal.line2);
                    c.Fill(D2D1::RectF(fr.left + 1.0f * s, fr.top + 1.0f * s, fr.right - 1.0f * s, fr.bottom - 1.0f * s),
                           7.0f * s, pal.pane);
                    c.Icon(AiIcon::File, fr.left + 10.0f * s, fr.top + 9.5f * s, 15.0f * s, pal.text2);
                    const float note_w = b.file_note.empty() ? 0.0f : c.Measure(b.file_note, formats_.caption) + 1.0f;
                    const float name_r = fr.right - 10.0f * s - (note_w > 0.0f ? note_w + 10.0f * s : 0.0f);
                    line(b.file, formats_.small125, fr.left + 33.0f * s, fr.top + 8.0f * s, name_r, 18.0f * s, pal.text);
                    if (note_w > 0.0f)
                        line(b.file_note, formats_.caption, fr.right - 10.0f * s - note_w, fr.top + 8.0f * s,
                             fr.right - 10.0f * s + 1.0f, 18.0f * s, pal.text3);
                    yy += 34.0f * s;
                }
                if (cf_n) {
                    yy += 8.0f * s;
                    const D2D1_RECT_F fr = D2D1::RectF(tx, yy, ir - 12.0f * s, yy + rows_h - 8.0f * s);
                    c.Fill(fr, 8.0f * s, pal.line2);
                    c.Fill(D2D1::RectF(fr.left + 1.0f * s, fr.top + 1.0f * s, fr.right - 1.0f * s, fr.bottom - 1.0f * s),
                           7.0f * s, pal.pane);
                    float ry = fr.top + 1.0f * s;
                    for (size_t k = 0; k < cf_n; ++k) {
                        if (k > 0)
                            c.Fill(D2D1::RectF(fr.left + 1.0f * s, ry - 0.5f * s, fr.right - 1.0f * s, ry + 0.5f * s), 0.0f, pal.line2);
                        if (k < b.cf_rows.size()) {
                            const AiConfirmRow& row = b.cf_rows[k];
                            c.Icon(row.folder ? AiIcon::Folder : AiIcon::File, fr.left + 10.0f * s, ry + 8.5f * s, 14.0f * s, pal.text2);
                            const float to_w = row.to.empty() ? 0.0f : (std::min)(c.Measure(row.to, formats_.caption) + 1.0f, (fr.right - fr.left) * 0.45f);
                            const float name_r = fr.right - 10.0f * s - (to_w > 0.0f ? to_w + 8.0f * s : 0.0f);
                            line(row.name, formats_.small125, fr.left + 32.0f * s, ry + 7.0f * s, name_r, 18.0f * s, pal.text);
                            if (to_w > 0.0f)
                                line(row.to, formats_.caption, fr.right - 10.0f * s - to_w, ry + 7.0f * s, fr.right - 10.0f * s + 1.0f,
                                     18.0f * s, pal.text3);
                        } else {
                            line(b.cf_more, formats_.small125, fr.left + 10.0f * s, ry + 7.0f * s, fr.right - 10.0f * s, 18.0f * s, pal.text3);
                        }
                        ry += 32.0f * s;
                    }
                    yy += rows_h - 8.0f * s;
                }
                if (!b.cf_bin.empty()) {
                    yy += 8.0f * s;
                    c.Icon(AiIcon::Check, tx, yy + 2.0f * s, 14.0f * s, pal.accent);
                    wrapped(b.cf_bin, formats_.small125, tx + 20.0f * s, yy, text_w - 20.0f * s, 18.0f * s, pal.accent);
                    yy += bin_h - 8.0f * s;
                }
                if (!b.hint.empty()) {
                    yy += 6.0f * s;
                    c.Icon(b.hint_icon, tx, yy + 2.5f * s, 13.0f * s, pal.text3);
                    wrapped(b.hint, formats_.caption, tx + 19.0f * s, yy, hint_w, 18.0f * s, pal.text3);
                    yy += hint0_h - 6.0f * s;
                }
                if (!b.hint1.empty()) {
                    const float gap = b.hint.empty() ? 6.0f * s : 3.0f * s;
                    yy += gap;
                    c.Icon(b.hint1_icon, tx, yy + 2.5f * s, 13.0f * s, pal.text3);
                    wrapped(b.hint1, formats_.caption, tx + 19.0f * s, yy, hint_w, 18.0f * s, pal.text3);
                    yy += hint1_h - gap;
                }
                if (!b.hint2.empty()) {
                    yy += any_hint ? 3.0f * s : 6.0f * s;
                    c.Icon(b.hint2_warn ? AiIcon::Warn : AiIcon::Check, tx, yy + 2.5f * s, 13.0f * s, b.hint2_warn ? pal.amber : pal.ok);
                    wrapped(b.hint2, formats_.caption, tx + 19.0f * s, yy, hint_w, 18.0f * s, b.hint2_warn ? pal.amber : pal.text3);
                    yy += hint2_h - (any_hint ? 3.0f * s : 6.0f * s);
                }
                yy += 12.0f * s;
                float bx = ir - 12.0f * s;
                bx -= button(bx, yy, b.accept, true, AiIcon::Check, false, AiPanelPart::ConfirmAccept, index, 0.0f,
                             b.accept_off ? 2 : b.danger ? 1 : 0) + 8.0f * s;
                bx -= button(bx, yy, b.deny, false, AiIcon::Close, false, AiPanelPart::ConfirmDeny, index);
                if (!b.extra.empty()) {
                    // .lk: margin-right auto, no border or fill, accent 600,
                    // padding 0 8, sliders icon 14, gap 6; hover fill. In a
                    // narrow panel the text ends in … (only the icon when tight).
                    const float room = bx - 8.0f * s - (il + 12.0f * s);
                    float lw = 8.0f * s + 14.0f * s + 6.0f * s + c.Measure(b.extra, formats_.body13b) + 8.0f * s;
                    if (lw > room) lw = room >= 8.0f * s + 14.0f * s + 6.0f * s + 40.0f * s + 8.0f * s ? room : 30.0f * s;
                    const D2D1_RECT_F lr = D2D1::RectF(il + 12.0f * s, yy, il + 12.0f * s + lw, yy + 32.0f * s);
                    const bool lh = hot(AiPanelPart::ConfirmExtra, index);
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    if (down(AiPanelPart::ConfirmExtra, index) && motion_on) {
                        const D2D1_POINT_2F mid = D2D1::Point2F((lr.left + lr.right) * 0.5f, (lr.top + lr.bottom) * 0.5f);
                        dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * before);
                    }
                    if (lh) c.Fill(lr, 8.0f * s, pal.hover);
                    c.Icon(AiIcon::Sliders, lr.left + 8.0f * s, yy + 9.0f * s, 14.0f * s, pal.accent);
                    if (lw > 30.0f * s)
                        line(b.extra, formats_.body13b, lr.left + 28.0f * s, yy, lr.right - 8.0f * s, 32.0f * s, pal.accent);
                    dc->SetTransform(before);
                    hit(lr, AiPanelPart::ConfirmExtra, index);
                }
                if (tip_k > 0.0f) {
                    const float ty = yy + 32.0f * s + 12.0f * s - 4.0f * s;
                    const D2D1_RECT_F clip = D2D1::RectF(il, ty, ir, ty + tip_h);
                    dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                    line(b.extra_tip, formats_.meta, il + 52.0f * s - 1.0f * s, ty, ir - 12.0f * s, 18.0f * s,
                         Alpha(pal.text3, std::clamp(tip_k * 1.2f, 0.0f, 1.0f)));
                    dc->PopAxisAlignedClip();
                }
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Result: {
            place(0.0f);
            // .result padding 12; .r-i 30 circle; .r-fs margin 10 0 0 40,
            // gap 6, tags lh 22; .r-b margin 12 0 0 40.
            const float il = x0 + 1.0f * s + 12.0f * s;
            const float ir = x1 - 1.0f * s - 12.0f * s;
            const float tx = il + 40.0f * s;
            const float text_w = ir - tx;
            const float title_h = (std::max)(20.0f * s, wrapped_h(b.title, formats_.body_b, text_w, 20.0f * s));
            // A sub of two parts (an ideographic space between) that does not
            // fit one line breaks between the parts, not inside the second.
            std::wstring sub_text = b.sub;
            if (const size_t gap = sub_text.find(L'\u3000');
                gap != std::wstring::npos && c.Measure(sub_text, formats_.caption) > text_w)
                sub_text[gap] = L'\n';
            const float sub_h = sub_text.empty() ? 0.0f : 2.0f * s + wrapped_h(sub_text, formats_.caption, text_w, 18.0f * s);
            const float top_h = (std::max)(30.0f * s, title_h + sub_h);
            // Tag rows (flex-wrap).
            struct TagPos { float x, y, w; };
            std::vector<TagPos> tags;
            float tags_h = 0.0f;
            if (!b.done && !b.tags.empty()) {
                float tx2 = tx, ty2 = 0.0f;
                for (const AiTagView& t : b.tags) {
                    const float tw = (6.0f + 13.0f + 5.0f + 8.0f) * s + c.Measure(t.text, formats_.caption_b);
                    if (tx2 > tx && tx2 + tw > ir) { tx2 = tx; ty2 += 28.0f * s; }
                    tags.push_back({tx2, ty2, tw});
                    tx2 += tw + 6.0f * s;
                }
                tags_h = 10.0f * s + ty2 + 22.0f * s;
            }
            // .r-ln: margin 10 0 0 40, gap 4, 12.5 text2 lh 18, icon 13 (ok /
            // amber; a warn line is amber). .gone drops the warn lines.
            std::vector<size_t> shown_lines;
            if (!b.done)
                for (size_t i = 0; i < b.lines.size(); ++i)
                    if (!(b.gone && b.lines[i].warn)) shown_lines.push_back(i);
            // A plain line (no bold part) wraps like the HTML's flex text; a
            // split line stays on one row.
            const float ln_w = (std::max)(1.0f, ir - (tx + 20.0f * s));
            // Key caps (改快捷键): old caps → new caps; when that is wider
            // than the line, → and the new caps go on a second row.
            const std::wstring caps_arrow = L"\u2192 ";
            const auto cap_row_w = [&](const std::vector<std::wstring>& keys) {
                float w = 0.0f;
                for (const std::wstring& cap : keys) w += c.Measure(cap, formats_.small11b) + 12.0f * s + 3.0f * s;
                return w;
            };
            const auto caps_two_rows = [&](const AiResultLine& ln) {
                if (ln.caps_to.empty() || ln.caps_from.empty()) return false;
                return c.Measure(ln.before, formats_.small125) + cap_row_w(ln.caps_from) + c.Measure(caps_arrow, formats_.small125) +
                           cap_row_w(ln.caps_to) > ln_w + 0.5f;
            };
            std::vector<float> line_hs;
            for (const size_t i : shown_lines) {
                const AiResultLine& ln = b.lines[i];
                line_hs.push_back(ln.bold.empty() && ln.after.empty() && ln.caps_to.empty()
                                      ? (std::max)(18.0f * s, wrapped_h(ln.before, formats_.small125, ln_w, 18.0f * s))
                                  : caps_two_rows(ln) ? 40.0f * s
                                                      : 18.0f * s);
            }
            float lines_h = shown_lines.empty() ? 0.0f : 10.0f * s + 4.0f * s * static_cast<float>(shown_lines.size() - 1);
            for (const float lh : line_hs) lines_h += lh;
            // Third batch. .r-chg: margin 10 0 0 40, border 1 line2, r 9; rows
            // padding 8 10 (12.5 lh 18), a line2 rule between them.
            const float chg_n = static_cast<float>(b.changes.size());
            const float chg_h = b.changes.empty() ? 0.0f : 10.0f * s + 2.0f * s + 34.0f * s * chg_n + 1.0f * s * (chg_n - 1.0f);
            // .r-warn: margin 8 0 0 40, warn 13 (amber), gap 7, 12 text2 lh 18.
            const float warn_w = (std::max)(1.0f, text_w - 20.0f * s);
            const float warn_h = b.done || b.gone || b.warn.empty()
                                     ? 0.0f
                                     : 8.0f * s + (std::max)(18.0f * s, wrapped_h(b.warn, formats_.caption, warn_w, 18.0f * s));
            // .r-tip: margin 10 0 0 40, kbd 14 (text3, margin 4 2 0 0), gap 5;
            // .ktab rows lh 22, gap 4: the name (min 112, 12 text2), key caps.
            const bool keys = !b.done && !b.keys.empty();
            const float keys_n = static_cast<float>(b.keys.size());
            const float keys_h = keys ? 10.0f * s + 22.0f * s * keys_n + 4.0f * s * (keys_n - 1.0f) : 0.0f;
            // .r-disk: margin 10 0 0 40, padding 9 10 8, r 9, pane, line2 border.
            const bool mini = b.mini.on && !b.gone && !b.done;
            const float mini_h = mini ? 10.0f * s + 2.0f * s + 9.0f * s + 20.0f * s + 6.0f * s + 8.0f * s + 6.0f * s + 15.0f * s + 8.0f * s : 0.0f;
            const bool buttons = !b.done && !b.gone && !b.freed && (!b.deny.empty() || !b.accept.empty() || !b.purge.empty());
            // 永久删除这些… goes after 撤销, or onto a second row (flex-wrap).
            const float purge_w = b.purge.empty() ? 0.0f : 28.0f * s + 20.0f * s + c.Measure(b.purge, formats_.body13);
            const float undo_w = 28.0f * s + 20.0f * s + c.Measure(b.deny, formats_.body13);
            const bool purge_wraps = buttons && !b.purge.empty() && undo_w + 8.0f * s + purge_w > text_w + 0.5f;
            const float h = 2.0f * s + 24.0f * s + top_h + lines_h + chg_h + warn_h + keys_h + tags_h + mini_h +
                            (buttons ? 44.0f * s + (purge_wraps ? 40.0f * s : 0.0f) : 0.0f);
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                const float iy = r.top + 1.0f * s + 12.0f * s;
                const D2D1_POINT_2F mid = D2D1::Point2F(il + 15.0f * s, iy + 15.0f * s);
                const bool partial = b.partial && !b.done && !b.gone;
                c.dc->FillEllipse(D2D1::Ellipse(mid, 15.0f * s, 15.0f * s),
                                  c.B(b.done || b.gone ? pal.hover : partial ? pal.amber_bg : pal.accent_soft));
                c.Icon(b.done ? AiIcon::Undo : partial ? AiIcon::Warn : AiIcon::Check, mid.x - 8.0f * s, mid.y - 8.0f * s,
                       16.0f * s, b.done || b.gone ? pal.text2 : partial ? pal.amber : pal.ok);
                wrapped(b.title, formats_.body_b, tx, iy, text_w, 20.0f * s, pal.text);
                if (!sub_text.empty()) wrapped(sub_text, formats_.caption, tx, iy + title_h + 2.0f * s, text_w, 18.0f * s, pal.text2);
                float yy = iy + top_h;
                if (lines_h > 0.0f) {
                    yy += 10.0f * s;
                    for (size_t n = 0; n < shown_lines.size(); ++n) {
                        const AiResultLine& ln = b.lines[shown_lines[n]];
                        const D2D1_COLOR_F ink = ln.warn ? pal.amber : ln.mute ? pal.text3 : pal.text2;
                        if (ln.mute)   // the mock's dash: 6 → 18 of 24 at 1.6
                            c.Fill(D2D1::RectF(tx + 3.25f * s, yy + 8.2f * s, tx + 9.75f * s, yy + 9.8f * s), 0.8f * s, pal.text3);
                        else
                            c.Icon(ln.warn ? AiIcon::Warn : AiIcon::Check, tx, yy + 2.5f * s, 13.0f * s, ln.warn ? pal.amber : pal.ok);
                        float lx = tx + 20.0f * s;
                        if (!ln.caps_to.empty()) {
                            // .r-ln .k: 600 11/18, padding 0 6, border line (2 below), r 5, margin-right 3.
                            line(ln.before, formats_.small125, lx, yy, ir, 18.0f * s, ink);
                            lx += c.Measure(ln.before, formats_.small125);
                            const float caps_left = lx;
                            const bool two_rows = caps_two_rows(ln);
                            float ky = yy;
                            const auto caps = [&](const std::vector<std::wstring>& keys) {
                                for (const std::wstring& cap : keys) {
                                    const float cw = c.Measure(cap, formats_.small11b) + 12.0f * s;
                                    if (lx + cw > ir + 0.5f) return;
                                    const D2D1_RECT_F kr = D2D1::RectF(lx, ky, lx + cw, ky + 18.0f * s);
                                    c.Fill(kr, 5.0f * s, pal.line);
                                    c.Fill(D2D1::RectF(kr.left + 1.0f * s, kr.top + 1.0f * s, kr.right - 1.0f * s,
                                                       kr.bottom - 2.0f * s),
                                           4.0f * s, pal.bg);
                                    line(cap, formats_.small11b, kr.left + 6.0f * s, kr.top, kr.right - 6.0f * s + 1.0f,
                                         17.0f * s, pal.text);
                                    lx += cw + 3.0f * s;
                                }
                            };
                            caps(ln.caps_from);
                            if (two_rows) {
                                // The new caps under the old ones, → hanging before them; from
                                // the line's start when that is too narrow.
                                ky = yy + 22.0f * s;
                                const float hang = caps_left - c.Measure(caps_arrow, formats_.small125);
                                lx = hang >= tx + 20.0f * s && caps_left + cap_row_w(ln.caps_to) <= ir + 0.5f ? hang : tx + 20.0f * s;
                            }
                            if (!ln.caps_from.empty()) {
                                line(caps_arrow, formats_.small125, lx, ky, ir, 18.0f * s, pal.text3);
                                lx += c.Measure(caps_arrow, formats_.small125);
                            }
                            caps(ln.caps_to);
                        } else if (ln.bold.empty() && ln.after.empty()) {
                            wrapped(ln.before, formats_.small125, lx, yy, ln_w, 18.0f * s, ink);
                        } else {
                            const float w1 = c.Measure(ln.before, formats_.small125);
                            line(ln.before, formats_.small125, lx, yy, ir, 18.0f * s, ink);
                            lx += w1;
                            const float w2 = c.Measure(ln.bold, formats_.small125b);
                            line(ln.bold, formats_.small125b, lx, yy, ir, 18.0f * s,
                                 ln.warn ? pal.amber : ln.mute ? pal.text3 : pal.text);
                            lx += w2;
                            line(ln.after, formats_.small125, lx, yy, ir, 18.0f * s, ink);
                        }
                        yy += line_hs[n] + (n + 1 < shown_lines.size() ? 4.0f * s : 0.0f);
                    }
                }
                if (chg_h > 0.0f) {
                    // .chg: the name (semibold) and its tag, then old → new at
                    // the right. Undone: the old value comes back (text), the
                    // new one is struck (text3), the tag goes.
                    yy += 10.0f * s;
                    const D2D1_RECT_F box = D2D1::RectF(tx, yy, ir, yy + chg_h - 10.0f * s);
                    c.Stroke(Inflate(box, -0.5f), 8.5f * s, pal.line2);
                    const float cl = box.left + 1.0f * s + 10.0f * s, cr = box.right - 1.0f * s - 10.0f * s;
                    const float rule = (std::max)(1.0f, std::round(1.0f * s));
                    float ry = box.top + 1.0f * s;
                    for (size_t n = 0; n < b.changes.size(); ++n) {
                        const AiChangeView& ch = b.changes[n];
                        if (n > 0) {
                            c.Fill(D2D1::RectF(box.left + 1.0f * s, ry, box.right - 1.0f * s, ry + rule), 0.0f, pal.line2);
                            ry += 1.0f * s;
                        }
                        const float ty = ry + 8.0f * s;
                        const float sy = std::round(ty + 9.5f * s);
                        const float to_w = c.Measure(ch.to, formats_.small125b);
                        float vx = cr - to_w - 14.0f * s;
                        if (!b.done) c.Fill(D2D1::RectF(vx, ty, cr, ty + 18.0f * s), 6.0f * s, pal.accent_soft);
                        line(ch.to, formats_.small125b, vx + 7.0f * s, ty, cr - 7.0f * s + 1.0f, 18.0f * s,
                             b.done ? pal.text3 : pal.accent);
                        if (b.done)
                            c.Fill(D2D1::RectF(vx + 7.0f * s, sy, vx + 7.0f * s + to_w, sy + rule), 0.0f, Alpha(pal.text3, 0.7f));
                        vx -= 6.0f * s + 12.0f * s;
                        c.Icon(AiIcon::Arrow, vx, ty + 3.0f * s, 12.0f * s, pal.text3);
                        const float from_w = c.Measure(ch.from, formats_.small125);
                        vx -= 6.0f * s + from_w;
                        line(ch.from, formats_.small125, vx, ty, vx + from_w + 1.0f, 18.0f * s, b.done ? pal.text : pal.text3);
                        if (!b.done) c.Fill(D2D1::RectF(vx, sy, vx + from_w, sy + rule), 0.0f, Alpha(pal.text3, 0.7f));
                        // .tg.w: 10.5/600 lh 17, padding 0 6, r 8, amber.
                        const bool tagged = !b.done && !ch.tag.empty();
                        const float tag_w = tagged ? c.Measure(ch.tag, formats_.badge) + 12.0f * s : 0.0f;
                        const float name_r = vx - 8.0f * s - (tagged ? tag_w + 6.0f * s : 0.0f);
                        const float name_w = (std::min)(c.Measure(ch.name, formats_.small125b), (std::max)(0.0f, name_r - cl));
                        line(ch.name, formats_.small125b, cl, ty, cl + name_w + 1.0f, 18.0f * s, pal.text);
                        if (tagged) {
                            const D2D1_RECT_F tr = D2D1::RectF(cl + name_w + 6.0f * s, ty + 0.5f * s, cl + name_w + 6.0f * s + tag_w,
                                                               ty + 17.5f * s);
                            c.Fill(tr, 8.0f * s, pal.amber_bg);
                            line(ch.tag, formats_.badge, tr.left + 6.0f * s, tr.top, tr.right - 6.0f * s + 1.0f, 17.0f * s, pal.amber);
                        }
                        ry += 34.0f * s;
                    }
                    yy = box.bottom;
                }
                if (warn_h > 0.0f) {
                    yy += 8.0f * s;
                    c.Icon(AiIcon::Warn, tx, yy + 2.5f * s, 13.0f * s, pal.amber);
                    wrapped(b.warn, formats_.caption, tx + 20.0f * s, yy, warn_w, 18.0f * s, pal.text2);
                    yy += warn_h - 8.0f * s;
                }
                if (mini) {
                    // .rd-top: label, then (.ar 17.0 GB →) the 16/600 figure
                    // and its unit at the right; .dk-bar.sm 8; .rd-leg 11 text3.
                    const AiMiniDiskView& m = b.mini;
                    yy += 10.0f * s;
                    const D2D1_RECT_F box = D2D1::RectF(tx, yy, ir, yy + mini_h - 10.0f * s);
                    c.Fill(box, 9.0f * s, pal.pane);
                    c.Stroke(Inflate(box, -0.5f), 8.5f * s, pal.line2);
                    const float bl2 = box.left + 1.0f * s + 10.0f * s, br2 = box.right - 1.0f * s - 10.0f * s;
                    float my = box.top + 1.0f * s + 9.0f * s;
                    const float k = Count(m.count_ms, 900);
                    const std::wstring val = Fixed1(m.value_from + (m.value - m.value_from) * k);
                    const float unit_w = c.Measure(m.unit, formats_.caption);
                    const float val_w = c.Measure(val, formats_.num16);
                    float rx = br2 - unit_w;
                    line(m.unit, formats_.caption, rx, my + 3.0f * s, br2 + 1.0f, 16.0f * s, pal.text2);
                    rx -= 5.0f * s + val_w;
                    line(val, formats_.num16, rx, my, rx + val_w + 1.0f, 20.0f * s, pal.text);
                    if (!m.from.empty()) {
                        const float fw = c.Measure(m.from, formats_.caption);
                        rx -= 5.0f * s + fw;
                        line(m.from, formats_.caption, rx, my + 3.0f * s, rx + fw + 1.0f, 16.0f * s, pal.text3);
                    }
                    line(m.label, formats_.caption, bl2, my + 3.0f * s, rx - 6.0f * s, 16.0f * s, pal.text2);
                    my += 20.0f * s + 6.0f * s;
                    const D2D1_RECT_F bar = D2D1::RectF(bl2, my, br2, my + 8.0f * s);
                    c.Fill(bar, 4.0f * s, pal.line2);
                    c.Stroke(Inflate(bar, -0.5f), 3.5f * s, pal.line);
                    if (PushRounded(c, bar, 4.0f * s)) {
                        const float bw = bar.right - bar.left;
                        const float uw = bw * std::clamp(m.used, 0.0f, 1.0f);
                        c.Fill(D2D1::RectF(bar.left, bar.top, bar.left + uw, bar.bottom), 0.0f, pal.sys);
                        const float rk = m.recycle_from + (m.recycle - m.recycle_from) * k;
                        // A batch of a few hundred MB on a big drive stays
                        // visible: 5 px floor, eased in or out with the count.
                        const float mf = m.recycle_from > 0.0f ? 1.0f : 0.0f, mt = m.recycle > 0.0f ? 1.0f : 0.0f;
                        const float rw = (std::max)(bw * std::clamp(rk, 0.0f, 1.0f), 5.0f * s * (mf + (mt - mf) * k));
                        if (rw > 0.3f) {
                            Hatch(c, D2D1::RectF(bar.left + uw, bar.top, bar.left + uw + rw, bar.bottom), pal.amber, 3.0f * s);
                            c.Fill(D2D1::RectF(bar.left + uw, bar.top, bar.left + uw + 1.0f * s, bar.bottom), 0.0f, pal.card);
                        }
                        dc->PopLayer();
                    }
                    my += 8.0f * s + 6.0f * s;
                    float gx = bl2;
                    const auto leg = [&](int kind, const std::wstring& t) {
                        if (t.empty()) return;
                        const D2D1_RECT_F sw = D2D1::RectF(gx, my + 3.5f * s, gx + 8.0f * s, my + 11.5f * s);
                        if (kind == 0) c.Fill(sw, 2.0f * s, pal.sys);
                        else if (kind == 1) {
                            c.Fill(sw, 2.0f * s, Alpha(pal.amber, 0.35f));
                            if (PushRounded(c, sw, 2.0f * s)) { Hatch(c, sw, pal.amber, 2.0f * s); dc->PopLayer(); }
                        } else {
                            c.Fill(sw, 2.0f * s, pal.line2);
                            c.Stroke(Inflate(sw, -0.5f), 1.5f * s, pal.line);
                        }
                        const float tw = c.Measure(t, formats_.small11);
                        line(t, formats_.small11, gx + 13.0f * s, my, (std::min)(gx + 13.0f * s + tw + 1.0f, br2), 15.0f * s, pal.text3);
                        gx += 13.0f * s + tw + 12.0f * s;
                    };
                    leg(0, m.leg_used);
                    if (m.recycle > 0.0f) leg(1, m.leg_recycle);
                    leg(2, m.leg_free);
                    yy = box.bottom;
                }
                if (!tags.empty()) {
                    yy += 10.0f * s;
                    for (size_t i = 0; i < tags.size(); ++i) {
                        const AiTagView& t = b.tags[i];
                        const int ti = static_cast<int>(t.tint);
                        const D2D1_RECT_F tr = D2D1::RectF(tags[i].x, yy + tags[i].y, tags[i].x + tags[i].w,
                                                           yy + tags[i].y + 22.0f * s);
                        c.Fill(tr, 7.0f * s, pal.tint_bg[ti]);
                        c.Icon(AiIcon::Folder, tr.left + 6.0f * s, tr.top + 4.5f * s, 13.0f * s, pal.tint_fg[ti]);
                        line(t.text, formats_.caption_b, tr.left + 24.0f * s, tr.top, tr.right, 22.0f * s, pal.tint_fg[ti]);
                    }
                    yy += tags_h - 10.0f * s;
                }
                if (keys) {
                    // .ktab: each row's name (min 112), gap 4, then the caps
                    // (.k: 11/600 lh 18, padding 0 6, border 1 line with a 2
                    // px bottom, r 5, bg), gap 4.
                    yy += 10.0f * s;
                    bool only_palette = true;
                    for (const AiKeyRowView& row : b.keys) only_palette = only_palette && row.palette;
                    c.Icon(only_palette ? AiIcon::Search : AiIcon::Keyboard, tx, yy + 4.0f * s, 14.0f * s, pal.text3);
                    const float kx = tx + 21.0f * s;
                    for (size_t n = 0; n < b.keys.size(); ++n) {
                        const AiKeyRowView& k = b.keys[n];
                        const float ry = yy + 26.0f * s * static_cast<float>(n);
                        // The name column gives way (down to the name itself)
                        // before a cap is dropped in a narrow panel.
                        // One name column for every row, so the caps line up.
                        float name_w = 0.0f, caps_w = 0.0f;
                        for (const AiKeyRowView& row : b.keys) {
                            name_w = (std::max)(name_w, c.Measure(row.label, formats_.caption));
                            float row_w = 0.0f;
                            for (const std::wstring& cap : row.caps) row_w += c.Measure(cap, formats_.small11b) + 14.0f * s + 4.0f * s;
                            caps_w = (std::max)(caps_w, row_w);
                        }
                        const float lw = (std::max)(name_w, (std::min)(112.0f * s, ir - kx - 4.0f * s - caps_w));
                        line(k.label, formats_.caption, kx, ry, (std::min)(kx + lw + 1.0f, ir), 22.0f * s, pal.text2);
                        float cx = kx + lw + 4.0f * s;
                        for (const std::wstring& cap : k.caps) {
                            const float cw = c.Measure(cap, formats_.small11b) + 14.0f * s;
                            if (cx + cw > ir + 0.5f) break;
                            const D2D1_RECT_F kr = D2D1::RectF(cx, ry + 0.5f * s, cx + cw, ry + 21.5f * s);
                            if (k.palette) {
                                // .pk: accent-soft, accent 600, r 5, no border.
                                const D2D1_RECT_F pr = D2D1::RectF(kr.left, ry + 2.0f * s, kr.right, ry + 20.0f * s);
                                c.Fill(pr, 5.0f * s, pal.accent_soft);
                                line(cap, formats_.small11b, pr.left + 7.0f * s, pr.top, pr.right - 7.0f * s + 1.0f, 18.0f * s,
                                     pal.accent);
                                cx += cw + 4.0f * s;
                                continue;
                            }
                            c.Fill(kr, 5.0f * s, pal.line);
                            c.Fill(D2D1::RectF(kr.left + 1.0f * s, kr.top + 1.0f * s, kr.right - 1.0f * s, kr.bottom - 2.0f * s),
                                   4.0f * s, pal.bg);
                            line(cap, formats_.small11b, kr.left + 7.0f * s, kr.top + 1.0f * s, kr.right - 7.0f * s + 1.0f, 18.0f * s,
                                 pal.text);
                            cx += cw + 4.0f * s;
                        }
                    }
                    yy += keys_h - 10.0f * s;
                }
                if (buttons) {
                    yy += 12.0f * s;
                    // Buttons flow left to right from the text column.
                    const float uw = 28.0f * s + 20.0f * s + c.Measure(b.deny, formats_.body13);
                    // Too tight for both from the text column: start them
                    // further left (down to the card's padding) before
                    // shortening the second one.
                    float bx0 = tx;
                    if (!b.accept.empty()) {
                        const float need = uw + 8.0f * s + 28.0f * s + 20.0f * s + c.Measure(b.accept, formats_.body13);
                        if (need > text_w) bx0 = (std::max)(il + 12.0f * s, tx + text_w - need);
                    }
                    float bx = bx0 + uw;
                    button(bx, yy, b.deny, false, AiIcon::Undo, true, AiPanelPart::ResultUndo, index);
                    if (!b.purge.empty()) {
                        // .b2.dz: red text and trash icon.
                        const float px = purge_wraps ? bx0 : bx + 8.0f * s;
                        const float py = purge_wraps ? yy + 40.0f * s : yy;
                        const float room = tx + text_w - px;
                        button(px + (std::min)(purge_w, room), py, b.purge, false, AiIcon::Trash, true, AiPanelPart::ResultPurge,
                               index, room, 3);
                    } else if (!b.accept.empty()) {
                        const float left = bx + 8.0f * s;
                        const float room = tx + text_w - left;
                        const float aw = (std::min)(28.0f * s + 20.0f * s + c.Measure(b.accept, formats_.body13), room);
                        button(left + aw, yy, b.accept, false, b.accept_icon, true, AiPanelPart::ResultShow, index, aw);
                    }
                }
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Acts: {
            // .acts: margin -4 0 8 -6, gap 2, buttons 28 (icon 15), meta 11.5.
            place(-4.0f * s);
            const float ay = top + y;
            if (visible(ay, ay + 28.0f * s)) {
                float x = x0 - 6.0f * s;
                const auto mini = [&](float mw, AiPanelPart mpart, AiIcon micon, float mpx, bool mon, bool men) {
                    const D2D1_RECT_F mr = D2D1::RectF(x, ay, x + mw, ay + 28.0f * s);
                    const bool mh = men && hot(mpart, index);
                    if (mh) c.Fill(mr, 7.0f * s, down(mpart, index) ? Mix(pal.hover, pal.text, 0.06f) : pal.hover);
                    const D2D1_COLOR_F ink = mon ? pal.accent : mh ? pal.text : pal.text2;
                    c.Icon(micon, x + (mw - mpx) * 0.5f, ay + (28.0f * s - mpx) * 0.5f, mpx,
                           men ? ink : Alpha(pal.text2, 0.35f));
                    if (men) hit(mr, mpart, index);
                    x += mw;
                };
                // .ver: ‹ 2 / 3 ›, margin 0 4 0 2, buttons 24 (icon 13), 12 text2.
                if (b.ver_count > 1) {
                    x += 2.0f * s;
                    mini(24.0f * s, AiPanelPart::VerPrev, AiIcon::ChevronLeft, 13.0f * s, false, b.ver > 1);
                    const std::wstring vt = std::to_wstring(b.ver) + L" / " + std::to_wstring(b.ver_count);
                    const float vw = c.Measure(vt, formats_.caption);
                    line(vt, formats_.caption, x, ay, x + vw + 2.0f, 28.0f * s, pal.text2);
                    x += vw;
                    mini(24.0f * s, AiPanelPart::VerNext, AiIcon::ChevronRight, 13.0f * s, false, b.ver < b.ver_count);
                    x += 4.0f * s;
                }
                if (!b.ver_only) {
                    mini(28.0f * s, AiPanelPart::ActCopy, AiIcon::Copy, 15.0f * s, false, true);
                    x += 2.0f * s;
                    // 重新生成 and its caret (16 wide, chevron 11) side by side.
                    mini(28.0f * s, AiPanelPart::ActRetry, AiIcon::Retry, 15.0f * s, false, true);
                    mini(16.0f * s, AiPanelPart::ActRegenMenu, AiIcon::ChevronDown, 11.0f * s, false, true);
                    x += 2.0f * s;
                    if (!b.done) {   // a failed turn has nothing to rate
                        mini(28.0f * s, AiPanelPart::ActThumb, AiIcon::Thumb, 15.0f * s, b.rating > 0, true);
                        x += 2.0f * s;
                        mini(28.0f * s, AiPanelPart::ActThumbDown, AiIcon::ThumbDown, 15.0f * s, b.rating < 0, true);
                        x += 2.0f * s;
                    }
                }
                line(b.meta, formats_.meta, x - 2.0f * s + 8.0f * s, ay, x1, 28.0f * s, pal.text3);
            }
            y += 28.0f * s;
            pending = 8.0f * s;
            break;
        }
        case AiBlockKind::Working: {
            // .working: h 28, gap 8, spark 16 spinning, 12.5 shimmer; margin 2 0 6.
            place(2.0f * s);
            const float wy = top + y;
            if (visible(wy, wy + 28.0f * s)) {
                AiIconPose pose;
                if (motion_on) { pose = SparkPose(now_ms); conv_decor_ = true; }
                c.Icon(AiIcon::Spark, x0, wy + 6.0f * s, 16.0f * s, pal.accent, pose);
                shimmer(b.text, formats_.small125, x0 + 24.0f * s, wy, 28.0f * s);
            }
            y += 28.0f * s;
            pending = 6.0f * s;
            break;
        }
        case AiBlockKind::Error: {
            // .err: border 1, radius 12, padding 12, margin 0 0 12. .e-top gap 10:
            // .e-i 30 tile (radius 8, icon 17); .e-t 13.5/600 lh 20; .e-s 12 lh 18
            // margin-top 2. .e-d / .e-h / .e-b margin-left 40.
            place(0.0f);
            if (b.done) {
                // .err.done (a paused task decided): padding 9 12, .e-i 24 on
                // hover (icon 14 text2), .e-t 13 regular text2 lh 24.
                const float h = 2.0f * s + 42.0f * s;
                const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
                if (visible(r.top, r.bottom)) {
                    card(r, pal.line);
                    const float il = x0 + 1.0f * s + 12.0f * s;
                    const float iy = r.top + 1.0f * s + 9.0f * s;
                    c.Fill(D2D1::RectF(il, iy, il + 24.0f * s, iy + 24.0f * s), 8.0f * s, pal.hover);
                    c.Icon(b.icon, il + 5.0f * s, iy + 5.0f * s, 14.0f * s, pal.text2);
                    line(b.title, formats_.body13, il + 34.0f * s, iy, x1 - 13.0f * s, 24.0f * s, pal.text2);
                }
                y += h;
                pending = 12.0f * s;
                break;
            }
            const bool live = !b.gone_ms;
            const float il = x0 + 1.0f * s + 12.0f * s;
            const float ir = x1 - 1.0f * s - 12.0f * s;
            const float tx = il + 40.0f * s;
            const float text_w = (std::max)(1.0f, ir - tx);
            const float title_h = (std::max)(20.0f * s, wrapped_h(b.title, formats_.body_b, text_w, 20.0f * s));
            const float sub_h = b.sub.empty() ? 0.0f : 2.0f * s + wrapped_h(b.sub, formats_.caption, text_w, 18.0f * s);
            const float top_h = (std::max)(30.0f * s, title_h + sub_h);
            // .e-d: padding 7 10, border 1 line2, radius 8, mono 11.5/17, at most 3 lines.
            const float dw = text_w - 22.0f * s;
            const float detail_h =
                b.detail.empty() ? 0.0f
                                 : (std::min)(51.0f * s, (std::max)(17.0f * s, wrapped_h(b.detail, formats_.mono, dw, 17.0f * s)));
            const float detail_box = b.detail.empty() ? 0.0f : 8.0f * s + detail_h + 16.0f * s;
            // .e-l: margin-top 8, rows 12 lh 18 gap 3; 13 px icons, gap 7.
            const float list_w = text_w - 20.0f * s;
            float list_h = 0.0f;
            for (const AiCheckView& ck : b.checklist)
                list_h += (list_h > 0.0f ? 3.0f * s : 0.0f) +
                          (std::max)(18.0f * s, wrapped_h(ck.text, formats_.caption, list_w, 18.0f * s));
            const float list_box = b.checklist.empty() ? 0.0f : 8.0f * s + list_h;
            // .e-h: warn 13, gap 6, 12 text3.
            const float hint_w = text_w - 19.0f * s;
            const float hint_h = b.hint.empty() ? 0.0f : (std::max)(18.0f * s, wrapped_h(b.hint, formats_.caption, hint_w, 18.0f * s));
            const float hint_box = b.hint.empty() ? 0.0f : 8.0f * s + hint_h;
            // .e-f: the file a paused task is stuck on; margin 8 0 0 40,
            // padding 6 10, radius 8, chrome, border line2, 12.
            const float file_box = b.file.empty() ? 0.0f : 8.0f * s + 30.0f * s;
            // .deny-l: margin 8 0 0 40, wrap, gap 6; pills 11.5 lh 20, padding
            // 0 8, r 7, hover, text2.
            struct PillPos { float x, y, w; };
            std::vector<PillPos> pills;
            float pills_h = 0.0f;
            if (!b.pills.empty()) {
                float px = tx, py = 0.0f;
                for (const std::wstring& t : b.pills) {
                    const float pw = (std::min)(c.Measure(t, formats_.meta) + 16.0f * s, text_w);
                    if (px > tx && px + pw > ir) { px = tx; py += 26.0f * s; }
                    pills.push_back({px, py, pw});
                    px += pw + 6.0f * s;
                }
                pills_h = 8.0f * s + py + 20.0f * s;
            }
            // .e-b: gap 8; buttons wrap when the panel is narrow.
            struct BtnPos { float x, y, w; };
            std::vector<BtnPos> btns;
            float bx = tx, by = 0.0f;
            for (const AiErrButton& eb : b.actions) {
                IDWriteTextFormat* fmt = eb.primary ? formats_.body13b : formats_.body13;
                // AiIcon::Count: a plain button (the voice cards).
                const float bw = 28.0f * s + (eb.icon == AiIcon::Count ? 0.0f : 20.0f * s) + c.Measure(eb.text, fmt);
                if (bx > tx && bx + bw > ir) { bx = tx; by += 40.0f * s; }
                btns.push_back({bx, by, bw});
                bx += bw + 8.0f * s;
            }
            const float btn_h = btns.empty() ? 0.0f : 12.0f * s + by + 32.0f * s;
            // .e-m: the model (or the card's `note`, in any tone), pushed right
            // of the buttons; a `note` that does not fit there gets its own line.
            const std::wstring& em = !b.note.empty() ? b.note : b.tone != AiErrTone::Accent ? b.meta : b.note;
            const bool em_on = !em.empty() && !btns.empty() && b.actions.size() < 3 && by == 0.0f;
            const float em_w = em_on ? c.Measure(em, formats_.meta) : 0.0f;
            const bool em_right = em_on && btns.back().x + btns.back().w + 12.0f * s + em_w <= ir;
            const bool em_below = em_on && !em_right && !b.note.empty();
            const float em_h = em_below ? 4.0f * s + 18.0f * s : 0.0f;
            const float h = 2.0f * s + 24.0f * s + top_h + detail_box + list_box + hint_box + pills_h + file_box + btn_h + em_h;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            if (visible(r.top - 4.0f * s, r.bottom + 4.0f * s)) {
                const D2D1_COLOR_F ink = b.tone == AiErrTone::Err ? pal.bad : b.tone == AiErrTone::Warn ? pal.amber : pal.accent;
                const D2D1_COLOR_F tile = b.tone == AiErrTone::Err    ? pal.bad_bg
                                          : b.tone == AiErrTone::Warn ? pal.amber_bg
                                                                      : pal.accent_soft;
                // Only the red tone takes a tinted border; a paused task (a
                // file row) asks like a confirmation: amber line + 3 px ring.
                const bool pause = !b.file.empty() && live;
                if (pause) c.Fill(Inflate(r, 3.0f * s), 15.0f * s, Alpha(pal.amber, 0.12f));
                card(r, b.tone == AiErrTone::Err ? pal.bad_line : pause ? pal.amber_line : pal.line);
                const float iy = r.top + 1.0f * s + 12.0f * s;
                const D2D1_RECT_F tr = D2D1::RectF(il, iy, il + 30.0f * s, iy + 30.0f * s);
                c.Fill(tr, 8.0f * s, tile);
                c.Icon(b.icon, il + 6.5f * s, iy + 6.5f * s, 17.0f * s, ink);
                wrapped(b.title, formats_.body_b, tx, iy, text_w, 20.0f * s, pal.text);
                if (!b.sub.empty()) wrapped(b.sub, formats_.caption, tx, iy + title_h + 2.0f * s, text_w, 18.0f * s, pal.text2);
                float yy = iy + top_h;
                if (!b.detail.empty()) {
                    yy += 8.0f * s;
                    const D2D1_RECT_F dr = D2D1::RectF(tx, yy, ir, yy + detail_h + 16.0f * s);
                    c.Fill(dr, 8.0f * s, pal.pane);
                    c.Stroke(dr, 8.0f * s, pal.line2);
                    dc->PushAxisAlignedClip(D2D1::RectF(dr.left, dr.top + 7.0f * s, dr.right, dr.top + 7.0f * s + detail_h + 1.0f),
                                            D2D1_ANTIALIAS_MODE_ALIASED);
                    wrapped(b.detail, formats_.mono, dr.left + 11.0f * s, dr.top + 8.0f * s, dw, 17.0f * s, pal.text2);
                    dc->PopAxisAlignedClip();
                    yy = dr.bottom;
                }
                if (!b.checklist.empty()) {
                    yy += 8.0f * s;
                    bool first_row = true;
                    for (const AiCheckView& ck : b.checklist) {
                        if (!first_row) yy += 3.0f * s;
                        first_row = false;
                        const float rh = (std::max)(18.0f * s, wrapped_h(ck.text, formats_.caption, list_w, 18.0f * s));
                        c.Icon(ck.warn ? AiIcon::Close : AiIcon::Check, tx, yy + 2.5f * s, 13.0f * s, ck.warn ? pal.bad : pal.ok);
                        wrapped(ck.text, formats_.caption, tx + 20.0f * s, yy, list_w, 18.0f * s, ck.warn ? pal.text : pal.text2);
                        yy += rh;
                    }
                }
                if (!b.hint.empty()) {
                    yy += 8.0f * s;
                    c.Icon(b.pills.empty() && b.checklist.empty() ? AiIcon::Warn : b.hint_icon, tx, yy + 2.5f * s, 13.0f * s, pal.text3);
                    wrapped(b.hint, formats_.caption, tx + 19.0f * s, yy, hint_w, 18.0f * s, pal.text3);
                    yy += hint_h;
                }
                if (!pills.empty()) {
                    yy += 8.0f * s;
                    for (size_t i = 0; i < pills.size(); ++i) {
                        const D2D1_RECT_F pr = D2D1::RectF(pills[i].x, yy + pills[i].y, pills[i].x + pills[i].w,
                                                           yy + pills[i].y + 20.0f * s);
                        c.Fill(pr, 7.0f * s, pal.hover);
                        line(b.pills[i], formats_.meta, pr.left + 8.0f * s, pr.top, pr.right - 8.0f * s + 1.0f, 20.0f * s, pal.text2);
                    }
                    yy += pills_h - 8.0f * s;
                }
                if (!b.file.empty()) {
                    yy += 8.0f * s;
                    const D2D1_RECT_F fr = D2D1::RectF(tx, yy, ir, yy + 30.0f * s);
                    c.Fill(fr, 8.0f * s, Mix(pal.pane, pal.hover, 0.45f));
                    c.Stroke(fr, 8.0f * s, pal.line2);
                    float note_l = fr.right - 10.0f * s;
                    if (!b.file_note.empty()) {
                        // The name comes first: the note gets what is left,
                        // but never less than 40% of the row.
                        const float nw = c.Measure(b.file_note, formats_.caption) + 16.0f * s;
                        const float name_w = c.Measure(b.file, formats_.caption_b);
                        const float row_l = fr.left + 34.0f * s, row_r = fr.right - 10.0f * s;
                        const float room = row_r - row_l - 12.0f * s;
                        const float note_w = (std::min)(nw, (std::max)(room - name_w, room * 0.4f));
                        note_l = row_r - note_w;
                        c.Icon(AiIcon::Lock, note_l, yy + 9.0f * s, 12.0f * s, pal.text3);
                        line(b.file_note, formats_.caption, note_l + 16.0f * s, yy, fr.right - 10.0f * s, 30.0f * s, pal.text3);
                    }
                    c.Icon(AiIcon::Document, fr.left + 10.0f * s, yy + 7.0f * s, 16.0f * s, pal.text2);
                    line(b.file, formats_.caption_b, fr.left + 34.0f * s, yy, note_l - 8.0f * s, 30.0f * s, pal.text);
                    yy = fr.bottom;
                }
                if (!btns.empty()) {
                    yy += 12.0f * s;
                    for (size_t i = 0; i < btns.size(); ++i) {
                        const AiErrButton& eb = b.actions[i];
                        const int hi = live ? index * 4 + static_cast<int>(i) : -1;
                        button(btns[i].x + btns[i].w, yy + btns[i].y, eb.text, eb.primary, eb.icon, eb.icon != AiIcon::Count,
                               AiPanelPart::ErrAction, hi);
                    }
                    if (em_right) line(em, formats_.meta, ir - em_w, yy, ir + 2.0f, 32.0f * s, pal.text3);
                    if (em_below) line(em, formats_.meta, tx, yy + 36.0f * s, ir, 18.0f * s, pal.text3);
                }
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Cut: {
            // .cut: h 34, padding 0 6 0 10, margin -2 0 12, radius 9, amber-bg,
            // amber 12.5; warn 15 gap 8; .t 600; .lnk h 26 padding 0 10 radius 7.
            place(-2.0f * s);
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + 34.0f * s);
            if (visible(r.top, r.bottom)) {
                c.Fill(r, 9.0f * s, pal.amber_bg);
                float x = r.left + 10.0f * s;
                c.Icon(AiIcon::Warn, x, r.top + 9.5f * s, 15.0f * s, pal.amber);
                x += 15.0f * s + 8.0f * s;
                // The link at the right first, so the words know their room.
                const float lw = b.accept.empty() ? 0.0f : 20.0f * s + 13.0f * s + 5.0f * s + c.Measure(b.accept, formats_.small125b);
                const D2D1_RECT_F lr = D2D1::RectF(r.right - 6.0f * s - lw, r.top + 4.0f * s, r.right - 6.0f * s, r.top + 30.0f * s);
                const float room = (lr.left - 8.0f * s);
                const float tw = c.Measure(b.title, formats_.small125b);
                line(b.title, formats_.small125b, x, r.top, room, 34.0f * s, pal.amber);
                x += tw + 8.0f * s;
                if (!b.sub.empty() && x + c.Measure(b.sub, formats_.small125) <= room)
                    line(b.sub, formats_.small125, x, r.top, room, 34.0f * s, pal.amber);
                if (lw > 0.0f) {
                    const int hi = b.gone_ms ? -1 : index;
                    const bool h = hot(AiPanelPart::CutRetry, hi), d = down(AiPanelPart::CutRetry, hi);
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    if (d && motion_on) {
                        const D2D1_POINT_2F mid = D2D1::Point2F((lr.left + lr.right) * 0.5f, (lr.top + lr.bottom) * 0.5f);
                        dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * before);
                    }
                    if (h) c.Fill(lr, 7.0f * s, Alpha(pal.amber, 0.14f));
                    c.Icon(AiIcon::Retry, lr.left + 10.0f * s, lr.top + 6.5f * s, 13.0f * s, pal.amber);
                    line(b.accept, formats_.small125b, lr.left + 28.0f * s, lr.top, lr.right, 26.0f * s, pal.amber);
                    dc->SetTransform(before);
                    if (hi >= 0) hit(lr, AiPanelPart::CutRetry, hi);
                }
            }
            y += 34.0f * s;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Stopped: {
            // .stopped: 12 text3, stop 14, gap 8, margin -2 0 14; the link in accent.
            place(-2.0f * s);
            const float sy = top + y;
            if (visible(sy, sy + 20.0f * s)) {
                float x = x0;
                c.Icon(AiIcon::Stop, x, sy + 3.0f * s, 14.0f * s, pal.text3);
                x += 14.0f * s + 8.0f * s;
                const float tw = c.Measure(b.title, formats_.caption);
                line(b.title, formats_.caption, x, sy, x + tw + 2.0f, 20.0f * s, pal.text3);
                x += tw;
                if (!b.accept.empty()) {
                    const float lw = c.Measure(b.accept, formats_.caption);
                    const D2D1_RECT_F lr = D2D1::RectF(x - 2.0f * s, sy, x + lw + 2.0f * s, sy + 20.0f * s);
                    const bool h = hot(AiPanelPart::StopContinue, index);
                    line(b.accept, formats_.caption, x, sy, x + lw + 2.0f, 20.0f * s, pal.accent);
                    if (h) c.Fill(D2D1::RectF(x, sy + 16.0f * s, x + lw, sy + 17.0f * s), 0.0f, pal.accent);
                    hit(lr, AiPanelPart::StopContinue, index);
                }
            }
            y += 20.0f * s;
            pending = 14.0f * s;
            break;
        }
        case AiBlockKind::Query: {
            // .qcard: padding 10 12 12, radius 12; .q-h 24; .q-chips margin
            // 9 0 10, gap 6; .q-res padding-top 10 over a line2 rule; .q-loc
            // margin-top 8, gap 6.
            place(0.0f);
            const float il = x0 + 1.0f * s + 12.0f * s;
            const float ir = x1 - 1.0f * s - 12.0f * s;
            // Chips (flex-wrap). A removed chip shrinks away (.qc.out: opacity
            // .2 s, scale .25 s, its width .35 s after .1 s).
            struct ChipPos { float x, y, w, full, alpha, scale; };
            std::vector<ChipPos> chips;
            float cx = il, cy = 0.0f;
            const auto chip_w = [&](const AiQueryChipView& q) {
                return 2.0f * s + 9.0f * s + c.Measure(q.key, formats_.caption_b) + 5.0f * s +
                       c.Measure(q.value, formats_.caption) + (q.fixed ? 9.0f * s : 5.0f * s + 18.0f * s + 4.0f * s);
            };
            for (const AiQueryChipView& q : b.chips) {
                const float full = chip_w(q);
                float w = full, alpha = 1.0f, scale = 1.0f;
                if (q.out_ms) {
                    const float t = motion_on ? static_cast<float>(now_ms - q.out_ms) : 1000.0f;
                    alpha = 1.0f - std::clamp(t / 200.0f, 0.0f, 1.0f);
                    scale = 1.0f - 0.3f * std::clamp(t / 250.0f, 0.0f, 1.0f);
                    const float k = EaseDesign(std::clamp((t - 100.0f) / 350.0f, 0.0f, 1.0f));
                    w = (full + 6.0f * s) * (1.0f - k) - 6.0f * s;
                    if (t < 450.0f) conv_anim_ = true;
                    if (w <= -6.0f * s + 0.5f) { chips.push_back({cx, cy, -6.0f * s, full, 0.0f, scale}); continue; }
                } else if (q.in_ms && motion_on) {
                    if (now_ms < q.in_ms) {
                        alpha = 0.0f;
                        scale = 0.9f;
                        conv_anim_ = true;
                    } else if (now_ms - q.in_ms < 250) {
                        const float t = static_cast<float>(now_ms - q.in_ms) / 250.0f;
                        alpha = t;
                        scale = 0.9f + 0.1f * CubicBezier(0.3f, 1.5f, 0.5f, 1.0f, t);
                        conv_anim_ = true;
                    }
                }
                if (cx > il && cx + w > ir) { cx = il; cy += 32.0f * s; }
                chips.push_back({cx, cy, w, full, alpha, scale});
                cx += w + 6.0f * s;
            }
            const float add_w = b.add.empty() ? 0.0f
                                              : 2.0f * s + 10.0f * s + 12.0f * s + 4.0f * s +
                                                    c.Measure(b.add, formats_.caption) + 10.0f * s;
            float add_x = cx, add_y = cy;
            if (add_w > 0.0f && add_x > il && add_x + add_w > ir) { add_x = il; add_y += 32.0f * s; }
            const float chips_h = (b.chips.empty() && add_w <= 0.0f) ? 0.0f
                                                                      : (add_w > 0.0f ? add_y : cy) + 26.0f * s;
            // Locations (flex-wrap).
            struct LocPos { float x, y, w; };
            std::vector<LocPos> locs;
            float lx = il, ly = 0.0f;
            for (const AiTagView& t : b.tags) {
                const float w = (6.0f + 16.0f + 6.0f) * s + c.Measure(t.text, formats_.caption) +
                                (t.bold.empty() ? 0.0f : 6.0f * s + c.Measure(t.bold, formats_.caption_b)) + 8.0f * s;
                if (lx > il && lx + w > ir) { lx = il; ly += 30.0f * s; }
                locs.push_back({lx, ly, (std::min)(w, ir - il)});
                lx += w + 6.0f * s;
            }
            const float locs_h = locs.empty() ? 0.0f : 8.0f * s + ly + 24.0f * s;
            // Result row: the re-search hint sits right-aligned after the size;
            // a narrow panel moves it to its own line under the size text.
            const float res_nw = c.Measure(b.count, formats_.num22);
            const float res_ux = il + (std::max)(res_nw, 14.0f * s) + 6.0f * s;
            const float res_uw = c.Measure(b.label, formats_.body13);
            const bool has_hint = b.re_ms && !b.hint.empty();
            const float hint_w = has_hint ? c.Measure(b.hint, formats_.heading) : 0.0f;
            const bool hint_wrap = has_hint && res_ux + res_uw + 12.0f * s + 16.0f * s + hint_w > ir;
            const float res_h = 26.0f * s + (hint_wrap ? 20.0f * s : 0.0f);
            const float h = 2.0f * s + 10.0f * s + 24.0f * s + 9.0f * s + chips_h + 10.0f * s +
                            1.0f * s + 10.0f * s + res_h + locs_h + 12.0f * s;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                // Header: search icon, 搜索条件, and the index time on the right.
                const float hy = r.top + 1.0f * s + 10.0f * s;
                c.Icon(AiIcon::Search, il, hy + 4.5f * s, 15.0f * s, pal.accent);
                const float tw = c.Measure(b.title, formats_.body13b);
                line(b.title, formats_.body13b, il + 23.0f * s, hy, il + 23.0f * s + tw + 1.0f, 24.0f * s, pal.text);
                if (!b.meta.empty()) {
                    const float mw = c.Measure(b.meta, formats_.meta);
                    const float mx = (std::max)(il + 23.0f * s + tw + 12.0f * s, ir - mw);
                    c.Icon(AiIcon::Bolt, mx - 17.0f * s, hy + 6.0f * s, 12.0f * s, pal.accent);
                    line(b.meta, formats_.meta, mx, hy, ir + 1.0f, 24.0f * s, pal.text3);
                }
                // Chips.
                const float chy = hy + 24.0f * s + 9.0f * s;
                for (size_t i = 0; i < b.chips.size(); ++i) {
                    const ChipPos& p = chips[i];
                    if (p.alpha <= 0.0f || p.w <= 0.0f) continue;
                    const AiQueryChipView& q = b.chips[i];
                    const D2D1_RECT_F cr = D2D1::RectF(p.x, chy + p.y, p.x + p.full, chy + p.y + 26.0f * s);
                    const D2D1_POINT_2F mid = D2D1::Point2F((cr.left + cr.right) * 0.5f, (cr.top + cr.bottom) * 0.5f);
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    const bool clip = p.w < p.full;
                    if (clip) dc->PushAxisAlignedClip(D2D1::RectF(cr.left, cr.top - 2.0f * s, cr.left + p.w, cr.bottom + 2.0f * s),
                                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    if (p.scale != 1.0f) dc->SetTransform(D2D1::Matrix3x2F::Scale(p.scale, p.scale, mid) * before);
                    const bool layer = p.alpha < 1.0f;
                    if (layer)
                        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                             D2D1::IdentityMatrix(), p.alpha, nullptr,
                                                             D2D1_LAYER_OPTIONS1_NONE),
                                      nullptr);
                    c.Fill(cr, 13.0f * s, pal.accent_tint);
                    c.Stroke(cr, 13.0f * s, pal.accent_soft);
                    float tx = cr.left + 1.0f * s + 9.0f * s;
                    const float kw = c.Measure(q.key, formats_.caption_b);
                    line(q.key, formats_.caption_b, tx, cr.top, tx + kw + 1.0f, 26.0f * s, pal.accent);
                    tx += kw + 5.0f * s;
                    const float vw = c.Measure(q.value, formats_.caption);
                    line(q.value, formats_.caption, tx, cr.top, tx + vw + 1.0f, 26.0f * s, pal.text);
                    tx += vw + 5.0f * s;
                    const int hi = index * 16 + static_cast<int>(i);
                    const D2D1_RECT_F xr = D2D1::RectF(tx, cr.top + 4.0f * s, tx + 18.0f * s, cr.top + 22.0f * s);
                    const bool xh = !q.out_ms && !q.fixed && hot(AiPanelPart::QueryChipRemove, hi);
                    if (xh) c.Fill(xr, 9.0f * s, Alpha(pal.accent, 0.16f));
                    if (!q.fixed)
                        c.Icon(AiIcon::Close, xr.left + 4.0f * s, xr.top + 4.0f * s, 10.0f * s, xh ? pal.text : pal.text3);
                    if (layer) dc->PopLayer();
                    dc->SetTransform(before);
                    if (clip) dc->PopAxisAlignedClip();
                    if (!q.out_ms && !q.fixed) hit(Inflate(xr, 3.0f * s), AiPanelPart::QueryChipRemove, hi);
                }
                if (add_w > 0.0f) {
                    // .qc.add: transparent, dashed line border, text3.
                    const D2D1_RECT_F ar = D2D1::RectF(add_x, chy + add_y, add_x + add_w, chy + add_y + 26.0f * s);
                    const bool ah = hot(AiPanelPart::QueryAdd, index);
                    if (ah) c.Fill(ar, 13.0f * s, pal.accent_tint);
                    ID2D1StrokeStyle* dash = nullptr;
                    if (ID2D1Factory* f = FactoryOf(dc)) {
                        const float dashes[] = {3.0f, 2.0f};
                        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                          D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f,
                                                                          D2D1_DASH_STYLE_CUSTOM, 0.0f),
                                             dashes, 2, &dash);
                        f->Release();
                    }
                    const float hw = 0.5f * s;
                    const D2D1_RECT_F sr = D2D1::RectF(ar.left + hw, ar.top + hw, ar.right - hw, ar.bottom - hw);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(sr, 13.0f * s - hw, 13.0f * s - hw),
                                             c.B(ah ? pal.accent : pal.line), 1.0f * s, dash);
                    if (dash) dash->Release();
                    const D2D1_COLOR_F ink = ah ? pal.text : pal.text3;
                    c.Icon(AiIcon::Plus, ar.left + 11.0f * s, ar.top + 7.0f * s, 12.0f * s, ink);
                    line(b.add, formats_.caption, ar.left + 27.0f * s, ar.top, ar.right, 26.0f * s, ink);
                    hit(ar, AiPanelPart::QueryAdd, index);
                }
                // Result line: the count (bumps when it changes), what and how
                // big, and 本机重新搜索… once a chip changed it.
                float yy = chy + chips_h + 10.0f * s;
                c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                yy += 1.0f * s + 10.0f * s;
                const float nw = res_nw;
                float bump = 1.0f;
                if (b.bump_ms && motion_on && now_ms >= b.bump_ms && now_ms - b.bump_ms < 450) {
                    const float t = static_cast<float>(now_ms - b.bump_ms) / 450.0f;
                    bump = t < 0.4f ? 1.0f + 0.28f * EaseDesign(t / 0.4f) : 1.28f - 0.28f * EaseDesign((t - 0.4f) / 0.6f);
                    conv_anim_ = true;
                }
                {
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    const D2D1_POINT_2F mid = D2D1::Point2F(il + nw * 0.5f, yy + 13.0f * s);
                    if (bump != 1.0f) dc->SetTransform(D2D1::Matrix3x2F::Scale(bump, bump, mid) * before);
                    line(b.count, formats_.num22, il, yy, il + nw + 2.0f, 26.0f * s, pal.text);
                    dc->SetTransform(before);
                }
                const float ux = res_ux;
                const float uw = res_uw;
                line(b.label, formats_.body13, ux, yy + 6.0f * s, ux + uw + 1.0f, 18.0f * s, pal.text2);
                if (has_hint) {
                    float ra = 1.0f, rdy = 0.0f;
                    if (motion_on && now_ms - b.re_ms < 300) {
                        const float t = now_ms < b.re_ms ? 0.0f : static_cast<float>(now_ms - b.re_ms) / 300.0f;
                        ra = EaseDesign(t);
                        rdy = 3.0f * s * (1.0f - ra);
                        conv_anim_ = true;
                    }
                    const float hx = hint_wrap ? ux + 16.0f * s : ir - hint_w;
                    const float hy2 = hint_wrap ? yy + 26.0f * s : yy;
                    const D2D1_COLOR_F ink = Alpha(pal.accent, ra);
                    c.Icon(AiIcon::Bolt, hx - 16.0f * s, hy2 + 7.0f * s + rdy, 12.0f * s, ink);
                    line(b.hint, formats_.heading, hx, hy2 + 4.0f * s + rdy, ir + 1.0f, 18.0f * s, ink);
                }
                yy += res_h;
                // Where the results are: folder pills with a count.
                if (!locs.empty()) {
                    yy += 8.0f * s;
                    const D2D1_COLOR_F chrome = Mix(pal.pane, pal.hover, 0.45f);
                    for (size_t i = 0; i < locs.size(); ++i) {
                        const AiTagView& t = b.tags[i];
                        const D2D1_RECT_F lr = D2D1::RectF(locs[i].x, yy + locs[i].y, locs[i].x + locs[i].w,
                                                           yy + locs[i].y + 24.0f * s);
                        c.Fill(lr, 7.0f * s, chrome);
                        c.Icon(AiIcon::FolderFilled, lr.left + 6.0f * s, lr.top + 4.0f * s, 16.0f * s, pal.folder_icon);
                        const float bw = t.bold.empty() ? 0.0f : c.Measure(t.bold, formats_.caption_b);
                        const float nx = lr.left + 28.0f * s;
                        const float nr = lr.right - 8.0f * s - (bw > 0.0f ? bw + 6.0f * s : 0.0f);
                        line(t.text, formats_.caption, nx, lr.top, nr, 24.0f * s, pal.text2);
                        if (bw > 0.0f) {
                            const float tw2 = (std::min)(c.Measure(t.text, formats_.caption), nr - nx);
                            const float bx = nx + tw2 + 6.0f * s;
                            line(t.bold, formats_.caption_b, bx, lr.top, bx + bw + 1.0f, 24.0f * s, pal.text);
                        }
                    }
                }
            }
            y += h;
            pending = 10.0f * s;
            break;
        }
        case AiBlockKind::Follow: {
            // .fu: flex-wrap gap 6, margin 0 0 12; .fl 11.5 text3 on its own
            // row (margin 0 0 1 2); chips h 30, padding 0 12 0 10, radius 15.
            place(0.0f);
            const float label_h = b.label.empty() ? 0.0f : 16.0f * s + 1.0f * s + 6.0f * s;
            struct FPos { float x, y, w; };
            std::vector<FPos> pos;
            float fx = x0, fy = 0.0f;
            for (const AiFollowView& f : b.follow) {
                const float w = 2.0f * s + 10.0f * s + 14.0f * s + 6.0f * s + c.Measure(f.text, formats_.small125) + 12.0f * s;
                if (fx > x0 && fx + w > x1) { fx = x0; fy += 36.0f * s; }
                pos.push_back({fx, fy, (std::min)(w, x1 - x0)});
                fx += w + 6.0f * s;
            }
            const float h = label_h + (pos.empty() ? 0.0f : fy + 30.0f * s);
            const float fy0 = top + y;
            if (visible(fy0, fy0 + h)) {
                if (!b.label.empty()) line(b.label, formats_.meta, x0 + 2.0f * s, fy0, x1, 16.0f * s, pal.text3);
                for (size_t i = 0; i < pos.size(); ++i) {
                    const AiFollowView& f = b.follow[i];
                    const int fi = index * 8 + static_cast<int>(i);
                    const D2D1_RECT_F fr = D2D1::RectF(pos[i].x, fy0 + label_h + pos[i].y, pos[i].x + pos[i].w,
                                                       fy0 + label_h + pos[i].y + 30.0f * s);
                    const bool fh = hot(AiPanelPart::FollowItem, fi), fd = down(AiPanelPart::FollowItem, fi);
                    D2D1_MATRIX_3X2_F before;
                    dc->GetTransform(&before);
                    if (fd && motion_on) {
                        const D2D1_POINT_2F mid = D2D1::Point2F((fr.left + fr.right) * 0.5f, (fr.top + fr.bottom) * 0.5f);
                        dc->SetTransform(D2D1::Matrix3x2F::Scale(0.95f, 0.95f, mid) * before);
                    }
                    c.Fill(fr, 15.0f * s, fh ? pal.accent_tint : pal.card);
                    c.Stroke(fr, 15.0f * s, fh ? pal.accent : pal.line);
                    c.Icon(f.icon, fr.left + 11.0f * s, fr.top + 8.0f * s, 14.0f * s, pal.accent);
                    line(f.text, formats_.small125, fr.left + 31.0f * s, fr.top, fr.right - 11.0f * s, 30.0f * s, pal.text);
                    dc->SetTransform(before);
                    hit(fr, AiPanelPart::FollowItem, fi);
                }
            }
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Rename: {
            // .card: .card-h 40 (icon 16 accent, title 13 semibold, .mu 12
            // text3, chevron); .card-b border-top line2, padding 6 0, holding
            // .rule (margin 6 12 6, padding 8 10, radius 9, chrome), the .rnr
            // rows (padding 5 12, lh 18; old .78fr | 14 | new 1.3fr, gap 8),
            // .rn-more (padding 3 12 7), .checks (border-top, padding 8 12 6,
            // gap 3) and .foot (padding 2 12 10).
            place(0.0f);
            const float il = x0 + 1.0f * s, ir = x1 - 1.0f * s;
            const float bl = il + 12.0f * s, br = ir - 12.0f * s;
            const D2D1_COLOR_F chrome = Mix(pal.pane, pal.hover, 0.45f);
            // The rule's pieces (flex-wrap, gap 4, 22 high): 规则, chips and
            // literal text, then 扩展名不变 pushed to the right.
            struct Piece { float x, y, w; int kind; size_t i; };   // 0 label, 1 chip, 2 literal, 3 note, 4 word
            std::vector<Piece> pieces;
            const float rx0 = bl + 10.0f * s, rx1 = br - 10.0f * s;
            float px = rx0, py = 0.0f;
            const auto put = [&](float w, int kind, size_t i, float gap) {
                if (px > rx0 && px + w > rx1) { px = rx0; py += 26.0f * s; }
                pieces.push_back({px, py, (std::min)(w, rx1 - rx0), kind, i});
                px += w + gap;
            };
            if (!b.rule_label.empty()) put(c.Measure(b.rule_label, formats_.meta), 0, 0, 10.0f * s);
            for (size_t i = 0; i < b.rule.size(); ++i) {
                const AiRuleToken& t = b.rule[i];
                if (t.token) put(c.Measure(t.text, formats_.caption_b) + 14.0f * s, 1, i, 4.0f * s);
                else if (t.word) put(c.Measure(t.text, formats_.meta), 4, i, 4.0f * s);
                else put(c.Measure(t.text, formats_.mono125), 2, i, 4.0f * s);
            }
            if (!b.hint.empty()) {
                const float w = c.Measure(b.hint, formats_.meta);
                if (px + w > rx1 + 0.5f) { px = rx0; py += 26.0f * s; }
                pieces.push_back({rx1 - w, py, w, 3, 0});
            }
            const float rule_h = 8.0f * s + py + 22.0f * s + 8.0f * s;
            std::vector<size_t> shown;
            for (size_t i = 0; i < b.rows.size(); ++i)
                if (b.open || b.rows[i].sample) shown.push_back(i);
            const std::wstring& toggle = b.open ? b.less : b.more;
            const float more_h = toggle.empty() ? 0.0f : 28.0f * s;
            const float checks_h =
                b.checks.empty() ? 0.0f
                                 : 1.0f * s + 8.0f * s + 18.0f * s * static_cast<float>(b.checks.size()) +
                                       3.0f * s * static_cast<float>(b.checks.size() - 1) + 6.0f * s;
            const float foot_h = b.sub.empty() ? 0.0f : 30.0f * s;
            float h = 2.0f * s + 40.0f * s;
            if (!b.collapsed)
                h += 1.0f * s + 6.0f * s + 6.0f * s + rule_h + 6.0f * s + 28.0f * s * static_cast<float>(shown.size()) + more_h + checks_h + foot_h +
                     6.0f * s;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const float col_k = TweenTo(key + 1, b.collapsed ? 1.0f : 0.0f, 250, now_ms);
            const D2D1_RECT_F hr = D2D1::RectF(il, r.top + 1.0f, ir, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                if (hot(AiPanelPart::RenameToggle, index)) {
                    dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
                    c.Fill(Inflate(r, -1.0f), 11.0f * s, Alpha(pal.hover, 0.6f));
                    dc->PopAxisAlignedClip();
                }
                const float cy = hr.top + 20.0f * s;
                c.Icon(AiIcon::Rename, hr.left + 12.0f * s, cy - 8.0f * s, 16.0f * s, pal.accent);
                float hx = hr.left + 36.0f * s;
                const float tw = c.Measure(b.label, formats_.body13b);
                line(b.label, formats_.body13b, hx, hr.top, hx + tw + 1.0f, 40.0f * s, pal.text);
                hx += tw + 8.0f * s;
                line(b.meta, formats_.caption, hx, hr.top, hr.right - 33.0f * s, 40.0f * s, pal.text3);
                chevron(hr.right - 25.0f * s, cy, -90.0f * col_k, pal.text3, AiIcon::ChevronDown);
                if (!b.collapsed) {
                    float yy = hr.bottom;
                    c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                    yy += 1.0f * s + 6.0f * s + 6.0f * s;
                    // .rule
                    const D2D1_RECT_F rb = D2D1::RectF(bl, yy, br, yy + rule_h);
                    c.Fill(rb, 9.0f * s, chrome);
                    for (const Piece& p : pieces) {
                        const float pyy = rb.top + 8.0f * s + p.y;
                        if (p.kind == 0) {
                            line(b.rule_label, formats_.meta, p.x, pyy, p.x + p.w + 1.0f, 22.0f * s, pal.text3);
                        } else if (p.kind == 3) {
                            line(b.hint, formats_.meta, p.x, pyy, p.x + p.w + 1.0f, 22.0f * s, pal.text3);
                        } else if (p.kind == 4) {
                            line(b.rule[p.i].text, formats_.meta, p.x, pyy, p.x + p.w + 1.0f, 22.0f * s, pal.text3);
                        } else if (p.kind == 1) {
                            const D2D1_RECT_F tr = D2D1::RectF(p.x, pyy, p.x + p.w, pyy + 22.0f * s);
                            c.Fill(tr, 6.0f * s, pal.accent_soft);
                            line(b.rule[p.i].text, formats_.caption_b, tr.left + 7.0f * s, tr.top, tr.right - 6.0f * s, 22.0f * s,
                                 pal.accent);
                        } else {
                            line(b.rule[p.i].text, formats_.mono125, p.x, pyy, p.x + p.w + 1.0f, 22.0f * s, pal.text);
                        }
                    }
                    yy = rb.bottom + 6.0f * s;
                    // .rnr rows
                    const float fr = (br - bl - 14.0f * s - 16.0f * s) / 2.08f;
                    const float old_r = bl + 0.78f * fr;
                    const float arrow_x = old_r + 8.0f * s;
                    const float nx0 = arrow_x + 14.0f * s + 8.0f * s;
                    for (size_t k : shown) {
                        const AiRenameRow& row = b.rows[k];
                        // .rnr: opacity 0 + translateY(4) until .in (.3 s).
                        float ra = 0.0f, rdy = 4.0f * s;
                        if (row.in_ms) {
                            const float t = motion_on ? std::clamp(static_cast<float>(now_ms - row.in_ms) / 300.0f, 0.0f, 1.0f) : 1.0f;
                            if (t < 1.0f) conv_anim_ = true;
                            const float e = CubicBezier(0.25f, 0.1f, 0.25f, 1.0f, t);
                            ra = e;
                            rdy = 4.0f * s * (1.0f - e);
                        }
                        if (ra > 0.0f && visible(yy, yy + 28.0f * s)) {
                            const bool rp = push_entrance({ra, rdy});
                            const float ty = yy + 5.0f * s;
                            // Old name: text3, struck through (55 % text3).
                            const float ow = (std::min)(c.Measure(row.old_name, formats_.small125), old_r - bl);
                            line(row.old_name, formats_.small125, bl, ty, old_r, 18.0f * s, pal.text3);
                            const float sy = std::round(ty + 9.5f * s);
                            c.Fill(D2D1::RectF(bl, sy, bl + ow, sy + (std::max)(1.0f, std::round(1.0f * s))), 0.0f,
                                   Alpha(pal.text3, 0.55f));
                            c.Icon(AiIcon::Arrow, arrow_x, ty + 3.0f * s, 12.0f * s, pal.text3);
                            // New name: semibold, token parts in accent, the
                            // extension regular text3, then the amber dot.
                            float nx = nx0;
                            const float nr = br - (row.warn ? 12.0f * s : 0.0f);
                            for (const AiRuleToken& part : row.parts) {
                                if (nx >= nr) break;
                                const float pw = c.Measure(part.text, formats_.small125b);
                                line(part.text, formats_.small125b, nx, ty, nr, 18.0f * s, part.token ? pal.accent : pal.text);
                                nx += pw;
                            }
                            if (nx < nr && !row.ext.empty()) {
                                const float ew = c.Measure(row.ext, formats_.small125);
                                line(row.ext, formats_.small125, nx, ty, nr, 18.0f * s, pal.text3);
                                nx += ew;
                            }
                            if (row.warn) {
                                const float dx = (std::min)(nx, nr) + 6.0f * s + 3.0f * s;
                                c.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dx, ty + 9.5f * s), 3.0f * s, 3.0f * s),
                                                  c.B(pal.amber));
                            }
                            pop_entrance(rp);
                        }
                        yy += 28.0f * s;
                    }
                    // .rn-more
                    if (!toggle.empty()) {
                        const float mw = 16.0f * s + c.Measure(toggle, formats_.caption_b);
                        const D2D1_RECT_F mr = D2D1::RectF(bl, yy + 3.0f * s, bl + mw, yy + 21.0f * s);
                        const bool mh = hot(AiPanelPart::RenameMore, index);
                        AiIconPose pose;
                        pose.rotate_deg = b.open ? 180.0f : 0.0f;
                        const D2D1_COLOR_F ink = mh ? Mix(pal.accent, pal.text, 0.25f) : pal.accent;
                        c.Icon(AiIcon::ChevronDown, mr.left, mr.top + 3.0f * s, 12.0f * s, ink, pose);
                        line(toggle, formats_.caption_b, mr.left + 16.0f * s, mr.top, mr.right + 1.0f, 18.0f * s, ink);
                        hit(Inflate(mr, 4.0f * s), AiPanelPart::RenameMore, index);
                        yy += 28.0f * s;
                    }
                    // .checks
                    if (!b.checks.empty()) {
                        c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                        yy += 1.0f * s + 8.0f * s;
                        for (size_t i = 0; i < b.checks.size(); ++i) {
                            const AiCheckView& ck = b.checks[i];
                            c.Icon(ck.warn ? AiIcon::Warn : AiIcon::Check, bl, yy + 2.5f * s, 13.0f * s, ck.warn ? pal.amber : pal.ok);
                            line(ck.text, formats_.caption, bl + 20.0f * s, yy, br, 18.0f * s, pal.text2);
                            yy += 18.0f * s + (i + 1 < b.checks.size() ? 3.0f * s : 0.0f);
                        }
                        yy += 6.0f * s;
                    }
                    // .foot
                    if (!b.sub.empty()) {
                        yy += 2.0f * s;
                        c.Icon(AiIcon::Bolt, bl, yy + 2.5f * s, 13.0f * s, pal.accent);
                        line(b.sub, formats_.meta, bl + 19.0f * s, yy, br, 18.0f * s, pal.text3);
                    }
                }
            }
            hit(hr, AiPanelPart::RenameToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Task: {
            // .card .plan: .card-h 40 (tasks icon), .card-b border-top line2.
            // .ps padding 9 12, gap 10: .pn 24 circle (600 12), .pt 13/600
            // lh 18 with .arg (12, hover, r 5, 0 6) and .tg (10.5/600 lh 17,
            // r 8, 0 6), .pd 12 text2 lh 18 margin-top 2, .pg 4 bar margin-top
            // 6 (running / paused). .ps+.ps::before: 2 px line at x 23, -9..9.
            // .p-sum: border-top line2, padding 9 12 10, 12 text2, items
            // (icon 13 accent, gap 5) wrap with gap 4 12; .sf (ok) on its own line.
            place(0.0f);
            const float il = x0 + 1.0f * s, ir = x1 - 1.0f * s;
            const float pl = il + 12.0f * s, pr = ir - 12.0f * s;
            const float mx = pl + 24.0f * s + 10.0f * s;   // .pm
            const float mw = (std::max)(1.0f, pr - mx);
            struct StepBox { float h, desc_h; bool bar; };
            std::vector<StepBox> boxes;
            float steps_h = 0.0f;
            for (const AiTaskStepView& st : b.task) {
                StepBox sb{};
                sb.desc_h = st.desc.empty() ? 0.0f : 2.0f * s + wrapped_h(st.desc, formats_.caption, mw, 18.0f * s);
                sb.bar = st.state == AiTaskState::Running || st.state == AiTaskState::Paused;
                sb.h = 9.0f * s + (std::max)(24.0f * s, 18.0f * s + sb.desc_h + (sb.bar ? 10.0f * s : 0.0f)) + 9.0f * s;
                boxes.push_back(sb);
                steps_h += sb.h;
            }
            // Summary items, flex-wrap.
            struct SumPos { float x, y, w; };
            std::vector<SumPos> sums;
            float sum_h = 0.0f;
            if (!b.sums.empty() || !b.safe.empty()) {
                float sx = pl, sy = 0.0f;
                for (const AiTaskSumView& it : b.sums) {
                    const float w = 13.0f * s + 5.0f * s + c.Measure(it.before, formats_.caption) +
                                    c.Measure(it.bold, formats_.caption_b) + c.Measure(it.after, formats_.caption);
                    if (sx > pl && sx + w > pr) { sx = pl; sy += 22.0f * s; }
                    sums.push_back({sx, sy, (std::min)(w, pr - pl)});
                    sx += w + 12.0f * s;
                }
                float lines_h = sums.empty() ? 0.0f : sy + 18.0f * s;
                if (!b.safe.empty()) lines_h += (sums.empty() ? 0.0f : 4.0f * s) + 18.0f * s;
                sum_h = 2.0f * s + 1.0f * s + 9.0f * s + lines_h + 10.0f * s;
            }
            float h = 2.0f * s + 40.0f * s;
            if (!b.collapsed) h += 1.0f * s + 6.0f * s + steps_h + sum_h;
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const float col_k = TweenTo(key + 1, b.collapsed ? 1.0f : 0.0f, 250, now_ms);
            const D2D1_RECT_F hr = D2D1::RectF(il, r.top + 1.0f, ir, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                if (hot(AiPanelPart::TaskToggle, index)) {
                    dc->PushAxisAlignedClip(hr, D2D1_ANTIALIAS_MODE_ALIASED);
                    c.Fill(Inflate(r, -1.0f), 11.0f * s, Alpha(pal.hover, 0.6f));
                    dc->PopAxisAlignedClip();
                }
                const float cy = hr.top + 20.0f * s;
                c.Icon(AiIcon::Tasks, hr.left + 12.0f * s, cy - 8.0f * s, 16.0f * s, pal.accent);
                float hx = hr.left + 36.0f * s;
                const float tw = c.Measure(b.label, formats_.body13b);
                line(b.label, formats_.body13b, hx, hr.top, hx + tw + 1.0f, 40.0f * s, pal.text);
                hx += tw + 8.0f * s;
                line(b.meta, formats_.caption, hx, hr.top, hr.right - 33.0f * s, 40.0f * s, pal.text3);
                chevron(hr.right - 25.0f * s, cy, -90.0f * col_k, pal.text3, AiIcon::ChevronDown);
                if (!b.collapsed) {
                    float yy = hr.bottom;
                    c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                    yy += 1.0f * s + 6.0f * s;
                    const D2D1_COLOR_F on_state = pal.on_accent;
                    for (size_t i = 0; i < b.task.size(); ++i) {
                        const AiTaskStepView& st = b.task[i];
                        const StepBox& sb = boxes[i];
                        const D2D1_RECT_F rowr = D2D1::RectF(il, yy, ir, yy + sb.h);
                        // State colours fade over .3 s (background / circle).
                        const float k = st.state_ms && motion_on
                                            ? std::clamp(static_cast<float>(now_ms - st.state_ms) / 300.0f, 0.0f, 1.0f)
                                            : 1.0f;
                        if (k < 1.0f) conv_anim_ = true;
                        if (st.state == AiTaskState::Running) c.Fill(rowr, 0.0f, Alpha(pal.accent_tint, k));
                        else if (st.state == AiTaskState::Paused) c.Fill(rowr, 0.0f, Alpha(pal.amber_bg, k));
                        if (i > 0)
                            c.Fill(D2D1::RectF(pl + 11.0f * s, yy - 9.0f * s, pl + 13.0f * s, yy + 9.0f * s), 0.0f, pal.line);
                        const float py = yy + 9.0f * s;
                        const D2D1_POINT_2F mid = D2D1::Point2F(pl + 12.0f * s, py + 12.0f * s);
                        D2D1_COLOR_F circle = pal.hover, ink = pal.text2;
                        if (st.state == AiTaskState::Done) { circle = pal.ok; ink = on_state; }
                        else if (st.state == AiTaskState::Running) { circle = pal.accent; ink = on_state; }
                        else if (st.state == AiTaskState::Paused) { circle = pal.amber; ink = on_state; }
                        if (k < 1.0f) {
                            circle = Mix(pal.hover, circle, k);
                            ink = Mix(pal.text2, ink, k);
                        }
                        c.dc->FillEllipse(D2D1::Ellipse(mid, 12.0f * s, 12.0f * s), c.B(circle));
                        if (st.state == AiTaskState::Done) {
                            c.Icon(AiIcon::Check, mid.x - 6.5f * s, mid.y - 6.5f * s, 13.0f * s, ink);
                        } else {
                            TextOptions o;
                            o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
                            o.ellipsis = false;
                            const std::wstring n = st.state == AiTaskState::Paused ? std::wstring(L"!") : std::to_wstring(i + 1);
                            c.Text(n, formats_.caption_b, D2D1::RectF(mid.x - 12.0f * s, mid.y - 9.0f * s, mid.x + 12.0f * s, mid.y + 9.0f * s), ink, o);
                        }
                        // .pt: title, then the arg pill and the tag on the same line.
                        float tx2 = mx;
                        const float ttw = (std::min)(c.Measure(st.title, formats_.body13b), mw);
                        line(st.title, formats_.body13b, tx2, py, mx + mw, 18.0f * s, pal.text);
                        tx2 += ttw + 6.0f * s;
                        if (!st.arg.empty() && tx2 + 30.0f * s < pr) {
                            const float aw = (std::min)(c.Measure(st.arg, formats_.caption) + 12.0f * s, pr - tx2);
                            const D2D1_RECT_F ar = D2D1::RectF(tx2, py, tx2 + aw, py + 18.0f * s);
                            c.Fill(ar, 5.0f * s, pal.hover);
                            line(st.arg, formats_.caption, ar.left + 6.0f * s, py, ar.right - 4.0f * s, 18.0f * s, pal.text2);
                            tx2 += aw + 6.0f * s;
                        }
                        if (!st.tag.empty() && tx2 + 30.0f * s < pr) {
                            const float gw = (std::min)(c.Measure(st.tag, formats_.badge) + 12.0f * s, pr - tx2);
                            const D2D1_RECT_F gr = D2D1::RectF(tx2, py + 0.5f * s, tx2 + gw, py + 17.5f * s);
                            const bool ok_tag = !st.tag_warn;
                            c.Fill(gr, 8.0f * s, ok_tag ? pal.accent_soft : pal.amber_bg);
                            line(st.tag, formats_.badge, gr.left + 6.0f * s, gr.top, gr.right - 4.0f * s, 17.0f * s,
                                 ok_tag ? pal.ok : pal.amber);
                        }
                        float dy = py + 18.0f * s;
                        if (!st.desc.empty()) {
                            wrapped(st.desc, formats_.caption, mx, dy + 2.0f * s, mw, 18.0f * s, pal.text2);
                            dy += sb.desc_h;
                        }
                        if (sb.bar) {
                            const D2D1_RECT_F br2 = D2D1::RectF(mx, dy + 6.0f * s, mx + mw, dy + 10.0f * s);
                            c.Fill(br2, 2.0f * s, pal.line);
                            const float p = std::clamp(TweenTo(key + 8 + static_cast<int>(i), st.progress, 250, now_ms), 0.0f, 1.0f);
                            if (p > 0.0f)
                                c.Fill(D2D1::RectF(br2.left, br2.top, br2.left + (br2.right - br2.left) * p, br2.bottom), 2.0f * s,
                                       st.state == AiTaskState::Paused ? pal.amber : pal.accent);
                        }
                        yy += sb.h;
                    }
                    if (sum_h > 0.0f) {
                        yy += 2.0f * s;
                        c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                        yy += 1.0f * s + 9.0f * s;
                        for (size_t i = 0; i < sums.size(); ++i) {
                            const AiTaskSumView& it = b.sums[i];
                            const float sy = yy + sums[i].y;
                            const float right = sums[i].x + sums[i].w;
                            c.Icon(it.icon, sums[i].x, sy + 2.5f * s, 13.0f * s, pal.accent);
                            float sx = sums[i].x + 18.0f * s;
                            const float w1 = c.Measure(it.before, formats_.caption);
                            line(it.before, formats_.caption, sx, sy, right, 18.0f * s, pal.text2);
                            sx += w1;
                            const float w2 = c.Measure(it.bold, formats_.caption_b);
                            line(it.bold, formats_.caption_b, sx, sy, right, 18.0f * s, pal.text);
                            sx += w2;
                            line(it.after, formats_.caption, sx, sy, right + 1.0f, 18.0f * s, pal.text2);
                        }
                        if (!b.safe.empty()) {
                            const float sy = yy + (sums.empty() ? 0.0f : sums.back().y + 18.0f * s + 4.0f * s);
                            c.Icon(AiIcon::Shield, pl, sy + 2.5f * s, 13.0f * s, pal.ok);
                            line(b.safe, formats_.caption, pl + 17.0f * s, sy, pr, 18.0f * s, pal.ok);
                        }
                    }
                }
            }
            hit(hr, AiPanelPart::TaskToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Disk: {
            // .card: .card-h 40 (hdd icon, .mu with the bolt), .card-b
            // border-top line2; .dk padding 2 12 12: .dk-top (20/600 used,
            // 12 text2, the free figure amber 600 at the right), .dk-bar
            // (10 high, r 5, margin 8 0 7), .dk-leg (11.5 text2, gap 3 12,
            // 8 px swatches), .tl-h (11.5/600 text3, margin 12 0 2) and the
            // .tl rows (36 high; 18 | 1fr | 58 | 58, gap 8).
            place(0.0f);
            const AiDiskView& d = b.disk;
            const float il = x0 + 1.0f * s, ir = x1 - 1.0f * s;
            const float bl = il + 12.0f * s, br = ir - 12.0f * s;
            const float count_k = Count(d.count_ms, 900);
            const float used_now = d.used_from + (d.used_gb - d.used_from) * count_k;
            const float free_now = d.free_from + (d.free_gb - d.free_from) * count_k;
            // Legend items, flex-wrap.
            struct Leg { float x, y, w; int kind; std::wstring text; };
            std::vector<Leg> legs;
            float lx = bl, ly = 0.0f;
            const auto leg = [&](int kind, std::wstring text) {
                const float w = 13.0f * s + c.Measure(text, formats_.meta);
                if (lx > bl && lx + w > br) { lx = bl; ly += 19.0f * s; }
                legs.push_back({lx, ly, w, kind, std::move(text)});
                lx += w + 12.0f * s;
            };
            for (int i = 0; i < 4; ++i)
                if (!d.legend[i].empty()) leg(i, d.legend[i]);
            const float rb_k = TweenTo(XKey(b.id, 5), d.seg[4], 700, now_ms);
            if (rb_k > 0.001f && !d.legend_recycle.empty()) leg(4, d.legend_recycle);
            if (!d.legend_free.empty()) leg(5, d.legend_free + L" " + Fixed1(free_now));
            const float leg_h = legs.empty() ? 0.0f : ly + 16.0f * s;
            const float rows_h = d.rows.empty() ? 0.0f : 12.0f * s + 16.0f * s + 2.0f * s + 36.0f * s * static_cast<float>(d.rows.size());
            const float body_h = 1.0f * s + 2.0f * s + 26.0f * s + 8.0f * s + 10.0f * s + 7.0f * s + leg_h + rows_h + 12.0f * s;
            const float h = 2.0f * s + 40.0f * s + (b.collapsed ? 0.0f : body_h);
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const D2D1_RECT_F hr = D2D1::RectF(il, r.top + 1.0f, ir, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                card_head(r, hr, AiIcon::Hdd, b.label, b.meta, b.hint_icon, b.collapsed, AiPanelPart::DiskToggle, index, key);
                if (!b.collapsed) {
                    float yy = hr.bottom;
                    c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                    yy += 1.0f * s + 2.0f * s;
                    // .dk-top, baseline aligned at yy + 20.
                    const std::wstring used_txt = Trim0(Fixed1(used_now));
                    const float uw = c.Measure(used_txt, formats_.num20);
                    line(used_txt, formats_.num20, bl, yy, bl + uw + 2.0f * s, 26.0f * s, pal.text);
                    const D2D1_COLOR_F free_ink = d.free_ok ? Mix(pal.amber, pal.ok, Count(d.count_ms, 400)) : pal.amber;
                    const std::wstring free_txt = Fixed1(free_now);
                    const float fuw = c.Measure(d.free_unit, formats_.caption_b);
                    const float fnw = c.Measure(free_txt, formats_.num14);
                    const float fx = br - fuw - 4.0f * s - fnw;
                    line(free_txt, formats_.num14, fx, yy + 6.1f * s, fx + fnw + 1.0f, 18.0f * s, free_ink);
                    line(d.free_unit, formats_.caption_b, br - fuw, yy + 7.8f * s, br + 1.0f, 16.0f * s, free_ink);
                    line(d.used_of, formats_.caption, bl + uw + 6.0f * s, yy + 7.8f * s, fx - 8.0f * s, 16.0f * s, pal.text2);
                    yy += 26.0f * s + 8.0f * s;
                    // .dk-bar: segments in a rounded clip, 1 px card gaps.
                    const D2D1_RECT_F bar = D2D1::RectF(bl, yy, br, yy + 10.0f * s);
                    c.Fill(bar, 5.0f * s, pal.line2);
                    c.Stroke(Inflate(bar, -0.5f), 4.5f * s, pal.line);
                    const float grow = Count(d.grow_ms, 700);
                    const D2D1_COLOR_F seg_ink[4] = {pal.sys, pal.tint_fg[static_cast<int>(AiTint::Document)], pal.accent, pal.line};
                    float sx = bar.left;
                    if (PushRounded(c, bar, 5.0f * s)) {
                        for (int i = 0; i < 5; ++i) {
                            const float f = i == 4 ? rb_k : TweenTo(XKey(b.id, i), d.seg[i], 700, now_ms);
                            const float w = (bar.right - bar.left) * f * grow;
                            if (w <= 0.0f) continue;
                            const D2D1_RECT_F sr = D2D1::RectF(sx, bar.top, sx + w, bar.bottom);
                            if (i == 4) Hatch(c, sr, pal.amber, 3.0f * s);
                            else c.Fill(sr, 0.0f, seg_ink[i]);
                            if (sx > bar.left) c.Fill(D2D1::RectF(sx - 1.0f * s, bar.top, sx, bar.bottom), 0.0f, pal.card);
                            sx += w;
                        }
                        dc->PopLayer();
                    }
                    yy += 10.0f * s + 7.0f * s;
                    for (const Leg& lg : legs) {
                        const float ly2 = yy + lg.y;
                        const D2D1_RECT_F sw = D2D1::RectF(lg.x, ly2 + 4.0f * s, lg.x + 8.0f * s, ly2 + 12.0f * s);
                        if (lg.kind < 4) c.Fill(sw, 2.0f * s, seg_ink[lg.kind]);
                        else if (lg.kind == 4) {
                            c.Fill(sw, 2.0f * s, Alpha(pal.amber, 0.35f));
                            if (PushRounded(c, sw, 2.0f * s)) { Hatch(c, sw, pal.amber, 2.0f * s); dc->PopLayer(); }
                        } else {
                            c.Fill(sw, 2.0f * s, pal.line2);
                            c.Stroke(Inflate(sw, -0.5f), 1.5f * s, pal.line);
                        }
                        line(lg.text, formats_.meta, lg.x + 13.0f * s, ly2, lg.x + lg.w + 1.0f, 16.0f * s, pal.text2);
                    }
                    yy += leg_h;
                    if (!d.rows.empty()) {
                        yy += 12.0f * s;
                        line(d.top_label, formats_.heading, bl, yy, br, 16.0f * s, pal.text3);
                        yy += 16.0f * s + 2.0f * s;
                        const float vx = br - 58.0f * s;
                        const float bx1 = vx - 8.0f * s, bx0 = bx1 - 58.0f * s;
                        const float nx = bl + 18.0f * s + 8.0f * s;
                        for (size_t i = 0; i < d.rows.size(); ++i) {
                            const AiDiskRowView& row = d.rows[i];
                            // .tl.in: fades up; its bar grows .6 s.
                            const uint64_t in_ms = d.grow_ms ? d.grow_ms + 250 + 110 * i : 0;
                            const float ik = Count(in_ms, 300);
                            if (ik > 0.0f && visible(yy, yy + 36.0f * s)) {
                                const bool rp = push_entrance({ik, 4.0f * s * (1.0f - ik)});
                                const float cy = yy + 18.0f * s;
                                if (row.file) c.Icon(AiIcon::File, bl + 1.0f * s, cy - 8.0f * s, 16.0f * s, pal.text3);
                                else c.Icon(AiIcon::FolderFilled, bl + 1.0f * s, cy - 8.0f * s, 16.0f * s, pal.folder_icon);
                                line(row.name, formats_.small125b, nx, yy + 2.0f * s, bx0 - 8.0f * s, 16.0f * s, pal.text);
                                line(row.sub, formats_.small11, nx, yy + 18.0f * s, bx0 - 8.0f * s, 16.0f * s, pal.text3);
                                const D2D1_RECT_F bb = D2D1::RectF(bx0, cy - 2.5f * s, bx1, cy + 2.5f * s);
                                c.Fill(bb, 2.5f * s, pal.line2);
                                const float bk = Count(in_ms ? in_ms + 16 : 0, 600);
                                const float bw = (bb.right - bb.left) * std::clamp(row.frac, 0.0f, 1.0f) * bk;
                                if (bw > 0.5f) c.Fill(D2D1::RectF(bb.left, bb.top, bb.left + bw, bb.bottom), 2.5f * s, row.sys ? pal.sys : pal.accent);
                                const float gbw = c.Measure(L" GB", formats_.small11);
                                const float vw = c.Measure(row.value, formats_.small125b);
                                line(row.value, formats_.small125b, br - gbw - vw, yy, br - gbw + 1.0f, 36.0f * s, pal.text);
                                line(L" GB", formats_.small11, br - gbw, yy + 0.5f * s, br + 1.0f, 36.0f * s, pal.text3);
                                pop_entrance(rp);
                            }
                            yy += 36.0f * s;
                        }
                    }
                }
            }
            hit(hr, AiPanelPart::DiskToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Clean: {
            // .card: .card-h 40 (sliders), then the .cl-row categories
            // (border-top line2, padding 9 12, gap 10: .ck 16 r 4 margin-top
            // 2, .cl-ic 24 r 7 tinted, .cl-t 13/600 lh 18 with the numbers
            // right, .cl-d 12 text2 lh 17 with 查看), .cl-files (.cf 26 high,
            // first margin-top 8), .cl-sum (border-top line, padding 10 12,
            // pane, 12.5 text2, 15/600 numbers, .sf 11.5 text3) and .cl-note
            // (pane, padding 0 12 10, 11.5 lh 16 text3).
            place(0.0f);
            const AiCleanView& cv = b.clean;
            const float il = x0 + 1.0f * s, ir = x1 - 1.0f * s;
            const float bl = il + 12.0f * s, br = ir - 12.0f * s;
            const float mx = bl + 16.0f * s + 10.0f * s + 24.0f * s + 10.0f * s;   // .cl-m
            std::vector<float> files_h(cv.rows.size(), 0.0f);
            float rows_h = 0.0f;
            for (size_t i = 0; i < cv.rows.size(); ++i) {
                const AiCleanRowView& row = cv.rows[i];
                const float full = row.files.empty() ? 0.0f
                                                     : 8.0f * s - 2.0f * s + 28.0f * s * static_cast<float>(row.files.size()) +
                                                           (row.more.empty() ? 0.0f : 28.0f * s);
                files_h[i] = full * TweenTo(XKey(b.id, 16 + static_cast<int>(i)), row.open ? 1.0f : 0.0f, 350, now_ms);
                rows_h += 1.0f * s + 9.0f * s + 18.0f * s + 2.0f * s + 17.0f * s + files_h[i] + 9.0f * s;
            }
            const float note_w = br - (bl + 20.0f * s);
            const float note_h = cv.note.empty() ? 0.0f : wrapped_h(cv.note, formats_.meta, note_w, 16.0f * s) + 10.0f * s;
            // 移到回收站，可撤销 sits right of the total, or on its own line
            // below it when the panel is too narrow; it is never dropped.
            const float clean_sum_w = c.Measure(cv.sum_pre + L" ", formats_.small125) + c.Measure(cv.sum_n, formats_.num15) +
                                      c.Measure(L" " + cv.sum_mid + L" ", formats_.small125) +
                                      c.Measure(cv.sum_gb, formats_.num15) + c.Measure(L" " + cv.sum_unit, formats_.small125);
            const float clean_safe_w = cv.safe.empty() ? 0.0f : c.Measure(cv.safe, formats_.meta) + 17.0f * s;
            const bool clean_safe_below = !cv.safe.empty() && br - clean_safe_w <= bl + clean_sum_w + 8.0f * s;
            const float sum_h = 1.0f * s + 10.0f * s + 20.0f * s + 10.0f * s + (clean_safe_below ? 20.0f * s : 0.0f);
            const float h = 2.0f * s + 40.0f * s + (b.collapsed ? 0.0f : rows_h + sum_h + note_h);
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const D2D1_RECT_F hr = D2D1::RectF(il, r.top + 1.0f, ir, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                card_head(r, hr, AiIcon::Sliders, b.label, b.meta, AiIcon::Count, b.collapsed, AiPanelPart::CleanToggle, index, key);
                if (!b.collapsed) {
                    float yy = hr.bottom;
                    for (size_t i = 0; i < cv.rows.size(); ++i) {
                        const AiCleanRowView& row = cv.rows[i];
                        const int ri = index * 32 + static_cast<int>(i);
                        const float rh = 1.0f * s + 9.0f * s + 18.0f * s + 2.0f * s + 17.0f * s + files_h[i] + 9.0f * s;
                        const float ik = row.in_ms ? Count(row.in_ms, 300) : 1.0f;
                        if (ik > 0.0f && visible(yy, yy + rh)) {
                            const bool rp = push_entrance({ik, 4.0f * s * (1.0f - ik)});
                            const D2D1_RECT_F rowr = D2D1::RectF(il, yy + 1.0f * s, ir, yy + rh);
                            const float hot_k = TweenTo(XKey(b.id, 48 + static_cast<int>(i)), row.open ? 1.0f : 0.0f, 300, now_ms);
                            if (hot_k > 0.0f) c.Fill(rowr, 0.0f, Alpha(pal.accent_tint, hot_k));
                            c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                            const float ty = yy + 1.0f * s + 9.0f * s;
                            // .ck
                            const D2D1_RECT_F ck = D2D1::RectF(bl, ty + 2.0f * s, bl + 16.0f * s, ty + 18.0f * s);
                            check_box(ck, row.on, hot(AiPanelPart::CleanCheck, ri), down(AiPanelPart::CleanCheck, ri), 11.0f * s);
                            hit(Inflate(ck, 5.0f * s), AiPanelPart::CleanCheck, ri);
                            // .cl-ic
                            const int ti = static_cast<int>(row.tint);
                            const float icx = bl + 16.0f * s + 10.0f * s;
                            c.Fill(D2D1::RectF(icx, ty, icx + 24.0f * s, ty + 24.0f * s), 7.0f * s, pal.tint_bg[ti]);
                            c.Icon(row.icon, icx + 5.0f * s, ty + 5.0f * s, 14.0f * s, pal.tint_fg[ti]);
                            // .cl-t: title, tag, then the numbers at the right.
                            const float bump = Bump(row.bump_ms);
                            const std::wstring sep = L" " + cv.unit_n + L" \u00B7 ";
                            const float w_unit = c.Measure(L" " + cv.unit_gb, formats_.meta);
                            const float w_size = c.Measure(row.size, formats_.body13b);
                            const float w_sep = c.Measure(sep, formats_.meta);
                            const float w_cnt = c.Measure(row.count, formats_.body13b);
                            const float nw = w_cnt + w_sep + w_size + w_unit;
                            float nx = br - nw;
                            const D2D1_COLOR_F num_ink = row.on ? pal.text : pal.text3;
                            {
                                D2D1_MATRIX_3X2_F before;
                                dc->GetTransform(&before);
                                if (bump > 0.0f)
                                    dc->SetTransform(D2D1::Matrix3x2F::Scale(1.0f + 0.08f * bump, 1.0f + 0.08f * bump,
                                                                             D2D1::Point2F(br - nw * 0.5f, ty + 9.0f * s)) * before);
                                line(row.count, formats_.body13b, nx, ty, nx + w_cnt + 1.0f, 18.0f * s, num_ink);
                                nx += w_cnt;
                                line(sep, formats_.meta, nx, ty + 0.5f * s, nx + w_sep + 1.0f, 18.0f * s, pal.text3);
                                nx += w_sep;
                                line(row.size, formats_.body13b, nx, ty, nx + w_size + 1.0f, 18.0f * s, num_ink);
                                nx += w_size;
                                line(L" " + cv.unit_gb, formats_.meta, nx, ty + 0.5f * s, nx + w_unit + 1.0f, 18.0f * s, pal.text3);
                                dc->SetTransform(before);
                            }
                            const float title_r = br - nw - 8.0f * s;
                            // 看一眼 keeps its place in a narrow panel: the title
                            // gives way (ellipsis) while it keeps about 4 characters.
                            const float tag_w = row.tag.empty() ? 0.0f : c.Measure(row.tag, formats_.badge) + 12.0f * s;
                            const bool tag_fits = tag_w > 0.0f && title_r - mx >= tag_w + 6.0f * s + 56.0f * s;
                            const float title_max = tag_fits ? title_r - mx - tag_w - 6.0f * s : title_r - mx;
                            const float tw = (std::min)(c.Measure(row.title, formats_.body13b), title_max);
                            line(row.title, formats_.body13b, mx, ty, mx + title_max, 18.0f * s, row.on ? pal.text : pal.text2);
                            if (tag_fits) {
                                const float gx = mx + tw + 6.0f * s;
                                const float gw = tag_w;
                                const D2D1_RECT_F gr = D2D1::RectF(gx, ty + 0.5f * s, gx + gw, ty + 17.5f * s);
                                c.Fill(gr, 8.0f * s, pal.amber_bg);
                                line(row.tag, formats_.badge, gr.left + 6.0f * s, gr.top, gr.right - 4.0f * s, 17.0f * s, pal.amber);
                            }
                            // .cl-d: the reason, then 查看 (the reason gives way).
                            const float dy = ty + 18.0f * s + 2.0f * s;
                            const float lkw = row.link.empty() ? 0.0f : c.Measure(row.link, formats_.caption_b) + 4.0f * s;
                            const float dw = (std::min)(c.Measure(row.desc, formats_.caption), (std::max)(0.0f, br - mx - lkw - 2.0f * s));
                            line(row.desc, formats_.caption, mx, dy, mx + dw + 1.0f, 17.0f * s, pal.text2);
                            if (!row.link.empty()) {
                                const D2D1_RECT_F lr = D2D1::RectF(mx + dw + 2.0f * s, dy, mx + dw + 2.0f * s + lkw, dy + 17.0f * s);
                                if (hot(AiPanelPart::CleanLink, ri)) c.Fill(lr, 4.0f * s, pal.accent_tint);
                                line(row.link, formats_.caption_b, lr.left + 2.0f * s, dy, lr.right, 17.0f * s, pal.accent);
                                hit(Inflate(lr, 3.0f * s), AiPanelPart::CleanLink, ri);
                            }
                            // .cl-files
                            if (files_h[i] > 0.5f) {
                                const float fy0 = dy + 17.0f * s;
                                const D2D1_RECT_F clip = D2D1::RectF(il, fy0, ir, fy0 + files_h[i]);
                                dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
                                float fy = fy0 + 8.0f * s - 2.0f * s;
                                for (size_t k = 0; k < row.files.size(); ++k) {
                                    const AiCleanFileView& f = row.files[k];
                                    const int fi = ri * 32 + static_cast<int>(k);
                                    const float frow = fy + 2.0f * s;
                                    const D2D1_RECT_F fr2 = D2D1::RectF(mx - 4.0f * s, frow, br + 4.0f * s, frow + 26.0f * s);
                                    if (hot(AiPanelPart::CleanFile, fi)) c.Fill(fr2, 6.0f * s, pal.hover);
                                    const float fc = frow + 13.0f * s;
                                    check_box(D2D1::RectF(mx, fc - 7.0f * s, mx + 14.0f * s, fc + 7.0f * s), f.on, false,
                                              down(AiPanelPart::CleanFile, fi), 10.0f * s);
                                    c.Icon(f.icon, mx + 14.0f * s + 8.0f * s, fc - 7.5f * s, 15.0f * s, pal.tint_fg[static_cast<int>(f.tint)]);
                                    const float szw = c.Measure(f.size, formats_.caption);
                                    float right = br - szw;
                                    line(f.size, formats_.caption, right, frow, br + 1.0f, 26.0f * s, pal.text3);
                                    right -= 8.0f * s;
                                    const float keep_k = TweenTo(XKey(b.id, 64 + static_cast<int>(i) * 32 + static_cast<int>(k)), f.on ? 0.0f : 1.0f, 250, now_ms);
                                    if (keep_k > 0.0f && !cv.keep.empty()) {
                                        const float kw = c.Measure(cv.keep, formats_.badge);
                                        line(cv.keep, formats_.badge, right - kw, frow, right + 1.0f, 26.0f * s, Alpha(pal.accent, keep_k));
                                    }
                                    right -= cv.keep.empty() ? 0.0f : (c.Measure(cv.keep, formats_.badge) + 8.0f * s) * keep_k;
                                    const float nx2 = mx + 14.0f * s + 8.0f * s + 15.0f * s + 8.0f * s;
                                    line(f.name, formats_.caption, nx2, frow, right, 26.0f * s, f.on ? pal.text : pal.text3);
                                    hit(Intersect(fr2, clip), AiPanelPart::CleanFile, fi);
                                    fy += 28.0f * s;
                                }
                                if (!row.more.empty())
                                    line(row.more, formats_.caption, mx + 22.0f * s, fy + 2.0f * s, br, 26.0f * s, pal.text3);
                                dc->PopAxisAlignedClip();
                            }
                            pop_entrance(rp);
                        }
                        yy += rh;
                    }
                    // .cl-sum + .cl-note on the pane, rounded at the bottom.
                    FillCorners(c, D2D1::RectF(il, yy, ir, r.bottom - 1.0f), 0.0f, 0.0f, 11.0f * s, 11.0f * s, pal.pane);
                    c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line);
                    yy += 1.0f * s + 10.0f * s;
                    {
                        const float bump = Bump(cv.sum_bump_ms);
                        float sx = bl;
                        const auto part = [&](const std::wstring& t, bool num) {
                            IDWriteTextFormat* fmt = num ? formats_.num15 : formats_.small125;
                            const float w = c.Measure(t, fmt);
                            D2D1_MATRIX_3X2_F before;
                            dc->GetTransform(&before);
                            if (num && bump > 0.0f)
                                dc->SetTransform(D2D1::Matrix3x2F::Scale(1.0f + 0.08f * bump, 1.0f + 0.08f * bump,
                                                                         D2D1::Point2F(sx + w * 0.5f, yy + 10.0f * s)) * before);
                            line(t, fmt, sx, yy + (num ? 0.0f : 1.0f * s), sx + w + 1.0f, 20.0f * s, num ? pal.text : pal.text2);
                            dc->SetTransform(before);
                            sx += w;
                        };
                        part(cv.sum_pre + L" ", false);
                        part(cv.sum_n, true);
                        part(L" " + cv.sum_mid + L" ", false);
                        part(cv.sum_gb, true);
                        part(L" " + cv.sum_unit, false);
                        const float sfw = c.Measure(cv.safe, formats_.meta);
                        if (clean_safe_below) {
                            c.Icon(AiIcon::Shield, bl, yy + 20.0f * s + 4.0f * s, 12.0f * s, pal.accent);
                            line(cv.safe, formats_.meta, bl + 17.0f * s, yy + 20.0f * s, br + 1.0f, 20.0f * s, pal.text3);
                        } else if (!cv.safe.empty()) {
                            c.Icon(AiIcon::Shield, br - sfw - 17.0f * s, yy + 4.0f * s, 12.0f * s, pal.accent);
                            line(cv.safe, formats_.meta, br - sfw, yy, br + 1.0f, 20.0f * s, pal.text3);
                        }
                    }
                    yy += 20.0f * s + 10.0f * s + (clean_safe_below ? 20.0f * s : 0.0f);
                    if (!cv.note.empty()) {
                        c.Icon(AiIcon::Shield, bl, yy + 2.0f * s, 13.0f * s, pal.text3);
                        wrapped(cv.note, formats_.meta, bl + 20.0f * s, yy, note_w, 16.0f * s, pal.text3);
                    }
                }
            }
            hit(hr, AiPanelPart::CleanToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Funnel: {
            // .card: .card-h 40 (dup, .mu bolt 本机 · 9.7 秒), .fnl padding 4 0 6
            // with three .fs rows (padding 7 12; .pn 24 circle over two lines;
            // .lb 12.5/600 + .num 13/600 right lh 18; .sub + .tm 11.5 text3 lh
            // 16; .bar 6 r 3 margin-top 5), then .fnl-f (border-top line2,
            // padding 8 12 10, shield 13 accent, 11.5 text3).
            place(0.0f);
            const AiFunnelView& fv = b.funnel;
            const float il = x0 + 1.0f * s, ir = x1 - 1.0f * s;
            const float bl = il + 12.0f * s, br = ir - 12.0f * s;
            const float row_h = 7.0f * s + 18.0f * s + 16.0f * s + 5.0f * s + 6.0f * s + 7.0f * s;
            const float foot_h = 1.0f * s + 8.0f * s + 16.0f * s + 10.0f * s;
            const float body_h = 4.0f * s + row_h * static_cast<float>(fv.stages.size()) + 6.0f * s + foot_h;
            const float h = 2.0f * s + 40.0f * s + (b.collapsed ? 0.0f : body_h);
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const D2D1_RECT_F hr = D2D1::RectF(il, r.top + 1.0f, ir, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                card_head(r, hr, AiIcon::Duplicate, b.label, b.meta, AiIcon::Bolt, b.collapsed, AiPanelPart::FunnelToggle, index, key);
                if (!b.collapsed) {
                    float yy = hr.bottom + 4.0f * s;
                    for (size_t i = 0; i < fv.stages.size(); ++i) {
                        const AiFunnelStageView& st = fv.stages[i];
                        const int si = static_cast<int>(i);
                        const bool on = st.state != AiFunnelState::Wait;
                        const float op = TweenTo(XKey(b.id, si), on ? 1.0f : 0.45f, 300, now_ms);
                        const float run_k = TweenTo(XKey(b.id, 4 + si), st.state == AiFunnelState::Run ? 1.0f : 0.0f, 300, now_ms);
                        const D2D1_RECT_F rowr = D2D1::RectF(il, yy, ir, yy + row_h);
                        if (visible(rowr.top, rowr.bottom)) {
                            if (run_k > 0.0f) c.Fill(rowr, 0.0f, Alpha(pal.accent_tint, run_k));
                            const bool layer = op < 1.0f;
                            if (layer)
                                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                                     D2D1::IdentityMatrix(), op, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                                              nullptr);
                            const float ty = yy + 7.0f * s;
                            // .pn: hover / accent (run) / ok with a check.
                            const D2D1_POINT_2F pm = D2D1::Point2F(bl + 12.0f * s, ty + 17.0f * s);
                            const D2D1_COLOR_F pbg = st.state == AiFunnelState::Ok    ? pal.ok
                                                     : st.state == AiFunnelState::Run ? pal.accent
                                                                                      : pal.hover;
                            dc->FillEllipse(D2D1::Ellipse(pm, 12.0f * s, 12.0f * s), c.B(pbg));
                            if (st.state == AiFunnelState::Ok) {
                                c.Icon(AiIcon::Check, pm.x - 6.5f * s, pm.y - 6.5f * s, 13.0f * s, pal.on_accent);
                            } else {
                                const std::wstring n = std::to_wstring(i + 1);
                                const float nw = c.Measure(n, formats_.caption_b);
                                line(n, formats_.caption_b, pm.x - nw * 0.5f, pm.y - 9.0f * s, pm.x + nw * 0.5f + 1.0f, 18.0f * s,
                                     st.state == AiFunnelState::Run ? pal.on_accent : pal.text2);
                            }
                            const float lx = bl + 24.0f * s + 10.0f * s;
                            // .num: the count, then the unit (small, text3).
                            float num_w = 0.0f;
                            if (on && (st.value > 0 || st.state == AiFunnelState::Ok)) {
                                const float k = Count(st.count_ms, st.count_dur);
                                const std::wstring v = Thousands(static_cast<uint64_t>(static_cast<double>(st.value) * k + 0.5));
                                const std::wstring u = st.unit.empty() ? std::wstring() : L" " + st.unit;
                                const float uw = c.Measure(u, formats_.meta);
                                const float vw = c.Measure(v, formats_.body13b);
                                line(u, formats_.meta, br - uw, ty + 0.5f * s, br + 1.0f, 18.0f * s, pal.text3);
                                line(v, formats_.body13b, br - uw - vw, ty, br - uw + 1.0f, 18.0f * s, pal.text);
                                num_w = uw + vw;
                            }
                            line(st.label, formats_.small125b, lx, ty, br - num_w - 8.0f * s, 18.0f * s, pal.text);
                            float tm_w = 0.0f;
                            if (!st.time.empty()) {
                                tm_w = c.Measure(st.time, formats_.meta);
                                line(st.time, formats_.meta, br - tm_w, ty + 18.0f * s, br + 1.0f, 16.0f * s, pal.text3);
                            }
                            line(st.sub, formats_.meta, lx, ty + 18.0f * s, br - tm_w - 8.0f * s, 16.0f * s, pal.text3);
                            // .bar
                            const float by = ty + 18.0f * s + 16.0f * s + 5.0f * s;
                            const D2D1_RECT_F track = D2D1::RectF(lx, by, br, by + 6.0f * s);
                            c.Fill(track, 3.0f * s, pal.line2);
                            const float bw = TweenTo(XKey(b.id, 8 + si), on ? st.bar : 0.0f, 600, now_ms);
                            if (bw > 0.0f)
                                c.Fill(D2D1::RectF(lx, by, lx + (std::max)(6.0f * s, (br - lx) * bw), by + 6.0f * s), 3.0f * s, pal.accent);
                            if (layer) dc->PopLayer();
                        }
                        yy += row_h;
                    }
                    yy += 6.0f * s;
                    c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                    yy += 1.0f * s + 8.0f * s;
                    c.Icon(AiIcon::Shield, bl, yy + 1.5f * s, 13.0f * s, pal.accent);
                    line(fv.foot, formats_.meta, bl + 19.0f * s, yy, br, 16.0f * s, pal.text3);
                }
            }
            hit(hr, AiPanelPart::FunnelToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Dup: {
            // .card: .card-h 40 (dup), .rules (padding 2 12 10, .qc chips 26
            // with a numbered .no2 15 circle, 改规则 dashed), the .dg groups
            // (border-top line2, padding 9 12 8: .dg-h 20 with the file icon
            // 16, name 12.5/600, 都留 .tg.w, .mu right; .df rows 26 margin 1 0
            // 0 18, padding 0 6, .kp 38 badge, the path (dir text3), date
            // right; .dg-why margin 3 0 0 24 lh 16), .dg-more (border-top,
            // padding 8 12 10 30, link right) and .cl-sum on the pane.
            place(0.0f);
            const AiDupView& dv = b.dup;
            const float il = x0 + 1.0f * s, ir = x1 - 1.0f * s;
            const float bl = il + 12.0f * s, br = ir - 12.0f * s;
            // Rule chips: wrap at br.
            struct RulePos { float x, y, w; };
            std::vector<RulePos> rp;
            float cx = bl, cy = 0.0f;
            const auto place_chip = [&](float w) {
                if (cx > bl && cx + w > br) {
                    cx = bl;
                    cy += 26.0f * s + 6.0f * s;
                }
                rp.push_back({cx, cy, w});
                cx += w + 6.0f * s;
            };
            for (const std::wstring& t : dv.rules)
                place_chip(1.0f * s + 6.0f * s + 15.0f * s + 5.0f * s + c.Measure(t, formats_.caption) + 10.0f * s + 1.0f * s);
            if (!dv.change.empty())
                place_chip(1.0f * s + 10.0f * s + 12.0f * s + 5.0f * s + c.Measure(dv.change, formats_.caption) + 10.0f * s + 1.0f * s);
            const float rules_h = dv.rules.empty() ? 0.0f : 2.0f * s + cy + 26.0f * s + 10.0f * s;
            const auto group_h = [&](const AiDupGroupView& g) {
                const float files = 27.0f * s * static_cast<float>(g.files.size() + (g.more.empty() ? 0 : 1));
                // .dg-why wraps (the 都留 reason is long in a narrow panel).
                const float why_h = g.why.empty() ? 0.0f
                                                  : 3.0f * s + (std::max)(16.0f * s, wrapped_h(g.why, formats_.meta,
                                                                                               (std::max)(1.0f, br - (bl + 41.0f * s)), 16.0f * s));
                return 1.0f * s + 9.0f * s + 20.0f * s + files + why_h + 8.0f * s;
            };
            float groups_h = 0.0f;
            for (const AiDupGroupView& g : dv.groups) groups_h += group_h(g);
            const float more_h = dv.more.empty() ? 0.0f : 1.0f * s + 8.0f * s + 18.0f * s + 10.0f * s;
            // 每组至少留 1 份: right of the total, or below it when narrow.
            float dup_sum_w = 0.0f;
            for (const auto& [t, num] : dv.sum) dup_sum_w += c.Measure(t, num ? formats_.num15 : formats_.small125);
            const float dup_safe_w = dv.safe.empty() ? 0.0f : c.Measure(dv.safe, formats_.meta) + 17.0f * s;
            const bool dup_safe_below = !dv.safe.empty() && br - dup_safe_w <= bl + dup_sum_w + 8.0f * s;
            const float sum_h = dv.sum.empty() ? 0.0f
                                               : 1.0f * s + 10.0f * s + 20.0f * s + 10.0f * s + (dup_safe_below ? 20.0f * s : 0.0f);
            const float h = 2.0f * s + 40.0f * s + (b.collapsed ? 0.0f : rules_h + groups_h + more_h + sum_h);
            const D2D1_RECT_F r = D2D1::RectF(x0, top + y, x1, top + y + h);
            const D2D1_RECT_F hr = D2D1::RectF(il, r.top + 1.0f, ir, r.top + 1.0f + 40.0f * s);
            if (visible(r.top, r.bottom)) {
                card(r, pal.line);
                card_head(r, hr, AiIcon::Duplicate, b.label, b.meta, AiIcon::Count, b.collapsed, AiPanelPart::DupToggle, index, key);
                if (!b.collapsed) {
                    float yy = hr.bottom;
                    if (!dv.rules.empty()) {
                        const float ry = yy + 2.0f * s;
                        for (size_t i = 0; i < dv.rules.size(); ++i) {
                            const RulePos& p = rp[i];
                            const D2D1_RECT_F cr = D2D1::RectF(p.x, ry + p.y, p.x + p.w, ry + p.y + 26.0f * s);
                            c.Fill(cr, 13.0f * s, pal.accent_tint);
                            c.Stroke(cr, 13.0f * s, pal.accent_soft);
                            if (i + 1 == dv.rules.size()) {
                                // The third rule just changed: .swap's inset ring.
                                const float fk = Flash(dv.rule_ms);
                                if (fk > 0.0f) c.Stroke(Inflate(cr, -1.0f * s), 12.0f * s, Alpha(pal.accent, fk), 2.0f * s);
                            }
                            const D2D1_POINT_2F nm = D2D1::Point2F(cr.left + 7.0f * s + 7.5f * s, cr.top + 13.0f * s);
                            dc->FillEllipse(D2D1::Ellipse(nm, 7.5f * s, 7.5f * s), c.B(pal.accent));
                            const std::wstring n = std::to_wstring(i + 1);
                            const float nw = c.Measure(n, formats_.badge);
                            line(n, formats_.badge, nm.x - nw * 0.5f, cr.top, nm.x + nw * 0.5f + 1.0f, 26.0f * s, pal.on_accent);
                            const float tx = nm.x + 7.5f * s + 5.0f * s;
                            line(dv.rules[i], formats_.caption, tx, cr.top, cr.right - 9.0f * s, 26.0f * s, pal.text);
                        }
                        if (!dv.change.empty()) {
                            const RulePos& p = rp.back();
                            const D2D1_RECT_F ar = D2D1::RectF(p.x, ry + p.y, p.x + p.w, ry + p.y + 26.0f * s);
                            const bool ah = !dv.settled && hot(AiPanelPart::DupRule, index);
                            if (ah) c.Fill(ar, 13.0f * s, pal.accent_tint);
                            ID2D1StrokeStyle* dash = nullptr;
                            if (ID2D1Factory* f = FactoryOf(dc)) {
                                const float dashes[] = {3.0f, 2.0f};
                                f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                                  D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f,
                                                                                  D2D1_DASH_STYLE_CUSTOM, 0.0f),
                                                     dashes, 2, &dash);
                                f->Release();
                            }
                            const float hw = 0.5f * s;
                            const D2D1_RECT_F sr = D2D1::RectF(ar.left + hw, ar.top + hw, ar.right - hw, ar.bottom - hw);
                            dc->DrawRoundedRectangle(D2D1::RoundedRect(sr, 13.0f * s - hw, 13.0f * s - hw),
                                                     c.B(ah ? pal.accent : pal.line), 1.0f * s, dash);
                            if (dash) dash->Release();
                            const D2D1_COLOR_F ink = dv.settled ? Alpha(pal.text3, 0.6f) : ah ? pal.text : pal.text3;
                            c.Icon(AiIcon::Sliders, ar.left + 11.0f * s, ar.top + 7.0f * s, 12.0f * s, ink);
                            line(dv.change, formats_.caption, ar.left + 28.0f * s, ar.top, ar.right, 26.0f * s, ink);
                            if (!dv.settled) hit(ar, AiPanelPart::DupRule, index);
                        }
                        yy += rules_h;
                    }
                    for (size_t gi = 0; gi < dv.groups.size(); ++gi) {
                        const AiDupGroupView& g = dv.groups[gi];
                        const float gh = group_h(g);
                        const float ik = g.in_ms ? Count(g.in_ms, 300) : 1.0f;
                        if (ik > 0.0f && visible(yy, yy + gh)) {
                            const bool g_pushed = push_entrance({ik, 4.0f * s * (1.0f - ik)});
                            c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                            float ty = yy + 1.0f * s + 9.0f * s;
                            // .dg-h
                            const int ti = static_cast<int>(g.tint);
                            c.Icon(g.icon, bl, ty + 2.0f * s, 16.0f * s, pal.tint_fg[ti]);
                            const float mw = c.Measure(g.meta, formats_.meta);
                            line(g.meta, formats_.meta, br - mw, ty, br + 1.0f, 20.0f * s, pal.text3);
                            const float nx = bl + 16.0f * s + 8.0f * s;
                            const float name_r = br - mw - 8.0f * s;
                            const float tag_w = g.both && !dv.both_tag.empty() ? c.Measure(dv.both_tag, formats_.badge) + 12.0f * s : 0.0f;
                            const float nw = (std::min)(c.Measure(g.name, formats_.small125b), (std::max)(0.0f, name_r - nx - (tag_w > 0.0f ? tag_w + 8.0f * s : 0.0f)));
                            line(g.name, formats_.small125b, nx, ty, nx + nw + 1.0f, 20.0f * s, pal.text);
                            if (tag_w > 0.0f && nx + nw + 8.0f * s + tag_w <= name_r + 1.0f) {
                                const D2D1_RECT_F gr = D2D1::RectF(nx + nw + 8.0f * s, ty + 1.5f * s, nx + nw + 8.0f * s + tag_w, ty + 18.5f * s);
                                c.Fill(gr, 8.0f * s, pal.amber_bg);
                                line(dv.both_tag, formats_.badge, gr.left + 6.0f * s, gr.top, gr.right, 17.0f * s, pal.amber);
                            }
                            ty += 20.0f * s;
                            // .df rows
                            for (size_t fi = 0; fi < g.files.size(); ++fi) {
                                const AiDupFileView& f = g.files[fi];
                                const int hi = index * 64 + static_cast<int>(gi) * 8 + static_cast<int>(fi);
                                const D2D1_RECT_F fr = D2D1::RectF(bl + 18.0f * s, ty + 1.0f * s, br, ty + 27.0f * s);
                                const float fade = TweenTo(XKey(b.id, 64 + static_cast<int>(gi) * 8 + static_cast<int>(fi)),
                                                           dv.settled && !f.keep ? 0.45f : 1.0f, 300, now_ms);
                                const bool fl = fade < 1.0f;
                                if (fl)
                                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                                         D2D1::IdentityMatrix(), fade, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                                                  nullptr);
                                if (!dv.settled && hot(AiPanelPart::DupFile, hi)) c.Fill(fr, 6.0f * s, pal.hover);
                                const float fk = Flash(f.swap_ms);
                                if (fk > 0.0f) c.Stroke(Inflate(fr, -1.0f * s), 5.0f * s, Alpha(pal.accent, fk), 2.0f * s);
                                const float fx = fr.left + 6.0f * s;
                                const float mid = fr.top + 13.0f * s;
                                // .kp
                                const float keep_k = TweenTo(XKey(b.id, 128 + static_cast<int>(gi) * 8 + static_cast<int>(fi)),
                                                             f.keep ? 1.0f : 0.0f, 250, now_ms);
                                const D2D1_RECT_F kp = D2D1::RectF(fx, mid - 8.5f * s, fx + 38.0f * s, mid + 8.5f * s);
                                c.Fill(kp, 8.0f * s, Mix(pal.hover, pal.accent_soft, keep_k));
                                const std::wstring& kt = f.keep ? dv.keep_tag : dv.extra_tag;
                                const float kw = c.Measure(kt, formats_.badge);
                                line(kt, formats_.badge, (kp.left + kp.right - kw) * 0.5f, kp.top, kp.right, 17.0f * s,
                                     Mix(pal.text3, pal.ok, keep_k));
                                // .dt2
                                const float dw = c.Measure(f.date, formats_.meta);
                                const float pr = fr.right - 6.0f * s;
                                line(f.date, formats_.meta, pr - dw, fr.top, pr + 1.0f, 26.0f * s, pal.text3);
                                // .p: dir (text3), then the name (struck through on a copy).
                                const float px = kp.right + 8.0f * s, pend = pr - dw - 8.0f * s;
                                const float dirw = (std::min)(c.Measure(f.dir, formats_.caption), (std::max)(0.0f, pend - px));
                                line(f.dir, formats_.caption, px, fr.top, px + dirw + 1.0f, 26.0f * s, pal.text3);
                                const float nx2 = px + dirw;
                                if (pend > nx2 + 4.0f * s) {
                                    const float namew = (std::min)(c.Measure(f.name, formats_.caption), pend - nx2);
                                    line(f.name, formats_.caption, nx2, fr.top, pend, 26.0f * s, Mix(pal.text3, pal.text, keep_k));
                                    if (keep_k < 1.0f)
                                        c.Fill(D2D1::RectF(nx2, mid + 0.5f * s, nx2 + namew, mid + 1.5f * s), 0.0f,
                                               Alpha(pal.text3, 0.6f * (1.0f - keep_k)));
                                }
                                if (fl) dc->PopLayer();
                                if (!dv.settled) hit(fr, AiPanelPart::DupFile, hi);
                                ty += 27.0f * s;
                            }
                            if (!g.more.empty()) {
                                line(g.more, formats_.caption, bl + 24.0f * s + 44.0f * s, ty + 1.0f * s, br, 26.0f * s, pal.text3);
                                ty += 27.0f * s;
                            }
                            // .dg-why
                            if (!g.why.empty()) {
                                ty += 3.0f * s;
                                const D2D1_COLOR_F wc = g.both ? pal.amber : pal.accent;
                                c.Icon(g.both ? AiIcon::Warn : AiIcon::Check, bl + 24.0f * s, ty + 2.0f * s, 12.0f * s, wc);
                                wrapped(g.why, formats_.meta, bl + 41.0f * s, ty, (std::max)(1.0f, br - (bl + 41.0f * s)), 16.0f * s,
                                        g.both ? pal.amber : pal.text3);
                            }
                            pop_entrance(g_pushed);
                        }
                        yy += gh;
                    }
                    if (!dv.more.empty()) {
                        c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line2);
                        const float my = yy + 1.0f * s + 8.0f * s;
                        const float lw = dv.more_link.empty() ? 0.0f : c.Measure(dv.more_link, formats_.caption_b);
                        const float lx = br - lw;
                        line(dv.more, formats_.caption, bl + 18.0f * s, my, lx - 8.0f * s, 18.0f * s, pal.text3);
                        if (lw > 0.0f) {
                            const D2D1_RECT_F lr = D2D1::RectF(lx - 3.0f * s, my - 1.0f * s, br + 3.0f * s, my + 19.0f * s);
                            if (hot(AiPanelPart::DupMore, index)) c.Fill(lr, 4.0f * s, pal.accent_tint);
                            line(dv.more_link, formats_.caption_b, lx, my, br + 1.0f, 18.0f * s, pal.accent);
                            hit(Inflate(lr, 2.0f * s), AiPanelPart::DupMore, index);
                        }
                        yy += more_h;
                    }
                    if (!dv.sum.empty()) {
                        FillCorners(c, D2D1::RectF(il, yy, ir, r.bottom - 1.0f), 0.0f, 0.0f, 11.0f * s, 11.0f * s, pal.pane);
                        c.Fill(D2D1::RectF(il, yy, ir, yy + 1.0f * s), 0.0f, pal.line);
                        yy += 1.0f * s + 10.0f * s;
                        const float bump = Bump(dv.sum_bump_ms);
                        float sx = bl;
                        for (const auto& [t, num] : dv.sum) {
                            IDWriteTextFormat* fmt = num ? formats_.num15 : formats_.small125;
                            const float w = c.Measure(t, fmt);
                            D2D1_MATRIX_3X2_F before;
                            dc->GetTransform(&before);
                            if (num && bump > 0.0f)
                                dc->SetTransform(D2D1::Matrix3x2F::Scale(1.0f + 0.08f * bump, 1.0f + 0.08f * bump,
                                                                         D2D1::Point2F(sx + w * 0.5f, yy + 10.0f * s)) * before);
                            line(t, fmt, sx, yy + (num ? 0.0f : 1.0f * s), sx + w + 1.0f, 20.0f * s, num ? pal.text : pal.text2);
                            dc->SetTransform(before);
                            sx += w;
                        }
                        const float sfw = c.Measure(dv.safe, formats_.meta);
                        if (dup_safe_below) {
                            c.Icon(AiIcon::Shield, bl, yy + 20.0f * s + 4.0f * s, 12.0f * s, pal.accent);
                            line(dv.safe, formats_.meta, bl + 17.0f * s, yy + 20.0f * s, br + 1.0f, 20.0f * s, pal.text3);
                        } else if (!dv.safe.empty()) {
                            c.Icon(AiIcon::Shield, br - sfw - 17.0f * s, yy + 4.0f * s, 12.0f * s, pal.accent);
                            line(dv.safe, formats_.meta, br - sfw, yy, br + 1.0f, 20.0f * s, pal.text3);
                        }
                    }
                }
            }
            hit(hr, AiPanelPart::DupToggle, index);
            y += h;
            pending = 12.0f * s;
            break;
        }
        case AiBlockKind::Note: {
            // .note: centred 12 text3 between 1 px rules, gap 8; margin 4 0 14.
            place(4.0f * s);
            const float ny = top + y;
            const float tw = c.Measure(b.text, formats_.caption);
            const float cx = (x0 + x1) * 0.5f;
            if (visible(ny, ny + 18.0f * s)) {
                const float l = cx - tw * 0.5f - 8.0f * s, rr = cx + tw * 0.5f + 8.0f * s;
                c.Fill(D2D1::RectF(x0, ny + 9.0f * s, l, ny + 9.0f * s + 1.0f), 0.0f, pal.line);
                c.Fill(D2D1::RectF(rr, ny + 9.0f * s, x1, ny + 9.0f * s + 1.0f), 0.0f, pal.line);
                line(b.text, formats_.caption, cx - tw * 0.5f, ny, cx + tw * 0.5f + 2.0f, 18.0f * s, pal.text3);
            }
            y += 18.0f * s;
            pending = 14.0f * s;
            break;
        }
        }
        pop_entrance(pushed);
    }
    y += pending + 10.0f * s;
    content_h_ = y;
    EndRichFrame();

    // Snap a following view to the end now that the height is known (no
    // one-frame lag while text streams in).
    const float max_scroll = (std::max)(0.0f, content_h_ - view_h_);
    if (follow_ && std::fabs(scroll_ - max_scroll) > 0.5f) {
        scroll_ = max_scroll;
        conv_anim_ = true;   // redraw at the new offset
    }
    if (scroll_ > max_scroll) scroll_ = max_scroll;

    // The shared scrollbar: only when it overflows.
    if (max_scroll > 0.5f)
        DrawBar(0, D2D1::RectF(area.right - 12.0f * s, area.top + 4.0f * s, area.right - 2.0f * s,
                               area.top + view_h_ - 4.0f * s), scroll_, view_h_, content_h_);
    // 55 P4: the 回到最新 pill shows once the view is scrolled up from the end.
    jump_shown_ = max_scroll > 1.0f && scroll_ < max_scroll - 4.0f * s && !vm.jump_latest.empty();
    if (jump_shown_) {
        const float w = 112.0f * s, h = 30.0f * s;
        const float cx = (conversation_rect_.left + conversation_rect_.right) * 0.5f;
        jump_rect_ = D2D1::RectF(cx - w * 0.5f, conversation_rect_.bottom - h - 10.0f * s,
                                 cx + w * 0.5f, conversation_rect_.bottom - 10.0f * s);
        c.Fill(jump_rect_, h * 0.5f, pal.card);
        c.Stroke(jump_rect_, h * 0.5f, pal.text3, 1.0f);
        TextOptions o;
        o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
        c.Text(vm.jump_latest, formats_.caption, jump_rect_, pal.text, o);
    }

    // Drop tweens of blocks that are gone.
    tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(), [](const auto& kv) { return !kv.second.used; }),
                  tweens_.end());
}

} // namespace pulse::ui
