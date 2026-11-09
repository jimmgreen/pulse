// ai_panel_peek.cpp — the bubble above the status bar chip once Pulse 助手
// is closed (scene 7):
//
//   ✦ Pulse 助手 · 完成                         ×
//     已把 12 个文件整理到 5 个新文件夹：…
//     [↶ 撤销] [打开对话]
//
// 318 wide, card, 1 px line, radius 14, the panel's shadow, padding 12; a
// tail at left 26 points at the chip. In: opacity .25 s, translateY(10) and
// scale(.95) from 10% 100% to none, .3 s cubic-bezier(.3,.7,.2,1).
#include "ai_panel_view.h"
#include "ai_panel_draw.h"

#include "FluentTokens.h"
#include "ui_compositor.h"
#include "ui_motion.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
using namespace ai_draw;
namespace {

float Bezier(float x1, float y1, float x2, float y2, float t) {
    t = (std::clamp)(t, 0.0f, 1.0f);
    float u = t;
    for (int i = 0; i < 8; ++i) {
        const float x = 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u - t;
        const float dx = 3 * (1 - u) * (1 - u) * x1 + 6 * (1 - u) * u * (x2 - x1) + 3 * u * u * (1 - x2);
        if (std::fabs(dx) < 1e-5f) break;
        u = (std::clamp)(u - x / dx, 0.0f, 1.0f);
    }
    return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u;
}

}  // namespace

void AiPanelView::DrawPeek(Compositor* compositor, const AiPanelViewModel& vm, const Theme& theme,
                           const D2D1_RECT_F& chip, float scale, uint64_t now_ms) {
    ID2D1DeviceContext* dc = compositor ? compositor->Dc() : nullptr;
    const bool want = vm.peek.on && chip.right > chip.left;
    if (want != peek_target_) {
        peek_from_ = peek_value_;
        peek_target_ = want;
        peek_t0_ = now_ms;
    }
    const bool motion_on = motion::SystemAnimationsEnabled();
    const float t = motion_on ? static_cast<float>(now_ms - peek_t0_) : 1e6f;
    const float target = peek_target_ ? 1.0f : 0.0f;
    const float k = (std::clamp)(t / 300.0f, 0.0f, 1.0f);
    peek_value_ = peek_from_ + (target - peek_from_) * k;
    peek_anim_ = k < 1.0f;
    if (peek_value_ <= 0.0f || !dc) {
        peek_rect_ = peek_close_ = peek_undo_ = peek_open_ = D2D1_RECT_F{};
        return;
    }
    EnsureFormats(compositor, scale);
    if (!formats_.body13b || !EnsureBrush(dc)) return;
    const Palette pal = AiPanelPalette(theme, vm.dark);
    const float s = scale;
    Ctx c{compositor, dc, brush_, s};

    // Opacity over .25 s, the move over .3 s with the design's curve; going
    // out runs the same way back.
    const float op_k = (std::clamp)(t / 250.0f, 0.0f, 1.0f);
    const float opacity = peek_target_ ? peek_from_ + (1.0f - peek_from_) * op_k : peek_from_ * (1.0f - op_k);
    const float pose = peek_target_ ? Bezier(0.3f, 0.7f, 0.2f, 1.0f, peek_value_) : peek_value_;

    // Layout.
    const float w = 318.0f * s;
    const float pad = 12.0f * s;
    const float inner = w - 2.0f * pad;
    const float text_w = inner - 24.0f * s;
    const float title_h = 18.0f * s;
    const float body_h = vm.peek.text.empty() ? 0.0f
                                              : c.MeasureWrapped(vm.peek.text, formats_.small125, text_w, 19.0f * s);
    const float h = pad + title_h + 6.0f * s + body_h + 10.0f * s + 32.0f * s + pad;
    // The status bar is 34 high with the chip centred; the bubble sits 8 above it.
    const float bar_pad = (std::max)(0.0f, (34.0f * s - (chip.bottom - chip.top)) * 0.5f);
    const float bottom = chip.top - bar_pad - 8.0f * s;
    const float left = chip.left;
    const D2D1_RECT_F box = D2D1::RectF(left, bottom - h, left + w, bottom);

    D2D1_MATRIX_3X2_F before;
    dc->GetTransform(&before);
    const D2D1_POINT_2F origin = D2D1::Point2F(box.left + w * 0.10f, box.bottom);
    const float sc = 0.95f + 0.05f * pose;
    dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, origin) * D2D1::Matrix3x2F::Translation(0.0f, 10.0f * s * (1.0f - pose)) *
                     before);
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         D2D1::IdentityMatrix(), (std::clamp)(opacity, 0.0f, 1.0f), nullptr,
                                         D2D1_LAYER_OPTIONS1_NONE),
                  nullptr);

    const float radius = 14.0f * s;
    // --shadow: 0 22 60 (.20), 0 3 10 (.08).
    c.Shadow(box, radius, 48.0f * s, 20.0f * s, pal.shadow);
    c.Shadow(box, radius, 8.0f * s, 3.0f * s, Alpha(pal.shadow, 0.4f));
    c.Fill(box, radius, pal.card);
    c.Stroke(box, radius, pal.line);
    // ::after: a 10 px square turned 45°, left 26, bottom -6, card with the
    // line on its outer edges.
    {
        const float cx = box.left + 31.0f * s;
        const float cy = box.bottom + 1.0f * s;
        const float d = 5.0f * s * 1.41421356f;
        ID2D1Factory* factory = nullptr;
        dc->GetFactory(&factory);
        ID2D1PathGeometry* tail = nullptr;
        if (factory && SUCCEEDED(factory->CreatePathGeometry(&tail))) {
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(tail->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(cx - d, cy), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(cx, cy - d));
                sink->AddLine(D2D1::Point2F(cx + d, cy));
                sink->AddLine(D2D1::Point2F(cx, cy + d));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                sink->Release();
            }
            dc->FillGeometry(tail, c.B(pal.card));
            tail->Release();
        }
        if (factory) factory->Release();
        dc->DrawLine(D2D1::Point2F(cx - d + 0.5f * s, cy + 0.5f * s), D2D1::Point2F(cx, cy + d), c.B(pal.line), 1.0f * s);
        dc->DrawLine(D2D1::Point2F(cx, cy + d), D2D1::Point2F(cx + d - 0.5f * s, cy + 0.5f * s), c.B(pal.line), 1.0f * s);
    }

    const auto hot = [&](AiPanelPart part) { return vm.hover.part == part; };
    const auto down = [&](AiPanelPart part) { return vm.pressed.part == part && vm.hover.part == part; };

    // Title: spark 16 accent, gap 8, 13 semibold; × 14 text3 at the right.
    float y = box.top + pad;
    const float x0 = box.left + pad;
    // A failed turn: the spark takes the card's tone (err / amber / accent).
    D2D1_COLOR_F spark = pal.accent;
    if (vm.peek.failed && vm.peek.tone == AiErrTone::Err) spark = pal.bad;
    else if (vm.peek.failed && vm.peek.tone == AiErrTone::Warn) spark = pal.amber;
    c.Icon(AiIcon::Spark, x0, y + 1.0f * s, 16.0f * s, spark);
    c.Text(vm.peek.title, formats_.body13b, D2D1::RectF(x0 + 24.0f * s, y, box.right - pad - 22.0f * s, y + title_h),
           pal.text);
    const D2D1_RECT_F xr = D2D1::RectF(box.right - pad - 18.0f * s, y - 2.0f * s, box.right - pad + 4.0f * s, y + 20.0f * s);
    if (hot(AiPanelPart::PeekClose)) c.Fill(xr, 6.0f * s, pal.hover);
    c.Icon(AiIcon::Close, box.right - pad - 14.0f * s, y + 2.0f * s, 14.0f * s,
           hot(AiPanelPart::PeekClose) ? pal.text2 : pal.text3);
    peek_close_ = xr;
    y += title_h + 6.0f * s;
    if (body_h > 0.0f) {
        TextOptions o;
        o.wrap = true;
        o.line_height = 19.0f * s;
        c.Text(vm.peek.text, formats_.small125, D2D1::RectF(x0 + 24.0f * s, y, x0 + 24.0f * s + text_w, y + body_h),
               pal.text2, o);
    }
    y += body_h + 10.0f * s;

    // Buttons (.b2 h 32 r 8 0 14, 13; .wi icon 14 gap 6; .pri accent).
    float bx = x0 + 24.0f * s;
    const auto button = [&](const std::wstring& text, bool primary, AiIcon icon, bool with_icon, AiPanelPart part) {
        IDWriteTextFormat* fmt = primary ? formats_.body13b : formats_.body13;
        const float tw = c.Measure(text, fmt);
        const float bw = 28.0f * s + tw + (with_icon ? 20.0f * s : 0.0f);
        const D2D1_RECT_F r = D2D1::RectF(bx, y, bx + bw, y + 32.0f * s);
        D2D1_MATRIX_3X2_F inner_before;
        dc->GetTransform(&inner_before);
        if (down(part) && motion_on) {
            const D2D1_POINT_2F mid = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * inner_before);
        }
        if (primary) {
            c.Fill(r, 8.0f * s, hot(part) ? Mix(pal.accent, pal.text, 0.12f) : pal.accent);
        } else {
            c.Fill(r, 8.0f * s, hot(part) ? pal.hover : pal.bg);
            c.Stroke(r, 8.0f * s, pal.line);
        }
        const D2D1_COLOR_F ink = primary ? pal.on_accent : pal.text;
        float tx = r.left + 14.0f * s;
        if (with_icon) {
            c.Icon(icon, tx, y + 9.0f * s, 14.0f * s, ink);
            tx += 20.0f * s;
        }
        c.Text(text, fmt, D2D1::RectF(tx, y, r.right, y + 32.0f * s), ink);
        dc->SetTransform(inner_before);
        bx += bw + 8.0f * s;
        return r;
    };
    if (vm.peek.failed) {
        // 重试 (or 连接模型) leads; 打开对话 follows as the quiet one.
        const AiIcon lead = vm.peek.tone == AiErrTone::Accent ? AiIcon::Plug : AiIcon::Retry;
        peek_undo_ = vm.peek.undo.empty() ? D2D1_RECT_F{} : button(vm.peek.undo, true, lead, true, AiPanelPart::PeekUndo);
        peek_open_ = button(vm.peek.open, false, AiIcon::Open, false, AiPanelPart::PeekOpen);
    } else {
        peek_undo_ = vm.peek.undo.empty() ? D2D1_RECT_F{} : button(vm.peek.undo, false, AiIcon::Undo, true, AiPanelPart::PeekUndo);
        peek_open_ = button(vm.peek.open, true, AiIcon::Open, false, AiPanelPart::PeekOpen);
    }

    dc->PopLayer();
    dc->SetTransform(before);
    // Hit rects in window space: the bubble at rest (clicks land once it is in).
    peek_rect_ = box;
    if (!peek_target_) peek_rect_ = peek_close_ = peek_undo_ = peek_open_ = D2D1_RECT_F{};
}

AiPanelHit AiPanelView::PeekHitTest(float x, float y) const {
    if (peek_value_ <= 0.0f || !peek_target_) return {};
    if (In(peek_close_, x, y)) return {AiPanelPart::PeekClose, -1};
    if (In(peek_undo_, x, y)) return {AiPanelPart::PeekUndo, -1};
    if (In(peek_open_, x, y)) return {AiPanelPart::PeekOpen, -1};
    if (In(peek_rect_, x, y)) return {AiPanelPart::PeekCard, -1};
    return {};
}

}  // namespace pulse::ui
