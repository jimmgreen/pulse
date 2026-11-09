#include "notification_toast.h"

#include "snack_style.h"
#include "ui_motion.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {
// Short, wall-clock bounded motion avoids a long spring tail under load.
constexpr float kRiseDip = 6.0f, kSinkDip = 6.0f, kUnfurlFrom = 0.90f, kExitTiltDeg = 0.0f;
constexpr float kFadeInMs = 90.0f, kContentDelayMs = 0.0f, kContentFadeMs = 100.0f, kSwapFadeMs = 100.0f;
constexpr uint64_t kExitMs = 100;
constexpr size_t kMaxGhosts = 3;
constexpr ULONGLONG kGhostLifeMs = 8000;
constexpr float kStackStepDip = 9.0f, kFanStepDip = 52.0f;
uint32_t EnterSettleMs() noexcept { return 180; }
float ToastProgress(float ms) noexcept {
    const float t = (std::clamp)(ms / 180.0f, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}
float Clamp01(float v) noexcept { return (std::clamp)(v, 0.0f, 1.0f); }

// Moves the drawing by the pose's drop and, once the plate is down, clips and
// fades the content; restores all of it on scope exit (Draw returns early).
class PoseScope {
public:
    PoseScope(ID2D1DeviceContext* dc, float dy, float rot = 0.0f, D2D1_POINT_2F pivot = {}) : dc_(dc) {
        dc_->GetTransform(&saved_);
        if (dy != 0.0f || rot != 0.0f)
            dc_->SetTransform(D2D1::Matrix3x2F::Rotation(rot, pivot) * D2D1::Matrix3x2F::Translation(0.0f, dy) * saved_);
    }
    ~PoseScope() {
        if (layer_) dc_->PopLayer();
        if (clip_) dc_->PopAxisAlignedClip();
        dc_->SetTransform(saved_);
    }
    PoseScope(const PoseScope&) = delete;
    PoseScope& operator=(const PoseScope&) = delete;
    void Clip(const D2D1_RECT_F& rect) {
        dc_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        clip_ = true;
    }
    void Fade(float alpha) {
        if (alpha >= 0.999f) return;
        dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                              D2D1::IdentityMatrix(), alpha),
                       nullptr);
        layer_ = true;
    }
private:
    ID2D1DeviceContext* dc_;
    D2D1_MATRIX_3X2_F saved_{};
    bool clip_ = false, layer_ = false;
};

bool Contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

// One line of capsule text, centered in `height`, cut with an ellipsis.
ComPtr<IDWriteTextLayout> Line(Compositor& compositor, std::wstring_view text, IDWriteTextFormat* format,
                               float max_width, float height, bool semibold) {
    ComPtr<IDWriteTextLayout> layout;
    IDWriteFactory2* const factory = compositor.DwriteFactory();
    if (!factory || !format ||
        FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
                                         (std::max)(1.0f, max_width), (std::max)(1.0f, height), &layout)))
        return {};
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (semibold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(layout.get(), &ellipsis))) {
        const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        layout->SetTrimming(&trimming, ellipsis.get());
    }
    return layout;
}

float LineWidth(IDWriteTextLayout* layout) {
    DWRITE_TEXT_METRICS metrics{};
    return layout && SUCCEEDED(layout->GetMetrics(&metrics)) ? std::ceil(metrics.widthIncludingTrailingWhitespace) : 0.0f;
}

std::wstring_view SnackGlyph(SnackIcon icon) {
    switch (icon) {
    case SnackIcon::Window: return L"\xE8A7";   // OpenInNewWindow
    case SnackIcon::Undo: return L"\xE7A7";     // Undo
    case SnackIcon::Merge: return L"\xE7C4";    // TaskView: windows into one
    case SnackIcon::Suggest: return L"\xEA80";  // Lightbulb
    case SnackIcon::Tab:
    default: return L"\xE8B7";                  // Folder
    }
}
}
void NotificationToast::Show(HWND owner, std::wstring title, std::wstring message, bool persistent, UINT action_message) {
    ghosts_.clear();   // ShowSnack keeps its stack
    replace_next_ = false;
    expand_from_ = expand_to_ = 0.0f;
    // Over a toast that is up, the plate morphs from what was drawn; else it rises.
    swapped_ = visible_ && drawn_w_ > 0.0f;
    swap_snack_ = snack_;
    swap_from_w_ = drawn_w_;
    exiting_ = false;
    animate_ = motion::SystemAnimationsEnabled();
    enter_at_ = motion::NowMs();
    title_ = std::move(title);
    message_ = std::move(message);
    persistent_ = persistent;
    snack_ = false;
    actions_.clear();
    action_rects_.clear();
    key_.clear();
    pressed_action_ = -1;
    duration_ = 6000;
    kind_ = persistent ? fluent::InfoBarKind::Warning : fluent::InfoBarKind::Informational;
    action_message_ = action_message;
    visible_ = true;
    hovered_ = pressed_close_ = pressed_card_ = false;
    born_ = last_tick_ = GetTickCount64();
    remaining_ = 6000;
    SetTimer(owner, timer, 16, nullptr);
    InvalidateRect(owner, nullptr, FALSE);
}
void NotificationToast::ShowSnack(HWND owner, SnackIcon icon, std::wstring text, std::wstring detail,
                                  std::vector<SnackAction> actions, std::wstring key, UINT duration_ms) {
    // A capsule that is up steps back into the stack instead of morphing.
    const bool stack = snack_ && visible_ && drawn_w_ > 0.0f && !replace_next_ && motion::SystemAnimationsEnabled();
    replace_next_ = false;
    std::vector<Ghost> ghosts = std::move(ghosts_);
    if (stack) {
        ghosts.insert(ghosts.begin(), Ghost{title_, message_, icon_, drawn_w_, GetTickCount64()});
        if (ghosts.size() > kMaxGhosts) ghosts.resize(kMaxGhosts);
    } else {
        ghosts.clear();
    }
    Show(owner, std::move(text), std::move(detail), false);
    ghosts_ = std::move(ghosts);
    if (stack) {
        swapped_ = false;   // rises in front
        stack_push_at_ = motion::NowMs();
    }
    snack_ = true;
    icon_ = icon;
    if (actions.size() > 2) actions.resize(2);
    actions_ = std::move(actions);
    action_rects_.assign(actions_.size(), D2D1_RECT_F{});
    key_ = std::move(key);
    duration_ = remaining_ = (std::max)(UINT{1500}, duration_ms);
}
void NotificationToast::ShowError(HWND owner, std::wstring title, std::wstring message) {
    // Same corner and card as every other toast; an error waits to be read
    // and closed (the close button or Esc) instead of timing out.
    Show(owner, std::move(title), std::move(message), true);
    kind_ = fluent::InfoBarKind::Error;
}
void NotificationToast::ShowActionCard(HWND owner, fluent::InfoBarKind kind, std::wstring title, std::wstring message,
                                       std::vector<SnackAction> actions) {
    Show(owner, std::move(title), std::move(message), true);
    kind_ = kind;
    if (actions.size() > 2) actions.resize(2);
    actions_ = std::move(actions);
    action_rects_.assign(actions_.size(), D2D1_RECT_F{});
}
void NotificationToast::Dismiss(HWND owner) {
    const bool was_up = visible_;
    visible_ = false;
    pressed_close_ = pressed_card_ = hovered_ = false;
    pressed_action_ = -1;
    // Input stops now; the drawing sinks away over kExitMs.
    if (was_up && animate_ && drawn_w_ > 0.0f) {
        exiting_ = true;
        exit_at_ = motion::NowMs();
        SetTimer(owner, timer, 16, nullptr);
    } else {
        exiting_ = false;
        drawn_w_ = 0.0f;
        ghosts_.clear();
        KillTimer(owner, timer);
    }
    InvalidateRect(owner, nullptr, FALSE);
}

float NotificationToast::Expand() const noexcept {
    if (expand_from_ == expand_to_) return expand_to_;
    const float p = ToastProgress(static_cast<float>(motion::NowMs() - expand_at_));
    return expand_from_ + (expand_to_ - expand_from_) * p;
}

void NotificationToast::AccountTime(ULONGLONG now) noexcept {
    if (!persistent_ && !hovered_) {
        const auto elapsed = now - last_tick_;
        remaining_ = elapsed >= remaining_ ? 0 : remaining_ - elapsed;
    }
    last_tick_ = now;
}
bool NotificationToast::StackMoving() const noexcept {
    if (ghosts_.empty()) return false;
    const uint64_t now = motion::NowMs();
    const uint64_t settle = EnterSettleMs();
    return now - stack_push_at_ < settle || now - expand_at_ < settle;
}

NotificationToast::Pose NotificationToast::CurrentPose(float scale, float target_w, bool high_contrast) const {
    Pose pose;
    pose.plate_w = target_w;
    if (!animate_ || high_contrast) return pose;
    if (exiting_) {
        const float e = Clamp01(static_cast<float>(motion::NowMs() - exit_at_) / static_cast<float>(kExitMs));
        pose.dy = kSinkDip * scale * e * e;
        pose.rot = snack_ ? kExitTiltDeg * e : 0.0f;
        if (snack_) pose.plate_w = target_w * (1.0f - 0.10f * e);
        pose.plate_alpha = 1.0f - e;
        pose.content_alpha = Clamp01(1.0f - 1.7f * e);
        return pose;
    }
    const float t = static_cast<float>(motion::NowMs() - enter_at_);
    if (swapped_ && swap_snack_ == snack_) {
        if (snack_ && swap_from_w_ > 0.0f) {
            const float s = ToastProgress(t);
            pose.plate_w = swap_from_w_ + (target_w - swap_from_w_) * s;
        }
        pose.content_alpha = Clamp01(t / kSwapFadeMs);
        return pose;
    }
    const float s = ToastProgress(t);
    pose.dy = kRiseDip * scale * (1.0f - s);
    if (snack_) pose.plate_w = target_w * (kUnfurlFrom + (1.0f - kUnfurlFrom) * s);
    pose.plate_alpha = Clamp01(t / kFadeInMs);
    pose.content_alpha = Clamp01((t - kContentDelayMs) / kContentFadeMs);
    return pose;
}
bool NotificationToast::HandleMessage(HWND owner, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) { KillTimer(owner, timer); return false; }
    if (message == WM_TIMER && wparam == timer && !visible_) {
        // The exit frames; nothing to count down any more.
        if (!exiting_ || motion::NowMs() - exit_at_ >= kExitMs) {
            exiting_ = false;
            drawn_w_ = 0.0f;
            ghosts_.clear();
            KillTimer(owner, timer);
        }
        InvalidateRect(owner, nullptr, FALSE);
        return true;
    }
    if (!visible_) return false;
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        Dismiss(owner);
        return true;
    }
    if (message == WM_TIMER && wparam == timer) {
        const auto now = GetTickCount64();
        AccountTime(now);
        if (!persistent_ && !hovered_ && !remaining_) { Dismiss(owner); return true; }
        // Old capsules leave the stack on their own; the fan holds them.
        bool changed = false;
        if (!hovered_) {
            const size_t before = ghosts_.size();
            while (!ghosts_.empty() && now - ghosts_.back().at > kGhostLifeMs) ghosts_.pop_back();
            changed = ghosts_.size() != before;
            if (changed && ghosts_.empty()) expand_from_ = expand_to_ = 0.0f;
        }
        // 16 ms frames only while something moves. A settled capsule repaints
        // nothing: a single timer wakes it to expire it (or the oldest ghost),
        // so the owner window is not redrawn on a fixed cadence.
        const bool entering = animate_ && motion::NowMs() - enter_at_ < EnterSettleMs();
        if (StackMoving() || entering) {
            SetTimer(owner, timer, 16, nullptr);
            changed = true;
        } else if (persistent_ || hovered_) {
            KillTimer(owner, timer);  // hover resumes it on leave
        } else {
            ULONGLONG wait = remaining_;
            if (!ghosts_.empty()) {
                const auto age = now - ghosts_.back().at;
                wait = (std::min)(wait, age >= kGhostLifeMs ? ULONGLONG{1} : kGhostLifeMs - age + 1);
            }
            SetTimer(owner, timer, static_cast<UINT>((std::max)(wait, ULONGLONG{16})), nullptr);
        }
        if (changed) InvalidateRect(owner, nullptr, FALSE);
        return true;
    }
    const float x = static_cast<float>(GET_X_LPARAM(lparam));
    const float y = static_cast<float>(GET_Y_LPARAM(lparam));
    if (message == WM_MOUSEMOVE) {
        const bool hovered = Contains(card_, x, y) || (!ghosts_.empty() && Contains(stack_area_, x, y));
        if (hovered != hovered_) {
            AccountTime(GetTickCount64());
            hovered_ = hovered;
            if (!ghosts_.empty()) {
                expand_from_ = Expand();
                expand_to_ = hovered ? 1.0f : 0.0f;
                expand_at_ = motion::NowMs();
            }
            // Leaving re-arms the expiry; entering lets the next tick pause it.
            SetTimer(owner, timer, 16, nullptr);
            InvalidateRect(owner, nullptr, FALSE);
        }
        if (hovered_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, owner, 0};
            TrackMouseEvent(&tracking);
            InvalidateRect(owner, nullptr, FALSE);
        }
        if (pressed_card_ || hovered_) return true;
    } else if (message == WM_MOUSELEAVE) {
        AccountTime(GetTickCount64());
        hovered_ = false;
        SetTimer(owner, timer, 16, nullptr);  // resume the expiry
        if (!ghosts_.empty() && expand_to_ != 0.0f) {
            expand_from_ = Expand();
            expand_to_ = 0.0f;
            expand_at_ = motion::NowMs();
            InvalidateRect(owner, nullptr, FALSE);
        }
    } else if (message == WM_LBUTTONDOWN && Contains(card_, x, y)) {
        pressed_card_ = true;
        pressed_close_ = Contains(close_, x, y);
        pressed_action_ = -1;
        for (size_t i = 0; i < action_rects_.size(); ++i)
            if (Contains(action_rects_[i], x, y)) pressed_action_ = static_cast<int>(i);
        SetCapture(owner);
        InvalidateRect(owner, nullptr, FALSE);
        return true;
    } else if (message == WM_LBUTTONUP && (pressed_card_ || Contains(card_, x, y))) {
        const bool close = pressed_close_ && Contains(close_, x, y);
        const int pressed_action = pressed_action_;
        const bool action = !snack_ && actions_.empty() && pressed_card_ && !pressed_close_ && Contains(card_, x, y) &&
                            action_message_;
        pressed_close_ = pressed_card_ = false;
        pressed_action_ = -1;
        if (GetCapture() == owner) ReleaseCapture();
        if (close) Dismiss(owner);
        else if (action) {
            PostMessageW(owner, action_message_, 0, 0);
            Dismiss(owner);
        } else if (pressed_action >= 0 && static_cast<size_t>(pressed_action) < actions_.size() &&
                   static_cast<size_t>(pressed_action) < action_rects_.size() &&
                   Contains(action_rects_[static_cast<size_t>(pressed_action)], x, y)) {
            // The capsule and an action card act on their buttons only: the rest is text.
            if (const UINT post = actions_[static_cast<size_t>(pressed_action)].message) PostMessageW(owner, post, actions_[static_cast<size_t>(pressed_action)].wparam, 0);
            Dismiss(owner);
        } else {
            InvalidateRect(owner, nullptr, FALSE);
        }
        return true;
    } else if (message == WM_CAPTURECHANGED) {
        pressed_close_ = pressed_card_ = false;
        pressed_action_ = -1;
    } else if ((message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP) &&
               Contains(card_, x, y)) {
        return true;
    }
    return false;
}
void NotificationToast::Draw(Compositor& compositor, const Theme& theme, float scale, bool high_contrast) {
    if (!visible_ && !exiting_) return;
    if (snack_) {
        DrawSnack(compositor, theme, scale, high_contrast);
        return;
    }
    painter_.SetCompositor(&compositor);
    painter_.SetScale(scale);
    if (!painter_.BeginFrame(theme, high_contrast)) return;
    ID2D1DeviceContext* const dc = compositor.Dc();
    IDWriteFactory2* const factory = compositor.DwriteFactory();
    if (!dc || !factory) return;
    // A floating card on the capsule's acrylic plate: a status glyph, a bold
    // line and up to two quieter ones, and a close button in the corner.
    const SnackPalette pal = MakeSnackPalette(theme, high_contrast);
    const auto px = [scale](float dip) { return dip * scale; };
    const float window_w = static_cast<float>(compositor.Width());
    const float window_h = static_cast<float>(compositor.Height());
    const float margin = px(16.0f);
    const float width = (std::min)(px(360.0f), window_w - 2 * margin);
    if (width < px(100.0f)) return;
    const float pad = px(14.0f), icon = px(20.0f);
    const float text_x = pad + icon + px(12.0f);
    const float text_w = (std::max)(px(40.0f), width - text_x - px(8.0f + 24.0f + 6.0f));
    const float title_h = px(22.0f);
    auto title = Line(compositor, title_, compositor.TextFormat(), text_w, title_h, true);
    ComPtr<IDWriteTextLayout> body;
    float body_h = 0.0f;
    if (!message_.empty() && SUCCEEDED(factory->CreateTextLayout(message_.c_str(), static_cast<UINT32>(message_.size()),
            compositor.SmallFormat(), text_w, px(400.0f), &body))) {
        body->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(body->GetMetrics(&metrics))) {
            const float line = metrics.lineCount ? metrics.height / static_cast<float>(metrics.lineCount) : px(16.0f);
            // Two lines at most (four on a card with buttons: it explains what to do).
            const UINT32 max_lines = actions_.empty() ? 2u : 4u;
            body_h = (std::min)(metrics.height, std::ceil(line * static_cast<float>(max_lines)));
            if (metrics.lineCount > max_lines) {
                body->SetMaxHeight(body_h + 0.5f);
                ComPtr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(body.get(), &ellipsis))) {
                    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_WORD, 0, 0};
                    body->SetTrimming(&trimming, ellipsis.get());
                }
            } else {
                body->SetMaxHeight(body_h + 0.5f);
            }
        }
    }
    // An action card: a row of buttons under the text.
    const float button_h = px(28.0f), buttons_gap = px(10.0f);
    const float buttons_h = actions_.empty() ? 0.0f : buttons_gap + button_h;
    const float height = std::round(px(12.0f) + title_h + (body_h > 0.0f ? px(2.0f) + body_h : 0.0f) + buttons_h +
                                    px(12.0f));
    const float right = std::round(window_w - margin);
    const float top = std::round(window_h - px(48.0f) - height);
    card_ = D2D1::RectF(right - width, top, right, top + height);
    const float radius = px(10.0f);
    const Pose pose = CurrentPose(scale, width, high_contrast);
    if (visible_) drawn_w_ = width;
    PoseScope scope(dc, std::round(pose.dy));
    DrawSnackPlate(dc, card_, radius, pal, scale, high_contrast, pose.plate_alpha);
    if (pose.content_alpha <= 0.0f) return;
    scope.Fade(pose.content_alpha);
    const bool toned = kind_ == fluent::InfoBarKind::Error ||
                       (kind_ == fluent::InfoBarKind::Warning && !actions_.empty());
    if (toned && !high_contrast) {
        // A card with buttons is tinted in its tone: it asks for a next step.
        if (!actions_.empty()) {
            D2D1_COLOR_F tint = painter_.StatusToneColor(kind_);
            tint.a = 0.07f;
            painter_.FillRoundedRect(card_, radius, tint);
        }
        // An error keeps a thin edge in its tone so it is not read as a status note.
        D2D1_COLOR_F edge = painter_.StatusToneColor(kind_);
        edge.a = 0.55f;
        painter_.StrokeRoundedRect(D2D1::RectF(card_.left + 0.5f, card_.top + 0.5f, card_.right - 0.5f, card_.bottom - 0.5f),
                                   radius - 0.5f, edge, 1.0f);
    }
    const float line_mid = top + px(12.0f) + title_h * 0.5f;
    painter_.DrawStatusGlyph(kind_, D2D1::RectF(card_.left + pad, line_mid - icon * 0.5f, card_.left + pad + icon,
                                                line_mid + icon * 0.5f), pal.plate);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(pal.text, &brush))) return;
    if (title.get()) compositor.DrawTextLayout(D2D1::Point2F(card_.left + text_x, top + px(12.0f)), title.get(),
                                               brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (body.get() && body_h > 0.0f) {
        brush->SetColor(pal.text_secondary);
        compositor.DrawTextLayout(D2D1::Point2F(card_.left + text_x, top + px(12.0f) + title_h + px(2.0f)),
                                  body.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    POINT cursor{};
    float cx = -1.0f, cy = -1.0f;
    if (GetCursorPos(&cursor) && ScreenToClient(compositor.Hwnd(), &cursor)) {
        cx = static_cast<float>(cursor.x);
        cy = static_cast<float>(cursor.y);
    }
    if (!actions_.empty()) {
        action_rects_.resize(actions_.size());
        float bx = card_.left + text_x;
        const float by = card_.bottom - px(12.0f) - button_h;
        for (size_t i = 0; i < actions_.size(); ++i) {
            const float bw = (std::max)(px(72.0f), painter_.MeasureButtonWidth(actions_[i].label));
            const D2D1_RECT_F button = D2D1::RectF(bx, by, (std::min)(bx + bw, card_.right - pad), by + button_h);
            action_rects_[i] = button;
            fluent::ButtonSpec spec;
            spec.bounds = button;
            spec.text = actions_[i].label;
            spec.kind = i == 0 && !high_contrast ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard;
            spec.state.hovered = visible_ && Contains(button, cx, cy);
            spec.state.pressed = pressed_action_ == static_cast<int>(i);
            painter_.DrawButton(spec);
            bx = button.right + px(8.0f);
        }
    }
    close_ = D2D1::RectF(card_.right - px(8.0f + 24.0f), line_mid - px(12.0f), card_.right - px(8.0f), line_mid + px(12.0f));
    if (visible_ && (Contains(close_, cx, cy) || pressed_close_)) painter_.FillRoundedRect(close_, px(12.0f), pal.hover);
    {
        const float c = (close_.left + close_.right) * 0.5f, arm = px(3.5f);
        brush->SetColor(pal.text_secondary);
        dc->DrawLine(D2D1::Point2F(c - arm, line_mid - arm), D2D1::Point2F(c + arm, line_mid + arm), brush.get(), px(1.2f));
        dc->DrawLine(D2D1::Point2F(c + arm, line_mid - arm), D2D1::Point2F(c - arm, line_mid + arm), brush.get(), px(1.2f));
    }
}

void NotificationToast::DrawSnack(Compositor& compositor, const Theme& theme, float scale, bool high_contrast) {
    painter_.SetCompositor(&compositor);
    painter_.SetScale(scale);
    if (!painter_.BeginFrame(theme, high_contrast)) return;
    ID2D1DeviceContext* const dc = compositor.Dc();
    if (!dc) return;
    const SnackPalette pal = MakeSnackPalette(theme, high_contrast);
    const auto px = [scale](float dip) { return dip * scale; };
    const float window_w = static_cast<float>(compositor.Width());
    const float window_h = static_cast<float>(compositor.Height());
    const float height = px(36.0f);
    const float max_width = window_w - px(32.0f);
    if (max_width < px(160.0f)) return;
    IDWriteTextFormat* const text_format = compositor.TextFormat();
    IDWriteTextFormat* const small_format = compositor.SmallFormat();

    // Measure: [pad icon gap] title [· detail] [gap | gap] buttons [key] [close pad].
    const std::wstring detail = message_.empty() ? std::wstring() : L" \u00B7 " + message_;
    auto title = Line(compositor, title_, text_format, max_width, height, true);
    ComPtr<IDWriteTextLayout> detail_line;
    if (!detail.empty()) detail_line = Line(compositor, detail, text_format, max_width, height, false);
    std::vector<ComPtr<IDWriteTextLayout>> labels;
    float buttons_w = 0.0f;
    for (size_t i = 0; i < actions_.size(); ++i) {
        labels.push_back(Line(compositor, actions_[i].label, text_format, max_width, height, i == 0));
        buttons_w += LineWidth(labels.back().get()) + px(16.0f) + (i ? px(2.0f) : 0.0f);
    }
    ComPtr<IDWriteTextLayout> key;
    if (!key_.empty()) key = Line(compositor, key_, small_format, max_width, px(18.0f), true);
    const float lead = px(14.0f + 16.0f + 9.0f);
    const float close_w = px(4.0f + 24.0f + 6.0f);
    const float divider_w = actions_.empty() ? 0.0f : px(10.0f + 1.0f + 4.0f);
    float title_w = LineWidth(title.get());
    float detail_w = LineWidth(detail_line.get());
    float key_w = key.get() ? LineWidth(key.get()) + px(10.0f + 6.0f) : 0.0f;
    const auto total = [&] { return lead + title_w + detail_w + divider_w + buttons_w + key_w + close_w; };
    if (total() > max_width) detail_w = 0.0f;  // the detail goes first
    if (total() > max_width) key_w = 0.0f;     // then the shortcut
    if (total() > max_width) title_w = (std::max)(px(40.0f), title_w - (total() - max_width));
    const float width = (std::min)(max_width, total());

    const float left = std::round((window_w - width) * 0.5f);
    const float top = std::round(window_h - px(48.0f) - height);
    card_ = D2D1::RectF(left, top, left + width, top + height);
    const float radius = height * 0.5f;
    const float mid = top + height * 0.5f;

    POINT cursor{};
    float cx = -1.0f, cy = -1.0f;
    if (GetCursorPos(&cursor) && ScreenToClient(compositor.Hwnd(), &cursor)) {
        cx = static_cast<float>(cursor.x);
        cy = static_cast<float>(cursor.y);
    }

    // Acrylic plate in the window's theme, with the flyout shadow. It
    // unfurls from the middle as the capsule rises; the content (laid out at
    // the settled place) shows through it as it widens.
    const Pose pose = CurrentPose(scale, width, high_contrast);
    const float plate_w = (std::max)(height, pose.plate_w);
    const float plate_left = (left + width * 0.5f) - plate_w * 0.5f;
    const D2D1_RECT_F plate = D2D1::RectF(plate_left, top, plate_left + plate_w, top + height);
    if (visible_) drawn_w_ = plate_w;
    DrawGhosts(compositor, pal, scale, height, pose, high_contrast);
    PoseScope scope(dc, std::round(pose.dy), pose.rot, D2D1::Point2F(left + width * 0.5f, mid));
    DrawSnackPlate(dc, plate, radius, pal, scale, high_contrast, pose.plate_alpha);
    if (pose.content_alpha <= 0.0f) return;
    if (plate_w < width - 0.5f) scope.Clip(D2D1::RectF(plate.left + radius * 0.5f, top, plate.right - radius * 0.5f, top + height));
    scope.Fade(pose.content_alpha);

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(pal.text, &brush))) return;
    const auto draw = [&](IDWriteTextLayout* layout, float x, float w, float h, float y, const D2D1_COLOR_F& color) {
        if (!layout || w <= 0.0f) return;
        layout->SetMaxWidth(w);
        layout->SetMaxHeight(h);
        brush->SetColor(color);
        compositor.DrawTextLayout(D2D1::Point2F(x, y), layout, brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    float x = left + px(14.0f);
    painter_.DrawGlyph(SnackGlyph(icon_), D2D1::RectF(x - px(2.0f), mid - px(10.0f), x + px(18.0f), mid + px(10.0f)),
                       pal.accent);
    x += px(16.0f + 9.0f);
    draw(title.get(), x, title_w, height, top, pal.text);
    x += title_w;
    if (detail_w > 0.0f) {
        draw(detail_line.get(), x, detail_w, height, top, pal.text_secondary);
        x += detail_w;
    }
    if (!actions_.empty()) {
        x += px(10.0f);
        painter_.FillRoundedRect(D2D1::RectF(x, mid - px(8.0f), x + px(1.0f), mid + px(8.0f)), 0.0f, pal.divider);
        x += px(1.0f + 4.0f);
    }
    for (size_t i = 0; i < actions_.size(); ++i) {
        if (i) x += px(2.0f);
        const float label_w = LineWidth(labels[i].get());
        const D2D1_RECT_F button = D2D1::RectF(x, mid - px(13.0f), x + label_w + px(16.0f), mid + px(13.0f));
        action_rects_[i] = button;
        const bool pressed = pressed_action_ == static_cast<int>(i);
        const bool hot = visible_ && (Contains(button, cx, cy) || pressed);
        // Pressed, the button gives a little (the label stays put).
        const D2D1_RECT_F wash = pressed ? D2D1::RectF(button.left + px(1.5f), button.top + px(1.0f),
                                                       button.right - px(1.5f), button.bottom - px(1.0f))
                                         : button;
        if (i == 0 && !high_contrast) {
            // The first button reads as one: a wash of the accent, deeper under the cursor.
            painter_.FillRoundedRect(wash, px(13.0f), hot ? SnackAlpha(pal.accent_wash, pal.accent_wash.a * 1.8f)
                                                            : pal.accent_wash);
        } else if (hot) {
            painter_.FillRoundedRect(wash, px(13.0f), pal.hover);
        }
        draw(labels[i].get(), x + px(8.0f), label_w + 1.0f, height, top, i == 0 ? pal.accent : pal.text_secondary);
        x = button.right;
    }
    if (key_w > 0.0f && key.get()) {
        x += px(6.0f);
        const float chip_w = key_w - px(6.0f);
        const D2D1_RECT_F chip = D2D1::RectF(x, mid - px(9.0f), x + chip_w, mid + px(9.0f));
        D2D1_COLOR_F edge = pal.text_secondary;
        edge.a *= 0.6f;
        painter_.StrokeRoundedRect(chip, px(4.0f), edge, px(1.0f));
        draw(key.get(), x + px(5.0f), chip_w - px(10.0f) + 1.0f, px(18.0f), chip.top, pal.text_secondary);
        x = chip.right;
    }
    // Close: a small x in a round hover target.
    close_ = D2D1::RectF(card_.right - px(6.0f + 24.0f), mid - px(12.0f), card_.right - px(6.0f), mid + px(12.0f));
    if (visible_ && (Contains(close_, cx, cy) || pressed_close_)) painter_.FillRoundedRect(close_, px(12.0f), pal.hover);
    {
        const float c = (close_.left + close_.right) * 0.5f, arm = px(3.5f);
        brush->SetColor(pal.text_secondary);
        dc->DrawLine(D2D1::Point2F(c - arm, mid - arm), D2D1::Point2F(c + arm, mid + arm), brush.get(), px(1.2f));
        dc->DrawLine(D2D1::Point2F(c + arm, mid - arm), D2D1::Point2F(c - arm, mid + arm), brush.get(), px(1.2f));
    }
}

void NotificationToast::DrawGhosts(Compositor& compositor, const SnackPalette& pal, float scale, float height,
                                   const Pose& pose, bool high_contrast) {
    stack_area_ = card_;
    if (ghosts_.empty() || high_contrast) return;
    ID2D1DeviceContext* const dc = compositor.Dc();
    const auto px = [scale](float dip) { return dip * scale; };
    const float center = (card_.left + card_.right) * 0.5f;
    const float e = Expand();
    // Each ghost moved one step back at the last push.
    const float push = ToastProgress(static_cast<float>(motion::NowMs() - stack_push_at_));
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(pal.text, &brush))) return;
    for (size_t n = ghosts_.size(); n-- > 0;) {
        const Ghost& g = ghosts_[n];
        const float depth = static_cast<float>(n + 1) - (1.0f - push);
        const float step = kStackStepDip + (kFanStepDip - kStackStepDip) * e;
        const float s = 1.0f - 0.05f * depth * (1.0f - e);
        const float rest_alpha = depth > 3.0f ? 0.0f : (std::max)(0.0f, 1.0f - 0.18f * depth);
        const float alpha = (rest_alpha + (1.0f - rest_alpha) * e) * pose.plate_alpha;
        if (alpha <= 0.01f) continue;
        const float w = g.width * s, h = height * s;
        const float bottom = card_.bottom - px(step) * depth;
        const D2D1_RECT_F plate = D2D1::RectF(std::round(center - w * 0.5f), std::round(bottom - h),
                                              std::round(center + w * 0.5f), std::round(bottom));
        stack_area_.left = (std::min)(stack_area_.left, plate.left);
        stack_area_.right = (std::max)(stack_area_.right, plate.right);
        stack_area_.top = (std::min)(stack_area_.top, plate.top);
        PoseScope scope(dc, std::round(pose.dy));
        DrawSnackPlate(dc, plate, h * 0.5f, pal, scale, high_contrast, alpha);
        // What it said shows once the stack fans out.
        const float content = alpha * Clamp01((e - 0.35f) / 0.65f);
        if (content <= 0.01f) continue;
        scope.Clip(D2D1::RectF(plate.left + h * 0.4f, plate.top, plate.right - h * 0.4f, plate.bottom));
        scope.Fade(content);
        const float mid = (plate.top + plate.bottom) * 0.5f;
        float x = plate.left + px(14.0f);
        painter_.DrawGlyph(SnackGlyph(g.icon), D2D1::RectF(x - px(2.0f), mid - px(10.0f), x + px(18.0f), mid + px(10.0f)),
                           pal.accent);
        x += px(16.0f + 9.0f);
        const float room = plate.right - px(14.0f) - x;
        auto title = Line(compositor, g.title, compositor.TextFormat(), (std::max)(1.0f, room), h, true);
        const float title_w = (std::min)(room, LineWidth(title.get()));
        if (title.get() && title_w > 0.0f) {
            brush->SetColor(pal.text);
            compositor.DrawTextLayout(D2D1::Point2F(x, plate.top), title.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (!g.detail.empty() && room - title_w > px(24.0f)) {
            auto detail = Line(compositor, L" \u00B7 " + g.detail, compositor.TextFormat(), room - title_w, h, false);
            if (detail.get()) {
                brush->SetColor(pal.text_secondary);
                compositor.DrawTextLayout(D2D1::Point2F(x + title_w, plate.top), detail.get(), brush.get(),
                                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }
}
}
