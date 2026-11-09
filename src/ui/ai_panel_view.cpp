// ai_panel_view.cpp — Pulse 助手 panel: layout, drawing, hit testing.
//
// Sizes are the design's CSS pixels,
// used as DIPs.
#include "ai_panel_view.h"
#include "ai_panel_draw.h"
#include "ai_panel_rich.h"

#include "typography.h"
#include "ui_compositor.h"
#include "ui_motion.h"

#include <algorithm>
#include <cmath>

namespace pulse::ui {
using namespace ai_draw;
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDockWidthDip = 408.0f;
constexpr float kFloatingWidthDip = 408.0f;
constexpr float kFloatingHeightDip = 600.0f;
constexpr float kHeadDip = 52.0f;
constexpr uint64_t kFloatFadeMs = 300;
constexpr uint64_t kMenuMs = 220;

template <class T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

} // namespace


void AiPanelView::Formats::Release() {
    SafeRelease(title);
    SafeRelease(greeting);
    SafeRelease(body);
    SafeRelease(body13);
    SafeRelease(body13b);
    SafeRelease(caption);
    SafeRelease(small125);
    SafeRelease(small125b);
    SafeRelease(mono);
    SafeRelease(mono125);
    SafeRelease(model);
    SafeRelease(heading);
    SafeRelease(body_b);
    SafeRelease(caption_b);
    SafeRelease(badge);
    SafeRelease(meta);
    SafeRelease(num22);
    SafeRelease(num20);
    SafeRelease(num16);
    SafeRelease(num15);
    SafeRelease(num14);
    SafeRelease(small11);
    SafeRelease(small11b);
    scale = 0.0f;
}

AiPanelView::AiPanelView() = default;
AiPanelView::~AiPanelView() { ReleaseDeviceResources(); }

void AiPanelView::ReleaseDeviceResources() {
    ReleaseRich();
    formats_.Release();
    SafeRelease(brush_);
    brush_dc_ = nullptr;
}

void AiPanelView::SetState(bool open, AiPanelMode mode, bool animate) {
    animate_ = animate && motion::SystemAnimationsEnabled();
    const uint64_t now = motion::NowMs();
    const float dock_target = open && mode == AiPanelMode::Docked ? 1.0f : 0.0f;
    const float float_target = open && mode == AiPanelMode::Floating ? 1.0f : 0.0f;
    const bool dock_was = open_ && mode_ == AiPanelMode::Docked;
    const bool float_was = open_ && mode_ == AiPanelMode::Floating;
    if (open && !open_) opened_at_ = now;
    open_ = open;
    mode_ = mode;
    if ((dock_target > 0.0f) != dock_was) {
        if (animate_) { dock_from_ = dock_value_; dock_start_ = now; dock_anim_ = true; }
        else { dock_value_ = dock_target; dock_anim_ = false; }
    }
    if ((float_target > 0.0f) != float_was) {
        if (animate_) { float_from_ = float_value_; float_start_ = now; float_anim_ = true; }
        else { float_value_ = float_target; float_anim_ = false; }
    }
    if (!open) drawn_ = false;
}

void AiPanelView::Rewake(uint64_t now_ms) { opened_at_ = now_ms; }

bool AiPanelView::Tick(uint64_t now_ms) {
    // Geometry is the last drawn frame's; a frame that does not draw the
    // panel (Settings page) leaves nothing to hit or to place the EDIT in.
    drawn_ = false;
    input_placed_ = false;
    menu_rect_ = {};
    if (dock_anim_) {
        const float target = open_ && mode_ == AiPanelMode::Docked ? 1.0f : 0.0f;
        const float ms = static_cast<float>(now_ms - dock_start_);
        if (ms >= static_cast<float>(motion::SpringSettleMs(motion::spring::Smooth))) {
            dock_value_ = target;
            dock_anim_ = false;
        } else {
            const float p = motion::SpringProgress(motion::spring::Smooth, ms);
            dock_value_ = std::clamp(dock_from_ + (target - dock_from_) * p, 0.0f, 1.0f);
        }
    }
    if (float_anim_) {
        const float target = open_ && mode_ == AiPanelMode::Floating ? 1.0f : 0.0f;
        const float t = static_cast<float>(now_ms - float_start_) / static_cast<float>(kFloatFadeMs);
        if (t >= 1.0f) {
            float_value_ = target;
            float_anim_ = false;
        } else {
            float_value_ = std::clamp(float_from_ + (target - float_from_) * EaseDesign(t), 0.0f, 1.0f);
        }
    }
    return dock_anim_ || float_anim_;
}

float AiPanelView::DockTargetWidth(float window_width, float rail_width, float scale) {
    // Leave the panes at least 480 DIP; never narrower than 300 DIP.
    const float room = window_width - rail_width - 480.0f * scale;
    return std::round(std::clamp(room, 300.0f * scale, kDockWidthDip * scale));
}

float AiPanelView::DockWidth(float window_width, float rail_width, float scale) const {
    if (dock_value_ <= 0.0f) return 0.0f;
    return std::round(DockTargetWidth(window_width, rail_width, scale) * dock_value_);
}

void AiPanelView::EnsureFormats(Compositor* compositor, float scale) {
    const float font_scale = typography::UiFontScale();
    if (formats_.scale == scale && formats_.font_scale == font_scale && formats_.body) return;
    formats_.Release();
    IDWriteFactory2* f = compositor->DwriteFactory();
    if (!f) return;
    auto make = [&](float dip, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat*& out) {
        typography::TextFormatSpec spec;
        spec.size = dip * scale;
        spec.weight = weight;
        typography::CreateTextFormat(f, spec, &out);
    };
    make(14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.title);
    make(21.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.greeting);
    make(13.5f, DWRITE_FONT_WEIGHT_NORMAL, formats_.body);
    make(13.0f, DWRITE_FONT_WEIGHT_NORMAL, formats_.body13);
    make(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.body13b);
    make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, formats_.caption);
    make(12.5f, DWRITE_FONT_WEIGHT_NORMAL, formats_.small125);
    make(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.small125b);
    make(12.5f, DWRITE_FONT_WEIGHT_MEDIUM, formats_.model);
    make(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.heading);
    make(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.body_b);
    make(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.caption_b);
    make(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.badge);
    make(11.5f, DWRITE_FONT_WEIGHT_NORMAL, formats_.meta);
    make(22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.num22);
    make(20.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.num20);
    make(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.num16);
    make(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.num15);
    make(14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.num14);
    make(11.0f, DWRITE_FONT_WEIGHT_NORMAL, formats_.small11);
    make(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, formats_.small11b);
    {
        typography::TextFormatSpec spec;
        spec.role = typography::FontRole::Monospace;
        spec.size = 11.5f * scale;
        typography::CreateTextFormat(f, spec, &formats_.mono);
        spec.size = 12.5f * scale;
        typography::CreateTextFormat(f, spec, &formats_.mono125);
    }
    formats_.scale = scale;
    formats_.font_scale = font_scale;
}

namespace {


AiPanelView::Palette MakePalette(const Theme& theme, bool dark) {
    AiPanelView::Palette p{};
    const D2D1_COLOR_F base = Over(theme.bg, dark ? Hex(0x1c1e1d) : Hex(0xfdfcfb));
    p.bg = base;
    p.card = Over(theme.surface_card, base);
    if (!dark) p.card = Mix(p.card, Hex(0xffffff), 0.6f);
    const D2D1_COLOR_F sheet = Over(theme.surface_sheet, base);
    p.pane = Mix(p.card, sheet, dark ? 0.45f : 0.55f);
    p.accent = theme.accent;
    p.accent.a = 1.0f;
    p.accent_soft = Mix(p.card, p.accent, dark ? 0.20f : 0.13f);
    p.accent_tint = Mix(p.card, p.accent, dark ? 0.10f : 0.07f);
    p.on_accent = dark ? Hex(0x10201b) : Hex(0xffffff);
    p.text = theme.text;
    p.text.a = 1.0f;
    p.text2 = theme.text_secondary;
    p.text2.a = 1.0f;
    p.text3 = Mix(p.text2, p.card, dark ? 0.30f : 0.36f);
    p.line = dark ? Hex(0x353936) : Hex(0xe3e4dd);
    p.line2 = dark ? Hex(0x2d302e) : Hex(0xecede7);
    p.hover = dark ? Hex(0x2d312e) : Hex(0xe9ebe4);
    p.ok = dark ? Hex(0x5fbf8a) : Hex(0x2f8a5b);
    p.amber = dark ? Hex(0xe3ab52) : Hex(0xa86c12);
    p.amber_bg = dark ? Hex(0x3a3020) : Hex(0xfbf0d9);
    p.amber_line = dark ? Hex(0x5e4a26) : Hex(0xefd59f);
    p.bad = dark ? Hex(0xef8a75) : Hex(0xb4442f);
    p.bad_bg = dark ? Hex(0x3d2622) : Hex(0xfbe9e5);
    p.bad_line = dark ? Hex(0x62372f) : Hex(0xf0c4ba);
    // Second batch: system parts of the disk bar, the red delete-for-good card.
    p.sys = dark ? Hex(0x97a3b1) : Hex(0x7a8796);
    p.red = dark ? Hex(0xf0768a) : Hex(0xc4314b);
    p.red_bg = dark ? Hex(0x3d2328) : Hex(0xfbe9ec);
    p.red_line = dark ? Hex(0x6a3540) : Hex(0xefc2ca);
    p.shadow = dark ? Hex(0x000000, 0.55f) : Hex(0x1c2822, 0.20f);
    p.folder_icon = Hex(0xc49a3a);
    // Image, Document, App, Video, Archive, Neutral.
    const uint32_t fg_light[6] = {0x8257c2, 0x2f6bc0, 0x16816f, 0xc03f66, 0xa86c12, 0x5b626c};
    const uint32_t bg_light[6] = {0xf1eafb, 0xe6effb, 0xddf3ee, 0xfbe7ee, 0xfbf0d9, 0xe9ebe4};
    const uint32_t fg_dark[6] = {0xb393e6, 0x7eaaf0, 0x5cc4ae, 0xee86a6, 0xe3ab52, 0xb1b6b0};
    const uint32_t bg_dark[6] = {0x352c45, 0x253248, 0x20372f, 0x43262f, 0x3a3020, 0x2d312e};
    for (int i = 0; i < 6; ++i) {
        p.tint_fg[i] = Hex(dark ? fg_dark[i] : fg_light[i]);
        p.tint_bg[i] = Hex(dark ? bg_dark[i] : bg_light[i]);
    }
    return p;
}

// The docked column over a wallpaper or Mica (see Palette::glass): the
// pane card's fill at `a` and its frame; the welcome cards a little firmer
// (light +30%, dark 5.5% white), the resting composer firmer still.
void ApplyGlass(AiPanelView::Palette& p, const Theme& theme, bool dark, float a, D2D1_COLOR_F frame) {
    p.glass = true;
    p.glass_panel = theme.surface_card;
    p.glass_panel.a = std::clamp(a, 0.0f, 1.0f);
    p.glass_panel_line = frame;
    const D2D1_COLOR_F white = Hex(0xffffff);
    if (dark) {
        p.glass_card = Alpha(white, 0.055f);
        p.glass_card_line = Alpha(white, 0.08f);
        p.glass_input = Alpha(white, 0.06f);
        p.glass_input_line = Alpha(white, 0.10f);
        p.glass_line = Alpha(white, 0.07f);
        p.glass_hover = Alpha(white, 0.08f);
    } else {
        p.glass_card = Alpha(white, (std::min)(1.0f, a + 0.30f));
        p.glass_card_line = Alpha(white, 0.70f);
        p.glass_input = Alpha(white, (std::min)(1.0f, a + 0.40f));
        p.glass_input_line = Alpha(white, 0.80f);
        p.glass_line = Hex(0x303b50, 0.09f);
        p.glass_hover = Hex(0x303b50, 0.08f);
    }
}

} // namespace

void AiPanelView::Draw(Compositor* compositor, const AiPanelViewModel& vm, const Theme& theme,
                       const D2D1_RECT_F& column, float column_content_width, const D2D1_RECT_F& floating_area,
                       float scale, uint64_t now_ms) {
    drawn_ = false;
    input_placed_ = false;
    decor_ = false;
    bars_[0].drawn = false;
    conv_anim_ = false;
    conv_decor_ = false;
    ID2D1DeviceContext* dc = compositor ? compositor->Dc() : nullptr;
    if (!dc || !Visible()) return;
    EnsureFormats(compositor, scale);
    if (!formats_.body) return;
    if (!EnsureBrush(dc)) return;
    const Palette pal = MakePalette(theme, vm.dark);
    card_color_ = pal.card;
    bg_color_ = pal.bg;
    text_color_ = pal.text;
    text3_color_ = pal.text3;
    const float s = scale;
    Ctx c{compositor, dc, brush_, s};
    bar_painter_.SetCompositor(compositor);
    bar_painter_.SetScale(scale);
    bar_painter_.BeginFrame(theme, vm.high_contrast);
    bar_scale_ = scale;
    bar_now_ = now_ms;

    glass_drawn_ = false;
    if (dock_value_ > 0.0f && column.right > column.left) {
        // The column: pane color, a hairline on the right; content laid out at
        // its full width and clipped while the column grows.
        D2D1_RECT_F full = column;
        full.right = column.left + column_content_width;
        dc->PushAxisAlignedClip(column, D2D1_ANTIALIAS_MODE_ALIASED);
        if (glass_) {
            // Over a wallpaper: a card on the sheet like the pane, inset 6 from
            // the rail and the title strip, 2 from its right edge, 4 above the
            // status bar.
            Palette gp = pal;
            ApplyGlass(gp, theme, vm.dark, glass_alpha_, glass_frame_);
            const D2D1_RECT_F card = D2D1::RectF(std::round(column.left + 6.0f * s), std::round(column.top + 6.0f * s),
                                                 std::round(column.right - 2.0f * s), std::round(column.bottom - 4.0f * s));
            const float radius = theme.radius_control * s;
            if (card.right - card.left > 2.0f * radius && card.bottom > card.top) {
                D2D1_RECT_F inner = card;
                inner.right = card.left + column_content_width - 8.0f * s;
                c.Fill(card, radius, gp.glass_panel);
                ID2D1RoundedRectangleGeometry* clip = nullptr;
                ID2D1Factory* factory = nullptr;
                dc->GetFactory(&factory);
                if (factory) {
                    factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(card, radius, radius), &clip);
                    factory->Release();
                }
                if (clip) {
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                                  nullptr);
                }
                DrawContent(compositor, vm, gp, inner, s, now_ms);
                if (clip) {
                    dc->PopLayer();
                    clip->Release();
                }
                c.Stroke(card, radius, gp.glass_panel_line);
            }
            glass_drawn_ = true;
        } else {
            c.Fill(column, 0.0f, pal.pane);
            DrawContent(compositor, vm, pal, full, s, now_ms);
        }
        dc->PopAxisAlignedClip();
        if (!glass_drawn_) c.Fill(D2D1::RectF(column.right - 1.0f, column.top, column.right, column.bottom), 0.0f, pal.line);
        panel_rect_ = column;
        input_placed_ = mode_ == AiPanelMode::Docked && !dock_anim_ && open_;
        drawn_ = true;
    }
    if (float_value_ > 0.0f) {
        const float w = (std::min)(kFloatingWidthDip * s, floating_area.right - floating_area.left - 20.0f * s);
        const float h = (std::min)(kFloatingHeightDip * s, floating_area.bottom - floating_area.top - 16.0f * s);
        const float left = std::round(floating_area.left + 10.0f * s);
        const float bottom = std::round(floating_area.bottom - 6.0f * s);
        const D2D1_RECT_F card = D2D1::RectF(left, bottom - h, left + w, bottom);
        const float p = float_value_;
        // opacity, translateY(14px) and scale(.96) about 12% / 100%.
        const float k = 0.96f + 0.04f * p;
        const float dy = (1.0f - p) * 14.0f * s;
        const D2D1_POINT_2F origin = D2D1::Point2F(card.left + w * 0.12f, card.bottom);
        D2D1_MATRIX_3X2_F saved;
        dc->GetTransform(&saved);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, origin) * D2D1::Matrix3x2F::Translation(0.0f, dy) * saved);
        const float radius = 14.0f * s;
        c.Shadow(card, radius, 46.0f * s, 18.0f * s, Alpha(pal.shadow, p));
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), p, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                      nullptr);
        ID2D1RoundedRectangleGeometry* clip = nullptr;
        ID2D1Factory* factory = nullptr;
        dc->GetFactory(&factory);
        if (factory) {
            factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(card, radius, radius), &clip);
            factory->Release();
        }
        c.Fill(card, radius, pal.pane);
        if (clip) {
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                          nullptr);
        }
        DrawContent(compositor, vm, pal, card, s, now_ms);
        if (clip) {
            dc->PopLayer();
            clip->Release();
        }
        c.Stroke(card, radius, pal.line);
        dc->PopLayer();
        dc->SetTransform(saved);
        if (mode_ == AiPanelMode::Floating && open_) {
            panel_rect_ = card;
            input_placed_ = !float_anim_;
            drawn_ = true;
        }
    }
    if (drawn_ && open_) {
        DrawEffortMenu(compositor, vm, pal, s, now_ms);
        DrawVoiceMenu(compositor, vm, pal, s, now_ms);
    }
}

void AiPanelView::DrawContent(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                              const D2D1_RECT_F& panel, float s, uint64_t now_ms) {
    Ctx c{compositor, compositor->Dc(), brush_, s};
    DrawHeader(compositor, vm, pal, panel, s);
    // Composer: margin 6 12 12 (its quote chip measured first).
    MeasureQuote(compositor, vm, panel.right - panel.left - 24.0f * s - 2.0f * s - 22.0f * s, s);
    const float ch = ComposerHeight(vm, s);
    const D2D1_RECT_F box = D2D1::RectF(panel.left + 12.0f * s, panel.bottom - 12.0f * s - ch,
                                        panel.right - 12.0f * s, panel.bottom - 12.0f * s);
    conversation_rect_ = D2D1::RectF(panel.left, panel.top + kHeadDip * s, panel.right, box.top - 6.0f * s);
    // 55 P3: 历史对话 slides in from the left (.32 s); what it covers is not drawn.
    hist_k_ = HistorySlide(vm.history.open, now_ms);
    hist_open_drawn_ = vm.history.open;
    const bool hist_shown = hist_k_ > 0.001f;
    if (hist_shown) {
        const float edge = panel.left + (panel.right - panel.left) * hist_k_;
        c.dc->PushAxisAlignedClip(D2D1::RectF(edge, panel.top + kHeadDip * s, (std::max)(edge, panel.right), panel.bottom),
                                  D2D1_ANTIALIAS_MODE_ALIASED);
    }
    c.dc->PushAxisAlignedClip(conversation_rect_, D2D1_ANTIALIAS_MODE_ALIASED);
    suggestion_rects_.clear();
    go_settings_ = {};
    conv_hits_.clear();
    jump_shown_ = false;   // 55 P4: set again by DrawConversation
    turn_rects_.clear();
    turn_edit_rect_ = {};
    turn_edit_placed_ = false;
    if (vm.empty) DrawEmptyState(compositor, vm, pal, conversation_rect_, s, now_ms);
    else DrawConversation(compositor, vm, pal, conversation_rect_, s, now_ms);
    c.dc->PopAxisAlignedClip();
    DrawComposer(compositor, vm, pal, box, s, now_ms);
    DrawSelectionBar(compositor, vm, pal, s);
    if (hist_shown) c.dc->PopAxisAlignedClip();
    DrawHistory(compositor, vm, pal, panel, s, now_ms);
}

void AiPanelView::DrawHeader(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                             const D2D1_RECT_F& panel, float s) {
    Ctx c{compositor, compositor->Dc(), brush_, s};
    head_rect_ = D2D1::RectF(panel.left, panel.top, panel.right, panel.top + kHeadDip * s);
    // .a-head: padding 0 10 0 16; spark 18; gap 8; title 14.5 semibold.
    const float cy = head_rect_.top + kHeadDip * s * 0.5f;
    c.Icon(AiIcon::Spark, panel.left + 16.0f * s, cy - 9.0f * s, 18.0f * s, pal.accent);
    const float tx = panel.left + (16.0f + 18.0f + 8.0f) * s;
    // Buttons from the right: close, mode, new (32 px, gap 8).
    const float bs = 32.0f * s;
    float bx = panel.right - 10.0f * s - bs;
    close_button_ = D2D1::RectF(bx, cy - bs * 0.5f, bx + bs, cy + bs * 0.5f);
    bx -= bs + 8.0f * s;
    mode_button_ = D2D1::RectF(bx, cy - bs * 0.5f, bx + bs, cy + bs * 0.5f);
    bx -= bs + 8.0f * s;
    new_button_ = D2D1::RectF(bx, cy - bs * 0.5f, bx + bs, cy + bs * 0.5f);
    bx -= bs + 8.0f * s;   // 55 P3: 历史对话, left of 新对话
    hist_button_ = D2D1::RectF(bx, cy - bs * 0.5f, bx + bs, cy + bs * 0.5f);
    c.Text(vm.title, formats_.title, D2D1::RectF(tx, head_rect_.top, hist_button_.left - 8.0f * s, head_rect_.bottom),
           pal.text);
    struct Button { const D2D1_RECT_F* r; AiPanelPart part; AiIcon icon; };
    const Button buttons[] = {
        {&hist_button_, AiPanelPart::History, AiIcon::History},
        {&new_button_, AiPanelPart::NewChat, AiIcon::Plus},
        {&mode_button_, AiPanelPart::Mode, mode_ == AiPanelMode::Docked ? AiIcon::Float : AiIcon::Dock},
        {&close_button_, AiPanelPart::Close, AiIcon::ChevronDown},
    };
    for (const Button& b : buttons) {
        const bool hot = vm.hover.part == b.part;
        const bool down = hot && vm.pressed.part == b.part;
        const bool on = b.part == AiPanelPart::History && vm.history.open;   // .hb.on
        const D2D1_COLOR_F hover = pal.glass ? pal.glass_hover : pal.hover;
        if (hot || on) c.Fill(*b.r, 8.0f * s, down ? Mix(hover, pal.text, 0.06f) : hover);
        const float is = 17.0f * s;
        c.Icon(b.icon, (b.r->left + b.r->right - is) * 0.5f, (b.r->top + b.r->bottom - is) * 0.5f, is,
               hot || on ? pal.text : pal.text2);
    }
    c.Fill(D2D1::RectF(panel.left, head_rect_.bottom - 1.0f, panel.right, head_rect_.bottom), 0.0f,
           pal.glass ? pal.glass_line : pal.line2);
}

void AiPanelView::DrawEmptyState(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal,
                                 const D2D1_RECT_F& area, float s, uint64_t now_ms) {
    Ctx c{compositor, compositor->Dc(), brush_, s};
    // .conv scrolls (overflow auto): in a short window the welcome scrolls
    // like the conversation instead of running under the composer.
    if (!was_empty_) {
        scroll_ = 0.0f;   // a new chat starts at the top
        scroll_anim_ = false;
    }
    was_empty_ = true;
    view_h_ = area.bottom - area.top;
    if (scroll_anim_) {
        const float p = std::clamp(static_cast<float>(now_ms - scroll_start_) / 160.0f, 0.0f, 1.0f);
        scroll_ = scroll_from_ + (scroll_to_ - scroll_from_) * EaseDesign(p);
        if (p >= 1.0f) scroll_anim_ = false;
        else conv_anim_ = true;
    }
    const float top = area.top - scroll_;
    // .conv padding 16 18 10, .empty padding 26 6 0.
    const float x0 = area.left + 24.0f * s;
    const float x1 = area.right - 24.0f * s;
    // The setup state leaves out the big spark (the header has one) and the
    // safety line, so it fits a common window without scrolling.
    float y = top + (16.0f + (vm.setup ? 12.0f : 26.0f)) * s;
    // Breathing spark: scale .88 and 45 deg at the middle of a 3.2 s cycle.
    if (!vm.setup) {
        // Nine breaths after opening, then it rests (a 30 fps repaint for as
        // long as the panel stays open would cost an idle window CPU).
        const uint64_t since = now_ms - opened_at_;
        const bool breathe = motion::SystemAnimationsEnabled() && since < 9 * 3200;
        const float phase = static_cast<float>(since % 3200) / 3200.0f;
        const float b = breathe ? 0.5f - 0.5f * std::cos(phase * 2.0f * kPi) : 0.0f;
        AiIconPose pose;
        pose.scale = 1.0f - 0.12f * b;
        pose.rotate_deg = 45.0f * b;
        c.Icon(AiIcon::Spark, x0, y, 44.0f * s, pal.accent, pose);
        if (breathe) decor_ = true;
        y += (44.0f + 16.0f) * s;
    }
    if (vm.setup) {
        // No model service yet: what the assistant needs, the services it
        // works with, and the way to Settings > AI 助手.
        const float th = 28.0f * s;
        c.Text(vm.setup_title, formats_.greeting, D2D1::RectF(x0, y, x1, y + th), pal.text);
        y += th + 6.0f * s;
        {
            TextOptions o;
            o.wrap = true;
            o.line_height = 20.0f * s;
            const float h = c.Text(vm.setup_intro, formats_.body13, D2D1::RectF(x0, y, x1, y + 200.0f * s), pal.text2, o);
            y += std::ceil(h) + 12.0f * s;
        }
        const float mark = 20.0f * s, gap = 10.0f * s;
        float lx = x0;
        for (const std::string& id : vm.setup_marks) {
            if (lx + mark > x1) {
                lx = x0;
                y += mark + 8.0f * s;
            }
            if (mark_painter_) mark_painter_(id, D2D1::RectF(lx, y, lx + mark, y + mark));
            lx += mark + gap;
        }
        if (!vm.setup_more.empty()) {
            const float mw = c.Measure(vm.setup_more, formats_.caption);
            if (lx + mw > x1) {
                lx = x0;
                y += mark + 8.0f * s;
            }
            c.Text(vm.setup_more, formats_.caption, D2D1::RectF(lx, y, lx + mw + 2.0f * s, y + mark), pal.text3);
        }
        y += mark + 18.0f * s;
        const float bw = c.Measure(vm.setup_go, formats_.body13b) + 32.0f * s;
        const D2D1_RECT_F go = D2D1::RectF(x0, y, (std::min)(x0 + bw, x1), y + 36.0f * s);
        D2D1_RECT_F shown = go;
        shown.top = (std::max)(shown.top, area.top);
        shown.bottom = (std::min)(shown.bottom, area.bottom);
        go_settings_ = shown.bottom > shown.top ? shown : D2D1_RECT_F{};
        const bool hot = vm.hover.part == AiPanelPart::GoSettings;
        c.Fill(go, 8.0f * s, hot ? Mix(pal.accent, pal.text, 0.12f) : pal.accent);
        TextOptions o;
        o.align = DWRITE_TEXT_ALIGNMENT_CENTER;
        c.Text(vm.setup_go, formats_.body13b, go, pal.on_accent, o);
        y = go.bottom - 18.0f * s;   // no safety line: the height ends at the button
    } else {
    const float gh = 28.0f * s;
    c.Text(vm.greeting, formats_.greeting, D2D1::RectF(x0, y, x1, y + gh), pal.text);
    y += gh + 6.0f * s;
    {
        TextOptions o;
        o.wrap = true;
        o.line_height = 20.0f * s;
        const float h = c.Text(vm.intro, formats_.body13, D2D1::RectF(x0, y, x1, y + 200.0f * s), pal.text2, o);
        y += std::ceil(h) + 18.0f * s;
    }
    // Suggestions: padding 11 12, radius 11, gap 12, tile 30, margin-bottom 8.
    for (size_t i = 0; i < vm.suggestions.size(); ++i) {
        const AiSuggestionView& sug = vm.suggestions[i];
        const D2D1_RECT_F r = D2D1::RectF(x0, y, x1, y + 60.0f * s);
        // Hits only where the card shows (indices stay aligned).
        D2D1_RECT_F v = r;
        v.top = (std::max)(v.top, area.top);
        v.bottom = (std::min)(v.bottom, area.bottom);
        suggestion_rects_.push_back(v.bottom > v.top ? v : D2D1_RECT_F{});
        const bool hot = vm.hover.part == AiPanelPart::Suggestion && vm.hover.index == static_cast<int>(i);
        const bool down = hot && vm.pressed.part == AiPanelPart::Suggestion;
        D2D1_MATRIX_3X2_F saved;
        if (down) {
            c.dc->GetTransform(&saved);
            const D2D1_POINT_2F mid = D2D1::Point2F((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
            c.dc->SetTransform(D2D1::Matrix3x2F::Scale(0.98f, 0.98f, mid) * saved);
        }
        if (pal.glass) {
            c.Fill(r, 11.0f * s, pal.glass_card);
            if (hot) c.Fill(r, 11.0f * s, Alpha(pal.accent, vm.dark ? 0.12f : 0.08f));
            c.Stroke(r, 11.0f * s, hot ? pal.accent : pal.glass_card_line);
        } else {
            c.Fill(r, 11.0f * s, hot ? pal.accent_tint : pal.card);
            c.Stroke(r, 11.0f * s, hot ? pal.accent : pal.line);
        }
        const int tint = static_cast<int>(sug.tint);
        const D2D1_RECT_F tile = D2D1::RectF(r.left + 12.0f * s, (r.top + r.bottom) * 0.5f - 15.0f * s,
                                             r.left + 42.0f * s, (r.top + r.bottom) * 0.5f + 15.0f * s);
        c.Fill(tile, 8.0f * s, pal.tint_bg[tint]);
        c.Icon(sug.icon, tile.left + 7.0f * s, tile.top + 7.0f * s, 16.0f * s, pal.tint_fg[tint]);
        const float tx = tile.right + 12.0f * s;
        const float tr = r.right - (12.0f + 14.0f + 8.0f) * s;
        c.Text(sug.title, formats_.body, D2D1::RectF(tx, r.top + 10.0f * s, tr, r.top + 30.0f * s), pal.text);
        c.Text(sug.detail, formats_.caption, D2D1::RectF(tx, r.top + 31.0f * s, tr, r.top + 49.0f * s), pal.text3);
        c.Icon(AiIcon::ChevronRight, r.right - 26.0f * s, (r.top + r.bottom) * 0.5f - 7.0f * s, 14.0f * s, pal.text3);
        if (down) c.dc->SetTransform(saved);
        y = r.bottom + 8.0f * s;
    }
    }
    // .safe: margin-top 14 (after the card's 8), shield 14, gap 7, 12 px text3.
    if (!vm.setup) {
        y += 6.0f * s;
        c.Icon(AiIcon::Shield, x0 + 2.0f * s, y + 2.0f * s, 14.0f * s, pal.text3);
        c.Text(vm.safe_note, formats_.caption, D2D1::RectF(x0 + 23.0f * s, y, x1, y + 18.0f * s), pal.text3);
    }
    // Height for the wheel (.conv padding-bottom 10) and the overlay bar.
    content_h_ = y + 18.0f * s + 10.0f * s - top;
    const float max_scroll = (std::max)(0.0f, content_h_ - view_h_);
    if (scroll_ > max_scroll) scroll_ = max_scroll;
    if (max_scroll > 0.5f)
        DrawBar(0, D2D1::RectF(area.right - 12.0f * s, area.top + 4.0f * s, area.right - 2.0f * s,
                               area.top + view_h_ - 4.0f * s), scroll_, view_h_, content_h_);
}

namespace {
// .cp-in: min-height 42, 21 px lines, padding 2; grows to six lines.
float InputHeight(const AiPanelViewModel& vm, int input_lines, float s) {
    const int lines = std::clamp(input_lines, 1, 6);
    const float line = vm.composer_line_px > 0.0f ? vm.composer_line_px : 21.0f * s;
    return std::round((std::max)(42.0f * s, line * static_cast<float>(lines) + 4.0f * s));
}
} // namespace

float AiPanelView::ComposerHeight(const AiPanelViewModel& vm, float s) const {
    // padding 10 / 8, context row 24 + 6, input, controls row 32, 1 px borders.
    const float quote = !vm.quote.empty() && rich_ ? rich_->quote_h : 0.0f;
    return std::round((2.0f + 10.0f + (vm.context_visible ? 30.0f : 0.0f) + 32.0f + 8.0f) * s) + quote +
           InputHeight(vm, InputLines(vm), s);
}

void AiPanelView::DrawComposer(Compositor* compositor, const AiPanelViewModel& vm, const Palette& base,
                               const D2D1_RECT_F& box, float s, uint64_t now_ms) {
    Ctx c{compositor, compositor->Dc(), brush_, s};
    // Glass: translucent at rest (the panel draws the text); solid while the
    // EDIT has focus, since the EDIT paints an opaque background.
    const bool clear = base.glass && !vm.composer_focused;
    Palette pal = base;
    if (clear) pal.hover = base.glass_hover;
    composer_rect_ = box;
    const float radius = 16.0f * s;
    const bool ring = vm.composer_focused || vm.voice.listening;   // .composer.listen: the same ring
    if (ring) {
        // Focus: accent border and a 3 px ring at 16%; only outside the box
        // when the box lets the panel through.
        if (clear) c.Stroke(Inflate(box, 3.0f * s), radius + 3.0f * s, Alpha(pal.accent, 0.16f), 3.0f * s);
        else c.Fill(Inflate(box, 3.0f * s), radius + 3.0f * s, Alpha(pal.accent, 0.16f));
    } else if (!clear) {
        c.Shadow(box, radius, 8.0f * s, 2.0f * s, Hex(0x000000, vm.dark ? 0.20f : 0.05f));
    }
    c.Fill(box, radius, clear ? pal.glass_input : pal.card);
    c.Stroke(box, radius, ring ? pal.accent : clear ? pal.glass_input_line : pal.line);
    const float left = box.left + 1.0f * s + 12.0f * s;
    const float right = box.right - 1.0f * s - 10.0f * s;
    float y = box.top + 1.0f * s + 10.0f * s;
    context_chip_ = {};
    context_remove_ = {};
    if (vm.context_visible) {
        // .ctxp: height 24, radius 12, padding 0 9 0 7, icon 13, gap 6, x 11.
        const float tw = c.Measure(vm.context_text, formats_.caption);
        const float w = (7.0f + 13.0f + 6.0f) * s + tw + (6.0f + 11.0f + 9.0f) * s;
        context_chip_ = D2D1::RectF(left, y, left + w, y + 24.0f * s);
        c.Fill(context_chip_, 12.0f * s, clear ? pal.glass_card : pal.card);
        c.Stroke(context_chip_, 12.0f * s, clear ? pal.glass_card_line : pal.line);
        c.Icon(AiIcon::FolderFilled, left + 7.0f * s, y + 5.5f * s, 13.0f * s, pal.folder_icon);
        c.Text(vm.context_text, formats_.caption, D2D1::RectF(left + 26.0f * s, y, left + 26.0f * s + tw + 1.0f, y + 24.0f * s),
               pal.text2);
        const float xx = context_chip_.right - 9.0f * s - 11.0f * s;
        context_remove_ = D2D1::RectF(xx - 4.0f * s, y + 2.0f * s, xx + 15.0f * s, y + 22.0f * s);
        const bool hot = vm.hover.part == AiPanelPart::ContextRemove;
        if (hot) c.Fill(D2D1::RectF(xx - 3.0f * s, y + 4.0f * s, xx + 14.0f * s, y + 20.0f * s), 8.0f * s, pal.hover);
        c.Icon(AiIcon::Close, xx, y + 6.5f * s, 11.0f * s, hot ? pal.text : pal.text3);
        y += 30.0f * s;
    }
    DrawQuoteChip(compositor, vm, pal, left, right, y, s);
    const float input_h = InputHeight(vm, InputLines(vm), s);
    // .cp-in padding 2: the EDIT sits inside; the placeholder is drawn here
    // too, for the frames the EDIT is hidden (opening, model menu).
    input_rect_ = D2D1::RectF(left + 2.0f * s, y + 2.0f * s, right - 2.0f * s, y + input_h - 2.0f * s);
    const float line = vm.composer_line_px > 0.0f ? vm.composer_line_px : 21.0f * s;
    if (vm.voice.listening && vm.voice.available) {
        DrawVoiceInput(compositor, vm, pal, line, s, now_ms);
    } else if (vm.composer_empty) {
        c.Text(vm.placeholder, formats_.body, D2D1::RectF(input_rect_.left, input_rect_.top, input_rect_.right,
                                                         input_rect_.top + line), pal.text3);
    } else if (vm.draw_composer_text) {
        c.dc->PushAxisAlignedClip(input_rect_, D2D1_ANTIALIAS_MODE_ALIASED);
        TextOptions o;
        o.wrap = true;
        o.line_height = line;
        c.Text(vm.composer_text, formats_.body, input_rect_, pal.text, o);
        c.dc->PopAxisAlignedClip();
    }
    y += input_h;
    // Controls row (height 32): 添加文件 · model button · (mic) · send. In a
    // narrow panel 添加文件 drops its label first, then the model name shortens.
    const float row_mid = y + 16.0f * s;
    const float model_r = vm.voice.available ? right - 32.0f * s - 10.0f * s - 46.0f * s - 10.0f * s
                                             : right - 32.0f * s - 6.0f * s;
    const float model_lw = (std::min)(c.Measure(vm.model_label, formats_.model), 180.0f * s);
    const float model_ew = vm.effort_label.empty() ? 0.0f : c.Measure(vm.effort_label, formats_.small125);
    const float model_pad = (8.0f + 4.0f + 12.0f + 8.0f) * s;
    float model_fit = model_lw;
    {
        const float tw = c.Measure(vm.add_files, formats_.caption);
        const float full = (8.0f + 14.0f + 5.0f + 8.0f) * s + tw;
        const bool compact = left - 4.0f * s + full + 6.0f * s + model_pad + model_lw + model_ew > model_r;
        const float w = compact ? 30.0f * s : full;
        add_files_ = D2D1::RectF(left - 4.0f * s, row_mid - 14.0f * s, left - 4.0f * s + w, row_mid + 14.0f * s);
        model_fit = std::clamp(model_r - (add_files_.right + 6.0f * s) - model_pad - model_ew, 30.0f * s, model_lw);
        const bool hot = vm.hover.part == AiPanelPart::AddFiles;
        if (hot) c.Fill(add_files_, 8.0f * s, pal.hover);
        c.Icon(AiIcon::At, add_files_.left + 8.0f * s, row_mid - 7.0f * s, 14.0f * s, pal.text2);
        if (!compact)
            c.Text(vm.add_files, formats_.caption, D2D1::RectF(add_files_.left + 27.0f * s, add_files_.top,
                                                             add_files_.right, add_files_.bottom), pal.text2);
    }
    // Send: 32 round.
    send_button_ = D2D1::RectF(right - 32.0f * s, row_mid - 16.0f * s, right, row_mid + 16.0f * s);
    {
        const bool hot = vm.hover.part == AiPanelPart::Send;
        const bool down = hot && vm.pressed.part == AiPanelPart::Send && vm.send != AiSendState::Dim;
        const D2D1_POINT_2F mid = D2D1::Point2F((send_button_.left + send_button_.right) * 0.5f, row_mid);
        D2D1_MATRIX_3X2_F saved;
        c.dc->GetTransform(&saved);
        if (down) c.dc->SetTransform(D2D1::Matrix3x2F::Scale(0.88f, 0.88f, mid) * saved);
        D2D1_COLOR_F fill = pal.accent, ink = pal.on_accent;
        if (vm.send == AiSendState::Dim) { fill = pal.line; ink = pal.text3; }
        else if (vm.send == AiSendState::Busy) { fill = pal.text; ink = pal.card; }
        else if (hot) fill = Mix(pal.accent, pal.text, 0.12f);
        c.dc->FillEllipse(D2D1::Ellipse(mid, 16.0f * s, 16.0f * s), c.B(fill));
        if (vm.send == AiSendState::Busy) {
            c.Icon(AiIcon::Stop, mid.x - 8.0f * s, mid.y - 8.0f * s, 16.0f * s, ink);
            // Rotating accent arc 4 px outside (1 s per turn).
            const float a = static_cast<float>(now_ms % 1000) / 1000.0f * 2.0f * kPi;
            const float r = 18.0f * s + 1.0f * s;
            ID2D1Factory* factory = nullptr;
            c.dc->GetFactory(&factory);
            ID2D1PathGeometry* arc = nullptr;
            if (factory && SUCCEEDED(factory->CreatePathGeometry(&arc))) {
                ID2D1GeometrySink* sink = nullptr;
                if (SUCCEEDED(arc->Open(&sink))) {
                    const float a0 = a - kPi * 0.25f, a1 = a + kPi * 0.25f;
                    sink->BeginFigure(D2D1::Point2F(mid.x + r * std::cos(a0), mid.y + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(mid.x + r * std::cos(a1), mid.y + r * std::sin(a1)),
                                                  D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    sink->Release();
                    c.dc->DrawGeometry(arc, c.B(pal.accent), 2.0f * s);
                }
                arc->Release();
            }
            if (factory) factory->Release();
            decor_ = true;
        } else {
            c.Icon(AiIcon::Send, mid.x - 8.0f * s, mid.y - 8.0f * s, 16.0f * s, ink);
        }
        c.dc->SetTransform(saved);
    }
    // .vsel (the mic 30 + its chevron 16, height 30): margin-right 6 + the
    // row's gap 4 left of the send button; the model button 10 left of it.
    float model_right = send_button_.left - 6.0f * s;
    mic_button_ = mic_menu_ = {};
    if (vm.voice.available) {
        const float r = send_button_.left - 10.0f * s;
        mic_menu_ = D2D1::RectF(r - 16.0f * s, row_mid - 15.0f * s, r, row_mid + 15.0f * s);
        mic_button_ = D2D1::RectF(mic_menu_.left - 30.0f * s, row_mid - 15.0f * s, mic_menu_.left, row_mid + 15.0f * s);
        DrawMic(compositor, vm, pal, s, now_ms);
        model_right = mic_button_.left - 10.0f * s;
    }
    // Model button: label (medium) + effort (text2) + chevron 12, padding 0 8.
    {
        // A long id is cut with an ellipsis (the menu shows it whole).
        const float lw = model_fit;
        const float ew = model_ew;
        const float w = model_pad + lw + ew;
        const float r = model_right;
        model_button_ = D2D1::RectF(r - w, row_mid - 14.0f * s, r, row_mid + 14.0f * s);
        const bool hot = vm.hover.part == AiPanelPart::ModelButton;
        if (hot || vm.menu_open) c.Fill(model_button_, 8.0f * s, pal.hover);
        float x = model_button_.left + 8.0f * s;
        c.Text(vm.model_label, formats_.model, D2D1::RectF(x, model_button_.top, x + lw + 1.0f, model_button_.bottom), pal.text);
        x += lw;
        if (ew > 0.0f) {
            c.Text(vm.effort_label, formats_.small125, D2D1::RectF(x, model_button_.top, x + ew + 1.0f, model_button_.bottom),
                   pal.text2);
            x += ew;
        }
        c.Icon(AiIcon::ChevronDown, x + 4.0f * s, row_mid - 6.0f * s, 12.0f * s, pal.text2);
    }
}

// 思考强度 popover (after Codex): the level and 「model ›」 on top, ↺ back to
// the default, the stepped slider (as many stops as the model has), and the
// level's line under it. A model that is only on or off shows a switch row
// instead; one that always thinks shows 始终思考 and why, with no control.
void AiPanelView::DrawEffortMenu(Compositor* compositor, const AiPanelViewModel& vm, const Palette& pal, float s,
                                 uint64_t now_ms) {
    const bool levels = vm.effort_mode == AiEffortMode::Levels;
    const bool toggle = vm.effort_mode == AiEffortMode::Toggle;
    const bool fixed = vm.effort_mode == AiEffortMode::Fixed;
    const bool want = vm.menu_open && (fixed || vm.effort_count >= 2);
    const bool opening = want && !menu_shown_;
    if (want != menu_shown_) {
        menu_shown_ = want;
        menu_at_ = now_ms;
    }
    menu_rect_ = {};
    menu_link_ = {};
    menu_slider_ = {};
    menu_reset_ = {};
    toggle_target_ = -1;
    const int stops = levels ? std::clamp(vm.effort_count, 2, 7) : 0;
    const int chosen = fixed ? 0 : std::clamp(vm.effort, 0, (levels ? stops : 2) - 1);
    if (!levels) {
        slider_stops_ = 0;
        slider_target_ = -1;
        slider_anim_ = false;
        slider_drag_ = false;
    } else if (opening || stops != slider_stops_ || slider_target_ < 0) {
        slider_pos_ = static_cast<float>(chosen);
        slider_target_ = chosen;
        slider_anim_ = false;
        slider_drag_ = false;
    }
    if (levels) slider_stops_ = stops;
    if (levels && !slider_drag_) {
        if (chosen != slider_target_) {   // a key, ↺, or the release's commit
            slider_from_ = slider_pos_;
            slider_target_ = chosen;
            slider_at_ = now_ms;
            slider_anim_ = animate_;
            if (!animate_) slider_pos_ = static_cast<float>(chosen);
        }
        if (slider_anim_) {
            constexpr float kSnapMs = 240.0f;
            const float u = std::clamp(static_cast<float>(now_ms - slider_at_) / kSnapMs, 0.0f, 1.0f);
            // Ease out with a slight overshoot onto the stop.
            const float c1 = 1.1f, c3 = c1 + 1.0f, v = u - 1.0f;
            const float e = 1.0f + c3 * v * v * v + c1 * v * v;
            slider_pos_ = slider_from_ + (static_cast<float>(slider_target_) - slider_from_) * e;
            if (u >= 1.0f) {
                slider_anim_ = false;
                slider_pos_ = static_cast<float>(slider_target_);
            } else {
                decor_ = true;
            }
        }
    }
    const float t = animate_ ? std::clamp(static_cast<float>(now_ms - menu_at_) / static_cast<float>(kMenuMs), 0.0f, 1.0f) : 1.0f;
    const float p = menu_shown_ ? EaseDesign(t) : 1.0f - EaseDesign(t);
    if (p <= 0.0f || (!fixed && vm.effort_count < 2)) return;
    if (t < 1.0f) decor_ = true;
    Ctx c{compositor, compositor->Dc(), brush_, s};
    // The level under the knob while it is dragged, else the chosen one.
    const int shown = levels && slider_drag_ ? std::clamp(static_cast<int>(std::lround(slider_pos_)), 0, stops - 1) : chosen;
    const std::wstring& name = vm.effort_names[static_cast<size_t>(shown)];
    const std::wstring& detail = vm.effort_details[static_cast<size_t>(shown)];

    // Geometry: width 320; right = button right + 6, bottom = button top - 8.
    const float w = 320.0f * s;
    const float pad_x = 16.0f * s;
    const float top_h = 42.0f * s;        // the level (20) and 「model ›」 (21)
    const float gap = 6.0f * s;
    const float slider_h = fluent::StepSliderHeight(s);
    const float line_h = 16.0f * s;
    // The line keeps one height for every level, so dragging never resizes.
    const float text_w = w - 2.0f * pad_x;
    int lines = 0;
    const int lined = levels ? stops : toggle ? 2 : 0;   // 始终思考's line sits in its box
    for (int i = 0; i < lined; ++i) {
        const std::wstring& d = vm.effort_details[static_cast<size_t>(i)];
        if (d.empty()) continue;
        const float dh = c.MeasureWrapped(d, formats_.caption, text_w, line_h);
        lines = (std::max)(lines, std::clamp(static_cast<int>(std::ceil(dh / line_h - 0.25f)), 1, 3));
    }
    const float caption_h = lines > 0 ? 4.0f * s + line_h * static_cast<float>(lines) : 0.0f;
    // 这个模型始终会思考 / 只能开或关, in the faintest text under the line.
    float note_h = 0.0f;
    if (!vm.effort_note.empty()) {
        const float dh = c.MeasureWrapped(vm.effort_note, formats_.caption, text_w, line_h);
        note_h = 2.0f * s + line_h * static_cast<float>(std::clamp(static_cast<int>(std::ceil(dh / line_h - 0.25f)), 1, 2));
    }
    // The body: the slider, the switch row, or 始终思考's box.
    const float row_h = 40.0f * s;
    const float box_pad = 10.0f * s;
    float body_h = slider_h;
    if (toggle) body_h = row_h;
    else if (fixed) {
        const float dh = c.MeasureWrapped(detail, formats_.caption, text_w - 2.0f * box_pad, line_h);
        body_h = 2.0f * box_pad +
                 line_h * static_cast<float>(std::clamp(static_cast<int>(std::ceil(dh / line_h - 0.25f)), 1, 4));
    }
    const float h = 10.0f * s + top_h + gap + body_h + caption_h + note_h + 10.0f * s;
    const float anchor = model_button_.right + 6.0f * s;
    const float bottom = model_button_.top - 6.0f * s;
    const float min_left = panel_rect_.left + 8.0f * s;
    const float right = std::round((std::max)(anchor, min_left + w));
    const D2D1_RECT_F box = D2D1::RectF(std::round(right - w), std::round(bottom - h), right, bottom);
    const D2D1_POINT_2F origin = D2D1::Point2F(box.left + (box.right - box.left) * 0.85f, box.bottom);
    const float k = 0.97f + 0.03f * p;
    D2D1_MATRIX_3X2_F saved;
    c.dc->GetTransform(&saved);
    c.dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, origin) * D2D1::Matrix3x2F::Translation(0.0f, (1.0f - p) * 6.0f * s) * saved);
    c.dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                           D2D1::IdentityMatrix(), p, nullptr, D2D1_LAYER_OPTIONS1_NONE),
                    nullptr);
    // The flyout surface of Pulse's menus (radius_flyout 12).
    const float radius = 12.0f * s;
    c.Shadow(box, radius, 40.0f * s, 16.0f * s, pal.shadow);
    c.Fill(box, radius, pal.card);
    c.Stroke(box, radius, pal.line);

    const float x0 = box.left + pad_x, x1 = box.right - pad_x;
    float y = box.top + 10.0f * s;
    TextOptions center;
    center.align = DWRITE_TEXT_ALIGNMENT_CENTER;
    // ↺ on the right; dim (and inert) at the default.
    const float rb = 26.0f * s;
    const D2D1_RECT_F reset = D2D1::RectF(x1 - rb + 6.0f * s, y - 2.0f * s, x1 + 6.0f * s, y - 2.0f * s + rb);
    const bool can_reset = !fixed && shown != std::clamp(vm.effort_default, 0, (levels ? stops : 2) - 1);
    const bool reset_hot = can_reset && vm.hover.part == AiPanelPart::MenuReset;
    if (reset_hot) c.Fill(reset, 8.0f * s, vm.pressed.part == AiPanelPart::MenuReset ? Mix(pal.hover, pal.text, 0.06f) : pal.hover);
    const float icon = 16.0f * s;
    if (!fixed)   // nothing to go back to
        c.Icon(AiIcon::Retry, (reset.left + reset.right - icon) * 0.5f, (reset.top + reset.bottom - icon) * 0.5f, icon,
               !can_reset ? Alpha(pal.text3, 0.45f) : reset_hot ? pal.text : pal.text2);
    // The level, in the accent.
    const float side = rb + 6.0f * s;
    c.Text(name, formats_.num15, D2D1::RectF(x0 + side, y, x1 - side, y + 20.0f * s), pal.accent, center);
    // 「model ›」: opens the model list.
    {
        const float chev = 12.0f * s;
        const float max_tw = (x1 - x0) - 2.0f * side - chev - 22.0f * s;
        const float tw = (std::min)(c.Measure(vm.model_label, formats_.body13), max_tw);
        const float lw = tw + 4.0f * s + chev + 18.0f * s;
        const float cx = (x0 + x1) * 0.5f;
        const D2D1_RECT_F link = D2D1::RectF(std::round(cx - lw * 0.5f), y + 21.0f * s, std::round(cx + lw * 0.5f),
                                             y + 42.0f * s);
        const bool hot = vm.hover.part == AiPanelPart::MenuModel;
        if (hot) c.Fill(link, (link.bottom - link.top) * 0.5f, vm.pressed.part == AiPanelPart::MenuModel
                                                                    ? Mix(pal.hover, pal.text, 0.06f) : pal.hover);
        const float tx = link.left + 9.0f * s;
        c.Text(vm.model_label, formats_.body13, D2D1::RectF(tx, link.top, tx + tw + 1.0f * s, link.bottom),
               hot ? pal.text : pal.text2);
        c.Icon(AiIcon::ChevronRight, tx + tw + 4.0f * s, (link.top + link.bottom - chev) * 0.5f, chev,
               hot ? pal.text2 : pal.text3);
        if (vm.menu_open) menu_link_ = link;
    }
    y += top_h + gap;
    TextOptions wrap = center;
    wrap.wrap = true;
    wrap.center_v = false;
    wrap.line_height = line_h;
    // The body.
    D2D1_RECT_F control = D2D1::RectF(x0, y, x1, y + body_h);
    const bool body_hot = vm.hover.part == AiPanelPart::MenuSlider;
    const D2D1_COLOR_F well = Mix(pal.card, pal.text, 0.035f);
    if (levels) {
        fluent::StepSliderSpec spec;
        spec.bounds = control;
        spec.stops = stops;
        spec.position = slider_pos_;
        spec.state.hovered = body_hot;
        spec.state.pressed = slider_drag_;
        bar_painter_.DrawStepSlider(spec);
        control = D2D1::RectF(control.left, control.top - 3.0f * s, control.right, control.bottom + 3.0f * s);
    } else if (toggle) {
        // 深度思考 and the shared switch; the whole row flips it.
        c.Fill(control, 8.0f * s, body_hot ? Mix(well, pal.text, vm.pressed.part == AiPanelPart::MenuSlider ? 0.06f : 0.03f) : well);
        const float sw = 42.0f * s, sh = 32.0f * s;
        c.Text(vm.effort_toggle_label, formats_.body13,
               D2D1::RectF(control.left + 12.0f * s, control.top, control.right - sw - 16.0f * s, control.bottom), pal.text);
        fluent::ControlState state{};
        state.checked = chosen > 0;
        state.hovered = body_hot;
        const float sy = std::round((control.top + control.bottom - sh) * 0.5f);
        bar_painter_.DrawSwitch(D2D1::RectF(control.right - 10.0f * s - sw, sy, control.right - 10.0f * s, sy + sh), L"", state);
        toggle_target_ = chosen > 0 ? 0 : 1;
    } else {
        c.Fill(control, 8.0f * s, well);
        TextOptions boxed = wrap;
        c.Text(detail, formats_.caption,
               D2D1::RectF(control.left + box_pad, control.top + box_pad, control.right - box_pad, control.bottom - box_pad),
               pal.text2, boxed);
    }
    y += body_h;
    if (lines > 0 && !detail.empty())
        c.Text(detail, formats_.caption, D2D1::RectF(x0, y + 4.0f * s, x1, y + caption_h), pal.text3, wrap);
    y += caption_h;
    if (note_h > 0.0f) c.Text(vm.effort_note, formats_.caption, D2D1::RectF(x0, y + 2.0f * s, x1, y + note_h), pal.text3, wrap);
    c.dc->PopLayer();
    c.dc->SetTransform(saved);
    if (vm.menu_open) {
        menu_rect_ = box;
        if (!fixed) menu_slider_ = control;
        if (can_reset) menu_reset_ = reset;
    }
}

bool AiPanelView::BeginSliderDrag(float x, float y) {
    if (!In(menu_rect_, x, y) || !In(menu_slider_, x, y) || slider_stops_ < 2) return false;
    slider_drag_ = true;
    slider_anim_ = false;
    slider_pos_ = fluent::StepSliderPositionAt(menu_slider_, slider_stops_, x, bar_scale_);
    return true;
}

int AiPanelView::DragSlider(float x) {
    if (!slider_drag_) return -1;
    slider_pos_ = fluent::StepSliderPositionAt(menu_slider_, slider_stops_, x, bar_scale_);
    return static_cast<int>(std::lround(slider_pos_));
}

int AiPanelView::EndSliderDrag() {
    if (!slider_drag_) return -1;
    slider_drag_ = false;
    const int snapped = std::clamp(static_cast<int>(std::lround(slider_pos_)), 0, (std::max)(slider_stops_ - 1, 0));
    slider_from_ = slider_pos_;
    slider_target_ = snapped;
    slider_at_ = motion::NowMs();
    slider_anim_ = animate_;
    if (!animate_) slider_pos_ = static_cast<float>(snapped);
    return snapped;
}

bool AiPanelView::EnsureBrush(ID2D1DeviceContext* dc) {
    if (brush_dc_ != dc || !brush_) {
        SafeRelease(brush_);
        brush_dc_ = nullptr;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush_))) return false;
        brush_dc_ = dc;
    }
    return true;
}

float AiStatusChipWidth(float label_w, float text_w, bool words, bool progress, float scale) {
    float w = (8.0f + 15.0f + 6.0f + 10.0f) * scale + label_w;
    if (words) w += 6.0f * scale + text_w;
    if (words && progress) w += (6.0f + 38.0f) * scale;
    return std::ceil(w);
}

void AiPanelView::DrawStatusChip(Compositor* compositor, const Theme& theme, const AiStatusChipView& chip,
                                 const D2D1_RECT_F& rect, float scale, uint64_t now_ms) {
    chip_decor_ = false;
    ID2D1DeviceContext* dc = compositor ? compositor->Dc() : nullptr;
    if (!dc || rect.right <= rect.left) return;
    EnsureFormats(compositor, scale);
    if (!formats_.small125 || !EnsureBrush(dc)) return;
    const Palette pal = MakePalette(theme, chip.dark);
    const float s = scale;
    Ctx c{compositor, dc, brush_, s};
    const bool busy = chip.state == AiChipState::Busy;
    const bool waiting = chip.state == AiChipState::Waiting;
    const bool done = chip.state == AiChipState::Done;
    // A failed turn.
    const bool failed = chip.state == AiChipState::Error;
    const bool unfinished = chip.state == AiChipState::Warn;
    const bool anim = motion::SystemAnimationsEnabled();
    if (waiting != chip_waiting_) { chip_waiting_ = waiting; chip_since_ = now_ms; }
    const D2D1_COLOR_F amber = chip.dark ? Hex(0xe3ab52) : Hex(0xa86c12);
    const D2D1_COLOR_F amber_bg = chip.dark ? Hex(0x3a3020) : Hex(0xfbf0d9);
    // .chip.press scale(.94)
    D2D1_MATRIX_3X2_F saved;
    dc->GetTransform(&saved);
    const D2D1_POINT_2F mid = D2D1::Point2F((rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f);
    if (chip.pressed) dc->SetTransform(D2D1::Matrix3x2F::Scale(0.94f, 0.94f, mid) * saved);
    const float radius = (rect.bottom - rect.top) * 0.5f;
    const D2D1_COLOR_F err = pal.bad;
    const D2D1_COLOR_F err_bg = pal.bad_bg;
    // The tone's ink: spark, words and the unread dot.
    const bool toned = waiting || unfinished || failed;
    const D2D1_COLOR_F tone = failed ? err : amber;
    D2D1_COLOR_F fill{0, 0, 0, 0};
    if (failed) fill = err_bg;
    else if (waiting || unfinished) fill = amber_bg;
    else if (busy || chip.open) fill = pal.accent_soft;
    else if (chip.hover) fill = pal.hover;
    if (fill.a > 0.0f && chip.hover && (busy || chip.open || toned)) fill = Mix(fill, pal.text, 0.05f);
    c.Fill(rect, radius, fill);
    if (waiting && anim) {
        // ::after ring: inset -3, 2 px amber, scale .96 -> 1.18, fading (1.6 s).
        const float t = static_cast<float>((now_ms - chip_since_) % 1600) / 1600.0f;
        const float e = 1.0f - (1.0f - t) * (1.0f - t);
        const float k = 0.96f + 0.22f * e;
        D2D1_RECT_F ring = Inflate(rect, 3.0f * s);
        const float hw = (ring.right - ring.left) * 0.5f * k, hh = (ring.bottom - ring.top) * 0.5f * k;
        ring = D2D1::RectF(mid.x - hw, mid.y - hh, mid.x + hw, mid.y + hh);
        c.Stroke(ring, hh, Alpha(amber, 0.55f * (1.0f - e)), 2.0f * s);
        chip_decor_ = true;
    }
    float x = rect.left + 8.0f * s;
    const float sp = 15.0f * s;
    AiIconPose pose;
    if (busy && anim) {
        // spark: rotate 0 -> 180 deg, scale 1 -> .78 -> 1 over 1.6 s.
        const float t = static_cast<float>(now_ms % 1600) / 1600.0f;
        const float e = 0.5f - 0.5f * std::cos(t * kPi);
        pose.rotate_deg = 180.0f * e;
        pose.scale = 1.0f - 0.22f * std::sin(t * kPi);
        chip_decor_ = true;
    }
    c.Icon(AiIcon::Spark, x, mid.y - sp * 0.5f, sp, toned ? tone : pal.accent, pose);
    x += sp + 6.0f * s;
    const std::wstring& label = chip.label.empty() ? std::wstring(L"AI") : chip.label;
    const float lw = c.Measure(label, formats_.small125b);
    c.Text(label, formats_.small125b, D2D1::RectF(x, rect.top, x + lw + 2.0f, rect.bottom), waiting ? amber : pal.text);
    x += lw + 6.0f * s;
    if (chip.words && !chip.text.empty()) {
        IDWriteTextFormat* f = toned || done ? formats_.small125b : formats_.small125;
        const D2D1_COLOR_F ink = toned ? tone : done ? pal.ok : pal.text2;
        const float bar = busy && chip.progress >= 0.0f ? (6.0f + 38.0f) * s : 0.0f;
        const float tr = rect.right - 10.0f * s - bar;
        c.Text(chip.text, f, D2D1::RectF(x, rect.top, (std::max)(x, tr), rect.bottom), ink);
        if (bar > 0.0f) {
            const D2D1_RECT_F track = D2D1::RectF(tr + 6.0f * s, mid.y - 2.0f * s, tr + 44.0f * s, mid.y + 2.0f * s);
            c.Fill(track, 2.0f * s, pal.line);
            D2D1_RECT_F v = track;
            v.right = track.left + (track.right - track.left) * std::clamp(chip.progress, 0.0f, 1.0f);
            if (v.right > v.left) c.Fill(v, 2.0f * s, pal.accent);
        }
    }
    if (chip.unread) {
        // 8 px ok dot at right 3 / top 2 with a 2 px ring of the bar color.
        const D2D1_POINT_2F d = D2D1::Point2F(rect.right - 3.0f * s - 4.0f * s, rect.top + 2.0f * s + 4.0f * s);
        dc->FillEllipse(D2D1::Ellipse(d, 6.0f * s, 6.0f * s), c.B(Over(theme.bg, pal.bg)));
        dc->FillEllipse(D2D1::Ellipse(d, 4.0f * s, 4.0f * s), c.B(failed ? err : unfinished ? amber : pal.ok));
    }
    dc->SetTransform(saved);
}

AiPanelView::Palette AiPanelPalette(const Theme& theme, bool dark) { return MakePalette(theme, dark); }

AiPanelHit AiPanelView::HitTest(float x, float y) const {
    AiPanelHit hit;
    if (!drawn_ || !open_) return hit;
    if (const AiPanelHit v = VoiceHitTest(x, y); v.part != AiPanelPart::None) return v;
    if (In(menu_rect_, x, y)) {
        hit.part = AiPanelPart::Menu;
        if (In(menu_link_, x, y)) return {AiPanelPart::MenuModel, -1};
        if (In(menu_reset_, x, y)) return {AiPanelPart::MenuReset, -1};
        if (In(menu_slider_, x, y)) {
            if (slider_stops_ < 2) return {AiPanelPart::MenuSlider, toggle_target_};   // the switch row
            const float at = fluent::StepSliderPositionAt(menu_slider_, slider_stops_, x, bar_scale_);
            return {AiPanelPart::MenuSlider, static_cast<int>(std::lround(at))};
        }
        return hit;
    }
    if (!In(panel_rect_, x, y)) return hit;
    if (In(hist_button_, x, y)) return {AiPanelPart::History, -1};
    if (In(new_button_, x, y)) return {AiPanelPart::NewChat, -1};
    if (hist_k_ > 0.001f && In(hist_rect_, x, y)) return HistoryHitTest(x, y);
    if (In(mode_button_, x, y)) return {AiPanelPart::Mode, -1};
    if (In(close_button_, x, y)) return {AiPanelPart::Close, -1};
    if (jump_shown_ && In(jump_rect_, x, y)) return {AiPanelPart::JumpLatest, -1};
    if (In(conversation_rect_, x, y)) {
        if (In(go_settings_, x, y)) return {AiPanelPart::GoSettings, -1};
        for (size_t i = 0; i < suggestion_rects_.size(); ++i)
            if (In(suggestion_rects_[i], x, y)) return {AiPanelPart::Suggestion, static_cast<int>(i)};
        for (auto it = conv_hits_.rbegin(); it != conv_hits_.rend(); ++it)
            if (In(it->rect, x, y)) return it->hit;
        return {AiPanelPart::Conversation, -1};
    }
    if (In(quote_remove_, x, y)) return {AiPanelPart::QuoteRemove, -1};
    if (In(context_remove_, x, y)) return {AiPanelPart::ContextRemove, -1};
    if (In(context_chip_, x, y)) return {AiPanelPart::ContextChip, -1};
    if (In(add_files_, x, y)) return {AiPanelPart::AddFiles, -1};
    if (In(model_button_, x, y)) return {AiPanelPart::ModelButton, -1};
    if (In(mic_button_, x, y)) return {AiPanelPart::Mic, -1};
    if (In(mic_menu_, x, y)) return {AiPanelPart::MicMenu, -1};
    if (In(send_button_, x, y)) return {AiPanelPart::Send, -1};
    if (In(input_rect_, x, y)) return {AiPanelPart::Input, -1};
    return {AiPanelPart::Panel, -1};
}

void AiPanelView::RevealTurnEdit() {
    if (turn_edit_rect_.bottom <= turn_edit_rect_.top) return;
    const float pad = 12.0f * bar_scale_;
    if (turn_edit_rect_.top < conversation_rect_.top + pad) ScrollBy(turn_edit_rect_.top - conversation_rect_.top - pad);
    else if (turn_edit_rect_.bottom > conversation_rect_.bottom - pad) ScrollBy(turn_edit_rect_.bottom - conversation_rect_.bottom + pad + 68.0f * bar_scale_);
}

D2D1_RECT_F AiPanelView::ConvHitRect(AiPanelHit hit) const {
    for (const ConvHit& h : conv_hits_)
        if (h.hit == hit) return h.rect;
    return {};
}

int AiPanelView::UserTurnAt(float x, float y) const {
    if (!drawn_ || !open_ || !In(conversation_rect_, x, y)) return -1;
    for (const auto& [r, block] : turn_rects_)
        if (In(r, x, y)) return block;
    return -1;
}

void AiPanelView::DrawBar(int which, const D2D1_RECT_F& viewport, float offset, float view, float content) {
    Bar& b = bars_[static_cast<size_t>(which)];
    // Thin at rest, full width with its track under the pointer or a drag.
    b.fade.KeepVisible(true);
    b.fade.SetHot(scroll_drag_ == which + 1 || scroll_hot_ == which + 1, bar_now_);
    b.fade.Tick(bar_now_);
    if (b.fade.Moving(bar_now_)) conv_anim_ = true;
    b.spec.viewport = viewport;
    b.spec.offset = offset;
    b.spec.viewport_extent = view;
    b.spec.content_extent = content;
    b.spec.expand_progress = b.fade.Expand();
    b.spec.opacity = b.fade.Opacity();
    b.spec.enabled = true;
    b.drawn = true;
    bar_painter_.DrawScrollbar(b.spec);
}

int AiPanelView::BarAt(float x, float y) const {
    auto in = [&](const D2D1_RECT_F& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    if (in(menu_rect_) || in(mic_menu_)) return 0;   // a menu covers the conversation there
    if (bars_[0].drawn && fluent::ScrollbarHitTest(bars_[0].spec, x, y)) return 1;
    return 0;
}

bool AiPanelView::BeginScrollDrag(float x, float y) {
    const int which = BarAt(x, y);
    if (which == 0) return false;
    const Bar& b = bars_[static_cast<size_t>(which - 1)];
    const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(b.spec, bar_scale_);
    drag_grab_ = fluent::ScrollbarGrabOffset(thumb.top, thumb.bottom - thumb.top, y);
    scroll_drag_ = which;
    scroll_hot_ = which;
    DragScroll(y);
    return true;
}

bool AiPanelView::DragScroll(float y) {
    if (scroll_drag_ == 0) return false;
    const Bar& b = bars_[static_cast<size_t>(scroll_drag_ - 1)];
    if (!b.drawn) {
        scroll_drag_ = 0;   // its view went away (the menu closed)
        return false;
    }
    const D2D1_RECT_F thumb = fluent::ScrollbarThumbRect(b.spec, bar_scale_);
    const float max = (std::max)(0.0f, b.spec.content_extent - b.spec.viewport_extent);
    const float v = fluent::ScrollbarDragOffset(b.spec.viewport.top, b.spec.viewport.bottom - b.spec.viewport.top,
                                                thumb.bottom - thumb.top, max, y, drag_grab_);
    if (std::abs(v - scroll_) < 0.01f && !scroll_anim_) return false;
    scroll_ = v;
    scroll_anim_ = false;
    follow_ = v >= max - 1.0f;
    return true;
}

bool AiPanelView::SetScrollHot(float x, float y) {
    const int hot = scroll_drag_ != 0 ? scroll_drag_ : BarAt(x, y);
    if (hot == scroll_hot_) return false;
    scroll_hot_ = hot;
    return true;
}

bool AiPanelView::Contains(float x, float y) const {
    return drawn_ && open_ && (In(panel_rect_, x, y) || In(menu_rect_, x, y) || VoiceHitTest(x, y).part != AiPanelPart::None);
}

} // namespace pulse::ui
