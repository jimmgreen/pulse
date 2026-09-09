#include "legacy_icons.h"
#include "fluent_components.h"
#include "tab_shape.h"
#include "typography.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace pulse::ui::fluent {

namespace {

constexpr std::wstring_view kChevronDown = L"\xE70D";
constexpr std::wstring_view kChevronUp = L"\xE70E";
constexpr std::wstring_view kChevronRight = L"\xE76C";
constexpr std::wstring_view kSearch = L"\xE721";
constexpr std::wstring_view kClose = L"\xE8BB";
constexpr std::wstring_view kMinimize = L"\xE921";
constexpr std::wstring_view kMaximize = L"\xE922";
constexpr std::wstring_view kRestore = L"\xE923";
constexpr std::wstring_view kCopyIntent = L"\xE8C8";
constexpr std::wstring_view kMoveIntent = L"\xE7C2";
constexpr std::wstring_view kSort = L"\xE8CB";
constexpr std::wstring_view kInfo = L"\xE946";
constexpr std::wstring_view kSuccess = L"\xE73E";
constexpr std::wstring_view kWarning = L"\xE7BA";
constexpr std::wstring_view kError = L"\xEA39";

float Clamp01(float value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

D2D1_RECT_F Inset(D2D1_RECT_F bounds, float amount) noexcept {
    bounds.left += amount;
    bounds.top += amount;
    bounds.right -= amount;
    bounds.bottom -= amount;
    return bounds;
}

D2D1_RECT_F Inflate(D2D1_RECT_F bounds, float amount) noexcept {
    return Inset(bounds, -amount);
}

float Width(const D2D1_RECT_F& bounds) noexcept {
    return std::max(0.0f, bounds.right - bounds.left);
}

float Height(const D2D1_RECT_F& bounds) noexcept {
    return std::max(0.0f, bounds.bottom - bounds.top);
}

float MeasureTextWidth(IDWriteFactory2* factory, IDWriteTextFormat* format,
                       std::wstring_view text) {
    return typography::MeasureAdvance(factory, format, text);
}

float MeasureTextWidth(Compositor* compositor, IDWriteTextFormat* format,
                       std::wstring_view text) {
    return typography::MeasureLine(compositor, format, text);
}

D2D1_COLOR_F TextFieldFillColor(const Theme& theme, const ControlState& state,
                                bool /*dark*/, bool high_contrast,
                                bool hosted_edit) noexcept {
    if (high_contrast) return theme.fill_input;
    if (hosted_edit) {
        // Layered EDIT paints an opaque #1E1E1E / white plate. Rest fill_input
        // is a translucent wash, so the overlay reads as a box inside a box.
        return theme.fill_input_focus;
    }
    if (!state.enabled) return theme.fill_input_disabled;
    if (state.focused) return theme.fill_input_focus;
    if (state.hovered) return theme.fill_input_hover;
    return theme.fill_input;
}

struct TextLayoutKey {
    IDWriteTextFormat* format = nullptr;
    std::wstring text;
    int width_64 = 0;
    int height_64 = 0;
    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    std::uint64_t generation = 0;

    bool operator==(const TextLayoutKey&) const = default;
};

struct TextLayoutKeyHash {
    size_t operator()(const TextLayoutKey& key) const noexcept {
        size_t value = std::hash<void*>{}(key.format);
        const auto combine = [&value](size_t next) {
            value ^= next + 0x9e3779b9u + (value << 6) + (value >> 2);
        };
        combine(std::hash<std::wstring>{}(key.text));
        combine(std::hash<int>{}(key.width_64));
        combine(std::hash<int>{}(key.height_64));
        combine(std::hash<int>{}(static_cast<int>(key.alignment)));
        combine(std::hash<std::uint64_t>{}(key.generation));
        return value;
    }
};

thread_local std::unordered_map<TextLayoutKey, ComPtr<IDWriteTextLayout>, TextLayoutKeyHash>
    g_text_layout_cache;
constexpr size_t kTextLayoutCacheLimit = 1024;

void ClearTextLayoutCache() {
    g_text_layout_cache.clear();
}

IDWriteTextLayout* GetTextLayout(Compositor* compositor, IDWriteTextFormat* format,
                                 std::wstring_view text, float width, float height,
                                 DWRITE_TEXT_ALIGNMENT alignment) {
    if (!compositor || !compositor->DwriteFactory() || !format || text.empty()) return nullptr;
    TextLayoutKey key{
        format,
        std::wstring(text),
        static_cast<int>(std::lround(width * 64.0f)),
        static_cast<int>(std::lround(height * 64.0f)),
        alignment,
        typography::Generation(),
    };
    if (const auto found = g_text_layout_cache.find(key); found != g_text_layout_cache.end()) {
        return found->second.get();
    }

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(compositor->DwriteFactory()->CreateTextLayout(
            text.data(), static_cast<UINT32>(text.size()), format,
            width, height, &layout)) || !layout.get()) return nullptr;
    layout->SetTextAlignment(alignment);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_text_layout_cache.size() >= kTextLayoutCacheLimit) g_text_layout_cache.clear();
    IDWriteTextLayout* result = layout.get();
    g_text_layout_cache.emplace(std::move(key), std::move(layout));
    return result;
}

D2D1_COLOR_F MultiplyAlpha(D2D1_COLOR_F color, float opacity) noexcept {
    color.a *= Clamp01(opacity);
    return color;
}

D2D1_COLOR_F Rgba(uint32_t rgb, int alpha) noexcept {
    return HexColor(rgb, static_cast<float>(std::clamp(alpha, 0, 255)) / 255.0f);
}

D2D1_COLOR_F RgbaF(uint32_t rgb, float alpha) noexcept {
    return HexColor(rgb, Clamp01(alpha));
}

float Ease(EasingCurve curve, float value) noexcept {
    const float t = Clamp01(value);
    switch (curve) {
    case EasingCurve::Linear:
        return t;
    case EasingCurve::OutQuad:
        return 1.0f - (1.0f - t) * (1.0f - t);
    case EasingCurve::OutCubic:
        return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    case EasingCurve::InOutQuad:
        return t < 0.5f ? 2.0f * t * t
                        : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
    case EasingCurve::InOutSine:
        return -(std::cos(3.14159265f * t) - 1.0f) * 0.5f;
    case EasingCurve::OutSine:
        return std::sin(3.14159265f * t * 0.5f);
    }
    return t;
}

} // namespace

float EvaluateMotion(const MotionSpec& motion, float elapsed_ms) noexcept {
    if (motion.duration_ms <= 0.0f) {
        return elapsed_ms >= motion.delay_ms ? 1.0f : 0.0f;
    }
    return Ease(motion.easing, (elapsed_ms - motion.delay_ms) / motion.duration_ms);
}

Painter::Painter(Compositor* compositor) noexcept : compositor_(compositor) {}

void Painter::SetCompositor(Compositor* compositor) noexcept {
    if (compositor_ == compositor && (!compositor || dc_ == compositor->Dc())) {
        return;
    }
    ClearTextLayoutCache();
    compositor_ = compositor;
    dc_ = nullptr;
    theme_ = nullptr;
    for (auto& brush : brushes_) {
        brush.reset();
    }
    scratch_brush_.reset();
    round_stroke_.reset();
    body_format_.reset();
    nav_format_.reset();
    section_format_.reset();
    caption_format_.reset();
    small_icon_format_.reset();
    micro_icon_format_.reset();
    ellipsis_sign_.reset();
    format_scale_ = 0.0f;
}

void Painter::SetScale(float scale) noexcept {
    const float resolved = std::max(0.25f, scale);
    if (std::abs(scale_ - resolved) > 0.001f) {
        ClearTextLayoutCache();
        scale_ = resolved;
        body_format_.reset();
        nav_format_.reset();
        section_format_.reset();
        caption_format_.reset();
        small_icon_format_.reset();
        micro_icon_format_.reset();
        ellipsis_sign_.reset();
        format_scale_ = 0.0f;
    }
}

bool Painter::BeginFrame(const Theme& theme, bool high_contrast) {
    if (!compositor_ || !compositor_->Dc()) {
        dc_ = nullptr;
        theme_ = nullptr;
        return false;
    }

    dc_ = compositor_->Dc();
    theme_ = &theme;
    high_contrast_ = high_contrast;
    dark_ = 0.2126f * theme.bg.r + 0.7152f * theme.bg.g + 0.0722f * theme.bg.b < 0.5f;
    dc_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    EnsureTextFormats();
    EnsureStrokeStyle();

    UpdateBrush(BrushId::Background, theme.bg);
    UpdateBrush(BrushId::Text, theme.text);
    UpdateBrush(BrushId::TextSecondary, theme.text_secondary);
    UpdateBrush(BrushId::TextDisabled, theme.text_disabled);
    UpdateBrush(BrushId::Hover, theme.fill_hover);
    UpdateBrush(BrushId::Pressed, theme.fill_pressed);
    UpdateBrush(BrushId::Selected, theme.fill_selected);
    UpdateBrush(BrushId::Input, theme.fill_input);
    UpdateBrush(BrushId::InputHover, theme.fill_input_hover);
    UpdateBrush(BrushId::StrokeCard, theme.stroke_card);
    UpdateBrush(BrushId::Divider, theme.stroke_divider);
    UpdateBrush(BrushId::InputBottom, theme.stroke_input_bottom);
    UpdateBrush(BrushId::Flyout, theme.surface_flyout);
    UpdateBrush(BrushId::Accent, theme.accent);
    UpdateBrush(BrushId::AccentHover, theme.accent_hover);
    UpdateBrush(BrushId::AccentPressed, theme.accent_pressed);
    UpdateBrush(BrushId::AccentText, theme.accent_text);
    UpdateBrush(BrushId::AccentTextPressed, MultiplyAlpha(theme.accent_text, 0.63f));
    UpdateBrush(BrushId::Danger, theme.danger);
    UpdateBrush(BrushId::DangerHover, theme.danger_hover);
    UpdateBrush(BrushId::CloseText, HexColor(0xFFFFFF));
    UpdateBrush(BrushId::Scrollbar, theme.scrollbar_thumb);
    UpdateBrush(BrushId::TabActive, theme.tab_active_bg);
    return true;
}

void Painter::EnsureTextFormats() {
    if (!compositor_ || !compositor_->DwriteFactory() ||
        (body_format_.get() && caption_format_.get() &&
         small_icon_format_.get() && micro_icon_format_.get() &&
         std::abs(format_scale_ - scale_) <= 0.001f)) {
        return;
    }
    body_format_.reset();
    nav_format_.reset();
    section_format_.reset();
    caption_format_.reset();
    small_icon_format_.reset();
    micro_icon_format_.reset();
    ellipsis_sign_.reset();
    auto create = [this](float size, DWRITE_FONT_WEIGHT weight,
                         ComPtr<IDWriteTextFormat>& format) {
        typography::CreateTextFormat(compositor_->DwriteFactory(),
            {typography::FontRole::Text, size * scale_, weight}, &format);
        if (format.get()) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            format->SetTrimming(&trimming, nullptr);
        }
    };
    auto create_icon = [this](float size, ComPtr<IDWriteTextFormat>& format) {
        typography::CreateTextFormat(compositor_->DwriteFactory(),
            {typography::FontRole::Icon, size * scale_}, &format);
        if (format.get()) {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    };
    create(14.0f, DWRITE_FONT_WEIGHT_NORMAL, body_format_);
    create(13.0f, DWRITE_FONT_WEIGHT_NORMAL, nav_format_);
    create(13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, section_format_);
    create(12.0f, DWRITE_FONT_WEIGHT_NORMAL, caption_format_);
    create_icon(12.0f, small_icon_format_);
    create_icon(10.0f, micro_icon_format_);
    if (body_format_.get())
        compositor_->DwriteFactory()->CreateEllipsisTrimmingSign(
            body_format_.get(), &ellipsis_sign_);
    format_scale_ = scale_;
}

void Painter::EnsureStrokeStyle() {
    if (!dc_ || round_stroke_.get()) {
        return;
    }
    ComPtr<ID2D1Factory> factory;
    dc_->GetFactory(&factory);
    if (!factory.get()) {
        return;
    }
    const auto properties = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
    factory->CreateStrokeStyle(properties, nullptr, 0, &round_stroke_);
}

IDWriteTextFormat* Painter::BodyFormat() const noexcept {
    return body_format_.get() ? body_format_.get()
                              : compositor_ ? compositor_->TextFormat() : nullptr;
}

IDWriteTextFormat* Painter::CaptionFormat() const noexcept {
    return caption_format_.get() ? caption_format_.get()
                                 : compositor_ ? compositor_->SmallFormat() : nullptr;
}

IDWriteTextFormat* Painter::NavFormat() const noexcept {
    return nav_format_.get() ? nav_format_.get() : BodyFormat();
}

IDWriteTextFormat* Painter::SectionFormat() const noexcept {
    return section_format_.get() ? section_format_.get() : CaptionFormat();
}

IDWriteTextFormat* Painter::SmallIconFormat() const noexcept {
    return small_icon_format_.get() ? small_icon_format_.get()
                                    : compositor_ ? compositor_->IconFormat() : nullptr;
}

IDWriteTextFormat* Painter::MicroIconFormat() const noexcept {
    return micro_icon_format_.get() ? micro_icon_format_.get()
                                    : SmallIconFormat();
}

void Painter::UpdateBrush(BrushId id, const D2D1_COLOR_F& color) {
    auto& brush = brushes_[static_cast<size_t>(id)];
    if (brush.get()) {
        brush->SetColor(color);
    } else if (dc_) {
        dc_->CreateSolidColorBrush(color, &brush);
    }
}

ID2D1SolidColorBrush* Painter::Brush(BrushId id) const noexcept {
    return brushes_[static_cast<size_t>(id)].get();
}

ID2D1SolidColorBrush* Painter::ScratchBrush(const D2D1_COLOR_F& color) {
    if (!dc_) {
        return nullptr;
    }
    if (scratch_brush_.get()) {
        scratch_brush_->SetColor(color);
    } else {
        dc_->CreateSolidColorBrush(color, &scratch_brush_);
    }
    return scratch_brush_.get();
}

float Painter::Px(float dips) const noexcept {
    return dips * scale_;
}

void Painter::Fill(const D2D1_RECT_F& bounds, BrushId brush) {
    if (dc_ && Width(bounds) > 0.0f && Height(bounds) > 0.0f) {
        dc_->FillRectangle(bounds, Brush(brush));
    }
}

void Painter::FillRounded(const D2D1_RECT_F& bounds, float radius, BrushId brush) {
    if (dc_ && Width(bounds) > 0.0f && Height(bounds) > 0.0f) {
        dc_->FillRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), Brush(brush));
    }
}

void Painter::StrokeRounded(const D2D1_RECT_F& bounds, float radius, BrushId brush, float width) {
    if (!dc_ || Width(bounds) <= 0.0f || Height(bounds) <= 0.0f) {
        return;
    }
    const float stroke = std::max(1.0f, width);
    const auto aligned = Inset(bounds, stroke * 0.5f);
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(aligned, radius, radius), Brush(brush), stroke,
                              round_stroke_.get());
}

void Painter::FillRoundedRect(const D2D1_RECT_F& bounds, float radius,
                              const D2D1_COLOR_F& color) {
    if (dc_ && Width(bounds) > 0.0f && Height(bounds) > 0.0f) {
        dc_->FillRoundedRectangle(D2D1::RoundedRect(bounds, radius, radius), ScratchBrush(color));
    }
}

void Painter::StrokeRoundedRect(const D2D1_RECT_F& bounds, float radius,
                                const D2D1_COLOR_F& color, float width) {
    if (!dc_ || Width(bounds) <= 0.0f || Height(bounds) <= 0.0f) {
        return;
    }
    const float stroke = std::max(1.0f, width);
    const auto aligned = Inset(bounds, stroke * 0.5f);
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(aligned, radius, radius),
                              ScratchBrush(color), stroke, round_stroke_.get());
}

void Painter::DrawText(std::wstring_view text, const D2D1_RECT_F& bounds,
                       IDWriteTextFormat* format, const D2D1_COLOR_F& color,
                       HorizontalAlignment alignment) {
    DrawText(text, bounds, format, color, alignment,
             theme_ ? theme_->bg : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
}

void Painter::DrawText(std::wstring_view text, const D2D1_RECT_F& bounds,
                       IDWriteTextFormat* format, const D2D1_COLOR_F& color,
                       HorizontalAlignment alignment, const D2D1_COLOR_F& background) {
    if (!dc_ || !format || text.empty() || !compositor_ ||
        !compositor_->DwriteFactory()) {
        return;
    }

    if (DrawLegacyIcon(dc_, compositor_->DwriteFactory(), text, bounds, ScratchBrush(color), format->GetFontSize())) return;
    const D2D1_RECT_F snapped = typography::SnapVerticalBounds(bounds);
    const float width = snapped.right - snapped.left;
    const float height = snapped.bottom - snapped.top;
    if (width <= 0.0f || height <= 0.0f) return;
    DWRITE_TEXT_ALIGNMENT text_alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
    if (alignment == HorizontalAlignment::Center) {
        text_alignment = DWRITE_TEXT_ALIGNMENT_CENTER;
    } else if (alignment == HorizontalAlignment::Right) {
        text_alignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
    }
    if (!high_contrast_ && compositor_->DrawLumaText(
            text, format, snapped, color, background, text_alignment)) {
        return;
    }
    IDWriteTextLayout* layout = GetTextLayout(
        compositor_, format, text, width, height, text_alignment);
    if (!layout) return;
    dc_->DrawTextLayout(D2D1::Point2F(snapped.left, snapped.top), layout,
                        ScratchBrush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Painter::DrawTextWithBrush(std::wstring_view text, const D2D1_RECT_F& bounds,
                                IDWriteTextFormat* format, BrushId brush,
                                HorizontalAlignment alignment) {
    if (!dc_ || !format || text.empty() || !compositor_ ||
        !compositor_->DwriteFactory()) {
        return;
    }

    const D2D1_RECT_F snapped = typography::SnapVerticalBounds(bounds);
    const float width = snapped.right - snapped.left;
    const float height = snapped.bottom - snapped.top;
    if (width <= 0.0f || height <= 0.0f) return;
    const DWRITE_TEXT_ALIGNMENT text_alignment = alignment == HorizontalAlignment::Center
        ? DWRITE_TEXT_ALIGNMENT_CENTER
        : alignment == HorizontalAlignment::Right
            ? DWRITE_TEXT_ALIGNMENT_TRAILING
            : DWRITE_TEXT_ALIGNMENT_LEADING;
    ID2D1SolidColorBrush* text_brush = Brush(brush);
    if (DrawLegacyIcon(dc_, compositor_->DwriteFactory(), text, bounds, text_brush, format->GetFontSize())) return;
    if (!high_contrast_ && theme_ && text_brush && compositor_->DrawLumaText(
            text, format, snapped, text_brush->GetColor(), theme_->bg, text_alignment)) {
        return;
    }
    IDWriteTextLayout* layout = GetTextLayout(
        compositor_, format, text, width, height, text_alignment);
    if (!layout) return;
    dc_->DrawTextLayout(D2D1::Point2F(snapped.left, snapped.top), layout,
                        text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Painter::DrawGlyph(std::wstring_view glyph, const D2D1_RECT_F& bounds,
                        const D2D1_COLOR_F& color) {
    if (!compositor_) {
        return;
    }
    DrawText(glyph, bounds, compositor_->IconFormat(), color, HorizontalAlignment::Center);
}

void Painter::DrawGlyphWithBrush(std::wstring_view glyph, const D2D1_RECT_F& bounds,
                                 BrushId brush) {
    if (!compositor_) {
        return;
    }
    DrawTextWithBrush(glyph, bounds, compositor_->IconFormat(), brush,
                      HorizontalAlignment::Center);
}

void Painter::DrawGlyphWithFormat(std::wstring_view glyph, const D2D1_RECT_F& bounds,
                                  const D2D1_COLOR_F& color, IDWriteTextFormat* format) {
    DrawText(glyph, bounds, format, color, HorizontalAlignment::Center);
}

void Painter::DrawCheckMark(const D2D1_RECT_F& bounds, const D2D1_COLOR_F& color) {
    if (!dc_) {
        return;
    }
    const auto first = D2D1::Point2F(bounds.left + Px(4.2f),
                                    (bounds.top + bounds.bottom) * 0.5f);
    const auto middle = D2D1::Point2F(bounds.left + Px(7.6f),
                                     bounds.bottom - Px(4.2f));
    const auto last = D2D1::Point2F(bounds.right - Px(3.5f),
                                   bounds.top + Px(4.0f));
    const float stroke = std::max(1.0f, Px(1.7f));
    auto* brush = ScratchBrush(color);
    dc_->DrawLine(first, middle, brush, stroke, round_stroke_.get());
    dc_->DrawLine(middle, last, brush, stroke, round_stroke_.get());
}

void Painter::DrawPaneLayoutIcon(const D2D1_RECT_F& bounds, MenuPictogram kind,
                                 const D2D1_COLOR_F& color) {
    if (!dc_ || kind == MenuPictogram::None) {
        return;
    }
    EnsureStrokeStyle();
    const float box = std::min(Width(bounds), Height(bounds)) * 0.72f;
    if (box < 6.0f) {
        return;
    }
    const float x0 = bounds.left + (Width(bounds) - box) * 0.5f;
    const float y0 = bounds.top + (Height(bounds) - box) * 0.5f;
    const float stroke = std::max(1.0f, Px(1.4f));
    const float radius = std::min(Px(2.25f), box * 0.18f);
    StrokeRoundedRect(D2D1::RectF(x0, y0, x0 + box, y0 + box), radius, color, stroke);
    if (kind == MenuPictogram::LayoutSingle) {
        return;
    }

    const float inset = stroke;
    const float left = x0 + inset;
    const float top = y0 + inset;
    const float right = x0 + box - inset;
    const float bottom = y0 + box - inset;
    const float mid_x = x0 + box * 0.5f;
    const float mid_y = y0 + box * 0.5f;
    auto* brush = ScratchBrush(color);
    auto vline = [&](float x, float y1, float y2) {
        dc_->DrawLine(D2D1::Point2F(x, y1), D2D1::Point2F(x, y2),
                      brush, stroke, round_stroke_.get());
    };
    auto hline = [&](float x1, float x2, float y) {
        dc_->DrawLine(D2D1::Point2F(x1, y), D2D1::Point2F(x2, y),
                      brush, stroke, round_stroke_.get());
    };
    switch (kind) {
    case MenuPictogram::LayoutSideBySide:
        vline(mid_x, top, bottom);
        break;
    case MenuPictogram::LayoutStacked:
        hline(left, right, mid_y);
        break;
    case MenuPictogram::LayoutThree:
        vline(mid_x, top, bottom);
        hline(mid_x, right, mid_y);
        break;
    case MenuPictogram::LayoutFour:
        vline(mid_x, top, bottom);
        hline(left, right, mid_y);
        break;
    default:
        break;
    }
}

void Painter::DrawArc(D2D1_POINT_2F center, float radius, float start_degrees,
                      float span_degrees, float stroke_width,
                      const D2D1_COLOR_F& color) {
    if (!dc_ || radius <= 0.0f || std::abs(span_degrees) < 0.01f) {
        return;
    }
    const int segments = std::max(2, static_cast<int>(std::ceil(std::abs(span_degrees) / 2.0f)));
    const float start = start_degrees * 3.14159265f / 180.0f;
    const float span = span_degrees * 3.14159265f / 180.0f;
    auto* brush = ScratchBrush(color);
    D2D1_POINT_2F previous = D2D1::Point2F(center.x + std::cos(start) * radius,
                                           center.y + std::sin(start) * radius);
    for (int i = 1; i <= segments; ++i) {
        const float angle = start + span * static_cast<float>(i) / static_cast<float>(segments);
        const auto point = D2D1::Point2F(center.x + std::cos(angle) * radius,
                                        center.y + std::sin(angle) * radius);
        dc_->DrawLine(previous, point, brush, stroke_width, round_stroke_.get());
        previous = point;
    }
    const float cap = stroke_width * 0.5f;
    const float end = start + span;
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x + std::cos(start) * radius,
                                                center.y + std::sin(start) * radius),
                                   cap, cap), brush);
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(center.x + std::cos(end) * radius,
                                                center.y + std::sin(end) * radius),
                                   cap, cap), brush);
}

void Painter::DrawFocusRing(const D2D1_RECT_F& bounds, float radius) {
    if (!theme_) {
        return;
    }
    const float width = std::max(1.0f, Px(theme_->focus_ring));
    StrokeRounded(Inflate(bounds, Px(1.0f)), radius + Px(1.0f), BrushId::Accent, width);
}

void Painter::DrawButton(const ButtonSpec& spec) {
    if (!theme_ || !compositor_) {
        return;
    }

    const auto& state = spec.state;
    const bool toggle = spec.kind == ButtonKind::Toggle ||
                        spec.kind == ButtonKind::TransparentToggle;
    const bool danger = spec.kind == ButtonKind::Danger;
    const bool primary = spec.kind == ButtonKind::Primary || (toggle && state.checked);
    const bool transparent = spec.kind == ButtonKind::Transparent ||
                             (spec.kind == ButtonKind::TransparentToggle && !state.checked);
    const float radius = Px(theme_->radius_control);
    D2D1_COLOR_F foreground = dark_ ? HexColor(0xFFFFFF) : HexColor(0x000000);
    D2D1_COLOR_F fill{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F edge{};
    bool draw_fill = true;
    bool draw_border = !transparent && spec.bordered;
    bool edge_on_top = dark_;

    if (high_contrast_) {
        draw_fill = !transparent || (state.enabled && (state.hovered || state.pressed));
        draw_border = !transparent && spec.bordered;
        fill = !state.enabled ? theme_->fill_input
               : primary ? theme_->accent
               : danger ? theme_->danger
                       : state.hovered || state.pressed ? theme_->fill_hover : theme_->fill_input;
        border = theme_->stroke_card;
        edge = border;
        foreground = state.enabled
            ? ((primary || danger) ? theme_->accent_text : theme_->text)
            : theme_->text_disabled;
        if (danger && state.enabled) foreground = HexColor(0xFFFFFF);
    } else if (danger) {
        if (!state.enabled) {
            fill = dark_ ? HexColor(0x343434) : HexColor(0xCDCDCD);
            border = fill;
            edge = fill;
            foreground = dark_ ? RgbaF(0xFFFFFF, 0.43f) : RgbaF(0xFFFFFF, 0.90f);
        } else {
            fill = state.pressed || state.hovered ? theme_->danger_hover : theme_->danger;
            border = fill;
            edge = fill;
            foreground = HexColor(0xFFFFFF);
        }
    } else if (primary) {
        const AccentShades shades = DeriveAccentShades(theme_->accent);
        if (!state.enabled) {
            fill = dark_ ? HexColor(0x343434) : HexColor(0xCDCDCD);
            border = fill;
            edge = fill;
            foreground = dark_ ? RgbaF(0xFFFFFF, 0.43f) : RgbaF(0xFFFFFF, 0.90f);
        } else if (dark_) {
            fill = state.pressed ? shades.dark2 : state.hovered ? shades.dark1 : theme_->accent;
            border = state.pressed ? shades.dark2 : shades.light1;
            edge = state.pressed ? shades.dark2 : shades.light2;
            edge_on_top = false;
            foreground = state.pressed ? RgbaF(0x000000, 0.63f) : HexColor(0x000000);
        } else {
            fill = state.pressed ? shades.light3 : state.hovered ? shades.light1 : theme_->accent;
            border = state.pressed ? shades.light3 : state.hovered ? shades.light2 : shades.light1;
            edge = state.pressed ? shades.light3 : shades.dark1;
            edge_on_top = false;
            foreground = state.pressed ? RgbaF(0xFFFFFF, 0.63f) : HexColor(0xFFFFFF);
        }
        if (spec.kind == ButtonKind::TransparentToggle && state.enabled) {
            foreground = state.pressed
                ? MultiplyAlpha(theme_->accent_text, 0.63f)
                : theme_->accent_text;
        }
        if (!spec.bordered && state.enabled) {
            foreground = state.pressed
                ? MultiplyAlpha(theme_->accent_text, 0.63f)
                : theme_->accent_text;
        }
    } else if (transparent) {
        draw_border = false;
        draw_fill = state.enabled && (state.hovered || state.pressed);
        fill = dark_ ? Rgba(0xFFFFFF, state.pressed ? 6 : 9)
                     : Rgba(0x000000, state.pressed ? 6 : 9);
        if (!state.enabled) {
            foreground = dark_ ? RgbaF(0xFFFFFF, 0.3628f) : RgbaF(0x000000, 0.36f);
        } else if (state.pressed) {
            foreground = MultiplyAlpha(foreground, dark_ ? 0.786f : 0.63f);
        }
    } else if (dark_) {
        fill = !state.enabled ? RgbaF(0xFFFFFF, 0.0419f)
               : state.pressed ? RgbaF(0xFFFFFF, 0.0326f)
               : state.hovered ? RgbaF(0xFFFFFF, 0.0837f)
                               : RgbaF(0xFFFFFF, 0.0605f);
        border = RgbaF(0xFFFFFF, 0.053f);
        edge = state.enabled && !state.pressed ? RgbaF(0xFFFFFF, 0.08f) : border;
        foreground = !state.enabled ? RgbaF(0xFFFFFF, 0.3628f)
                     : state.pressed ? RgbaF(0xFFFFFF, 0.786f) : HexColor(0xFFFFFF);
    } else {
        fill = !state.enabled || state.pressed ? RgbaF(0xF9F9F9, 0.30f)
               : state.hovered ? RgbaF(0xF9F9F9, 0.50f) : RgbaF(0xFFFFFF, 0.70f);
        border = !state.enabled ? RgbaF(0x000000, 0.06f) : RgbaF(0x000000, 0.073f);
        edge = !state.enabled ? border
                              : state.pressed ? border : RgbaF(0x000000, 0.183f);
        foreground = !state.enabled ? RgbaF(0x000000, 0.36f)
                     : state.pressed ? RgbaF(0x000000, 0.63f) : HexColor(0x000000);
    }

    if (draw_fill) {
        FillRoundedRect(spec.bounds, radius, fill);
    }
    if (draw_border) {
        StrokeRoundedRect(spec.bounds, radius, border);
        const float y = edge_on_top ? spec.bounds.top + 0.5f : spec.bounds.bottom - 0.5f;
        dc_->DrawLine(D2D1::Point2F(spec.bounds.left + radius, y),
                      D2D1::Point2F(spec.bounds.right - radius, y),
                      ScratchBrush(edge), 1.0f);
    }

    const float pad_x = Px(8.0f);
    const float line = BodyFormat() ? BodyFormat()->GetFontSize() * 1.35f : Px(19.0f);
    float pad_y = Px(4.0f);
    if (Height(spec.bounds) - pad_y * 2.0f < line)
        pad_y = std::max(0.0f, (Height(spec.bounds) - line) * 0.5f);
    D2D1_RECT_F content = spec.bounds;
    content.left += pad_x;
    content.right -= pad_x;
    content.top += pad_y;
    content.bottom -= pad_y;
    if (spec.icon_only) {
        if (!spec.skip_glyph) DrawGlyph(spec.glyph, content, foreground);
    } else if (Width(content) > 0.0f && Height(content) > 0.0f) {
        float left = content.left;
        if (!spec.glyph.empty()) {
            const float icon_width = Px(20.0f);
            if (!spec.skip_glyph) {
                DrawGlyph(spec.glyph,
                          D2D1::RectF(left, content.top, left + icon_width, content.bottom),
                          foreground);
            }
            left += icon_width + Px(4.0f);
        }
        float right = content.right;
        if (spec.drop_down) {
            const float arrow_width = Px(16.0f);
            DrawGlyph(kChevronDown,
                      D2D1::RectF(right - arrow_width, content.top, right, content.bottom),
                      foreground);
            right -= arrow_width + Px(4.0f);
        }
        IDWriteTextFormat* body = BodyFormat();
        DWRITE_WORD_WRAPPING old_wrap = DWRITE_WORD_WRAPPING_WRAP;
        if (body) {
            old_wrap = body->GetWordWrapping();
            body->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        const bool center = spec.glyph.empty() && !spec.drop_down;
        DrawText(spec.text, D2D1::RectF(left, content.top, right, content.bottom),
                 body, foreground,
                 center ? HorizontalAlignment::Center : HorizontalAlignment::Left);
        if (body) body->SetWordWrapping(old_wrap);
    }

    if (state.keyboard_focus) {
        DrawFocusRing(spec.bounds, radius);
    }
}

void Painter::DrawTextFieldFrame(const D2D1_RECT_F& bounds, const ControlState& state,
                                 bool hosted_edit) {
    if (!theme_ || !dc_ || Width(bounds) <= 0.0f || Height(bounds) <= 0.0f) {
        return;
    }

    const float radius = Px(theme_->radius_control);
    const D2D1_COLOR_F fill = TextFieldFillColor(*theme_, state, dark_, high_contrast_,
                                                hosted_edit);
    D2D1_COLOR_F border;
    D2D1_COLOR_F bottom;
    if (high_contrast_) {
        border = theme_->stroke_card;
        bottom = state.focused ? theme_->accent : theme_->stroke_input_bottom;
    } else if (dark_) {
        border = !state.enabled ? RgbaF(0xFFFFFF, 0.0698f)
                                : RgbaF(0xFFFFFF, 0.08f);
        bottom = state.focused ? theme_->accent : theme_->stroke_input_bottom;
    } else {
        border = Rgba(0x000000, 13);
        bottom = state.focused ? theme_->accent : theme_->stroke_input_bottom;
    }

    FillRoundedRect(bounds, radius, fill);
    StrokeRoundedRect(bounds, radius, border);

    if (state.focused && state.enabled) {
        const float arc = std::max(2.0f, Px(2.0f));
        const float bowl = Px(10.0f);
        const auto clip = D2D1::RectF(bounds.left, bounds.bottom - arc,
                                      bounds.right, bounds.bottom);
        dc_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        FillRoundedRect(D2D1::RectF(bounds.left, bounds.bottom - bowl,
                                    bounds.right, bounds.bottom),
                        radius, bottom);
        dc_->PopAxisAlignedClip();
    } else if (state.enabled) {
        const auto clip = D2D1::RectF(bounds.left + 1.0f,
                                      bounds.bottom - std::max(1.5f, Px(1.5f)),
                                      bounds.right - 1.0f,
                                      bounds.bottom);
        dc_->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        StrokeRoundedRect(bounds, radius, bottom);
        dc_->PopAxisAlignedClip();
    }
}

void Painter::DrawTextField(const TextFieldSpec& spec) {
    if (!theme_ || !compositor_) {
        return;
    }

    DrawTextFieldFrame(spec.bounds, spec.state, spec.hosted_edit);

    D2D1_COLOR_F foreground;
    D2D1_COLOR_F placeholder_color;
    if (high_contrast_) {
        foreground = spec.state.enabled ? theme_->text : theme_->text_disabled;
        placeholder_color = theme_->text_secondary;
    } else if (dark_) {
        foreground = spec.state.enabled ? HexColor(0xFFFFFF) : Rgba(0xFFFFFF, 92);
        placeholder_color = RgbaF(0xFFFFFF, 0.6063f);
    } else {
        foreground = spec.state.enabled ? HexColor(0x000000) : Rgba(0x000000, 92);
        placeholder_color = RgbaF(0x000000, 0.6063f);
    }

    auto content = Inset(spec.bounds, Px(10.0f));
    content.top = spec.bounds.top;
    content.bottom = spec.bounds.bottom;
    if (!spec.leading_glyph.empty()) {
        const float icon_width = Px(18.0f);
        DrawGlyphWithFormat(
            spec.leading_glyph,
            D2D1::RectF(content.left, content.top,
                        content.left + icon_width, content.bottom),
            spec.state.enabled ? placeholder_color : theme_->text_disabled,
            spec.compact_leading_glyph ? SmallIconFormat()
                                       : compositor_->IconFormat());
        content.left += icon_width + Px(6.0f);
    }
    float trailing = 0.0f;
    if (!spec.trailing_glyph.empty() || spec.trailing_width > 0.0f) {
        trailing = spec.trailing_width > 0.0f ? Px(spec.trailing_width) : Px(18.0f);
    }
    if (!spec.trailing_keycap.empty())
        trailing += MeasureBadgeWidth(spec.trailing_keycap) + Px(6.0f);
    if (!spec.trailing_badge.empty())
        trailing += MeasureBadgeWidth(spec.trailing_badge) + Px(6.0f);
    if (trailing > 0.0f)
        content.right -= trailing;
    float badge_right = spec.bounds.right - Px(8.0f);
    auto draw_trailing_badge = [&](std::wstring_view label, BadgeKind kind) {
        if (label.empty()) return;
        const float w = MeasureBadgeWidth(label);
        const float y0 = spec.bounds.top + Px(3.0f);
        const float y1 = spec.bounds.bottom - Px(3.0f);
        DrawBadge({D2D1::RectF(badge_right - w, y0, badge_right, y1), label, kind});
        badge_right -= w + Px(6.0f);
    };
    draw_trailing_badge(spec.trailing_keycap, BadgeKind::Keycap);
    draw_trailing_badge(spec.trailing_badge, BadgeKind::Keycap);
    if (!spec.trailing_glyph.empty()) {
        const float icon_width = spec.trailing_width > 0.0f
                                     ? Px(spec.trailing_width) : Px(18.0f);
        DrawGlyphWithFormat(spec.trailing_glyph,
                            D2D1::RectF(content.right, content.top,
                                       content.right + icon_width, content.bottom),
                            foreground, SmallIconFormat());
    }
    if (!spec.suppress_text) {
        const bool is_placeholder = spec.text.empty();
        const D2D1_COLOR_F luma_bg = BlendOver(
            TextFieldFillColor(*theme_, spec.state, dark_, high_contrast_,
                               spec.hosted_edit), theme_->bg);
        DrawText(is_placeholder ? spec.placeholder : spec.text, content,
                 BodyFormat(), is_placeholder ? placeholder_color : foreground,
                 HorizontalAlignment::Left, luma_bg);
    }

    if (spec.state.keyboard_focus) {
        DrawFocusRing(spec.bounds, Px(theme_->radius_control));
    }
}

void Painter::DrawCheckBox(const D2D1_RECT_F& bounds, std::wstring_view text,
                           const ControlState& state) {
    if (!theme_ || !compositor_) {
        return;
    }
    const float indicator = Px(18.0f);
    const float top = bounds.top + (Height(bounds) - indicator) * 0.5f;
    const auto box = D2D1::RectF(bounds.left, top, bounds.left + indicator, top + indicator);
    const float radius = Px(4.5f);
    D2D1_COLOR_F border;
    D2D1_COLOR_F fill;
    D2D1_COLOR_F foreground = state.enabled ? theme_->text : theme_->text_disabled;
    if (high_contrast_) {
        border = !state.enabled ? theme_->text_disabled
                 : state.checked ? theme_->accent : theme_->stroke_input_bottom;
        fill = !state.enabled ? (state.checked ? theme_->text_disabled : theme_->fill_input)
               : state.checked ? theme_->accent : theme_->fill_input;
    } else if (state.checked && state.enabled) {
        const AccentShades shades = DeriveAccentShades(theme_->accent);
        fill = state.pressed ? (dark_ ? shades.dark2 : shades.light2)
               : state.hovered ? (dark_ ? shades.dark1 : shades.light1)
                               : theme_->accent;
        border = fill;
    } else if (state.checked) {
        fill = dark_ ? Rgba(0xFFFFFF, 41) : Rgba(0x000000, 56);
        border = Rgba(0x000000, 0);
    } else if (dark_) {
        border = !state.enabled ? Rgba(0xFFFFFF, 41)
                 : state.pressed ? Rgba(0xFFFFFF, 40) : Rgba(0xFFFFFF, 141);
        fill = !state.enabled ? Rgba(0x000000, 0)
               : state.pressed ? Rgba(0xFFFFFF, 18)
               : state.hovered ? Rgba(0xFFFFFF, 11) : Rgba(0x000000, 26);
    } else {
        border = !state.enabled ? Rgba(0x000000, 56)
                 : state.pressed ? Rgba(0x000000, 69)
                 : state.hovered ? Rgba(0x000000, 143) : Rgba(0x000000, 122);
        fill = !state.enabled ? Rgba(0x000000, 0)
               : state.pressed ? Rgba(0x000000, 31)
               : state.hovered ? Rgba(0x000000, 13) : Rgba(0x000000, 6);
    }
    FillRoundedRect(box, radius, fill);
    StrokeRoundedRect(box, radius, border);
    if (state.checked) {
        D2D1_COLOR_F check_color = high_contrast_ ? theme_->accent_text
                                                  : dark_ ? HexColor(0x000000)
                                                          : HexColor(0xFFFFFF);
        if (!state.enabled) {
            check_color = MultiplyAlpha(check_color, 0.8f);
        }
        DrawCheckMark(box, check_color);
    }
    DrawText(text,
             D2D1::RectF(box.right + Px(8.0f), bounds.top, bounds.right, bounds.bottom),
             BodyFormat(), foreground);
    if (state.keyboard_focus) {
        DrawFocusRing(box, radius);
    }
}

void Painter::DrawRadioButton(const D2D1_RECT_F& bounds, std::wstring_view text,
                              const ControlState& state) {
    if (!theme_ || !compositor_) {
        return;
    }
    const float indicator = Px(20.0f);
    const float top = bounds.top + (Height(bounds) - indicator) * 0.5f;
    const auto center = D2D1::Point2F(bounds.left + indicator * 0.5f,
                                     top + indicator * 0.5f);
    const float outer_radius = indicator * 0.5f;
    D2D1_COLOR_F border;
    D2D1_COLOR_F fill;
    float thickness = Px(1.0f);
    if (high_contrast_) {
        border = !state.enabled ? theme_->text_disabled
                 : state.checked ? theme_->accent : theme_->stroke_input_bottom;
        fill = state.checked ? theme_->bg : theme_->fill_input;
        thickness = state.checked ? Px(5.0f) : Px(1.0f);
    } else if (state.checked) {
        border = state.enabled ? theme_->accent
                               : dark_ ? Rgba(0xFFFFFF, 40) : Rgba(0x000000, 55);
        fill = dark_ ? HexColor(0x000000) : HexColor(0xFFFFFF);
        thickness = state.hovered && !state.pressed ? Px(4.0f) : Px(5.0f);
    } else if (dark_) {
        border = state.enabled && !state.pressed ? Rgba(0xFFFFFF, 153)
                                                 : Rgba(0xFFFFFF, 40);
        fill = !state.enabled ? Rgba(0x000000, 0)
               : state.pressed ? HexColor(0x000000)
               : state.hovered ? Rgba(0xFFFFFF, 11) : Rgba(0x000000, 26);
    } else {
        border = state.enabled && !state.pressed ? Rgba(0x000000, 153)
                                                 : Rgba(0x000000, 55);
        fill = !state.enabled ? Rgba(0x000000, 0)
               : state.pressed ? HexColor(0xFFFFFF)
               : state.hovered ? Rgba(0x000000, 15) : Rgba(0x000000, 6);
    }

    dc_->FillEllipse(D2D1::Ellipse(center, outer_radius - thickness * 0.5f,
                                   outer_radius - thickness * 0.5f),
                     ScratchBrush(fill));
    dc_->DrawEllipse(D2D1::Ellipse(center, outer_radius - thickness * 0.5f,
                                   outer_radius - thickness * 0.5f),
                     ScratchBrush(border), thickness);
    if (!state.checked && state.enabled && state.pressed) {
        const D2D1_COLOR_F pressed_ring = dark_ ? Rgba(0xFFFFFF, 40) : Rgba(0x000000, 24);
        dc_->DrawEllipse(D2D1::Ellipse(center, Px(7.0f), Px(7.0f)),
                         ScratchBrush(pressed_ring), Px(4.0f));
    }
    DrawText(text,
             D2D1::RectF(bounds.left + indicator + Px(8.0f), bounds.top,
                         bounds.right, bounds.bottom),
             BodyFormat(), state.enabled ? theme_->text : theme_->text_disabled);
    if (state.keyboard_focus) {
        DrawFocusRing(D2D1::RectF(bounds.left, top, bounds.left + indicator, top + indicator),
                      indicator * 0.5f);
    }
}

void Painter::DrawSwitch(const D2D1_RECT_F& bounds, std::wstring_view text,
                         const ControlState& state) {
    if (!theme_ || !compositor_) {
        return;
    }
    const float switch_width = Px(42.0f);
    const float switch_height = Px(22.0f);
    const float top = bounds.top + (Height(bounds) - switch_height) * 0.5f;
    const auto track = D2D1::RectF(bounds.left, top, bounds.left + switch_width,
                                  top + switch_height);
    D2D1_COLOR_F fill;
    D2D1_COLOR_F border;
    D2D1_COLOR_F knob;
    if (high_contrast_) {
        fill = !state.enabled ? theme_->fill_input
               : state.checked ? theme_->accent : theme_->fill_input;
        border = !state.enabled ? theme_->text_disabled
                 : state.checked ? theme_->accent : theme_->stroke_input_bottom;
        knob = !state.enabled ? theme_->text_disabled
               : state.checked ? theme_->accent_text : theme_->text;
    } else if (state.checked) {
        const AccentShades shades = DeriveAccentShades(theme_->accent);
        fill = !state.enabled ? (dark_ ? Rgba(0xFFFFFF, 41) : Rgba(0x000000, 56))
               : state.pressed ? shades.light2
               : state.hovered ? shades.light1 : theme_->accent;
        border = state.enabled ? fill : Rgba(0x000000, 0);
        knob = !state.enabled ? (dark_ ? Rgba(0xFFFFFF, 77) : HexColor(0xFFFFFF))
                              : dark_ ? HexColor(0x000000) : HexColor(0xFFFFFF);
    } else if (dark_) {
        fill = !state.enabled ? Rgba(0x000000, 0)
               : state.pressed ? Rgba(0xFFFFFF, 18)
               : state.hovered ? Rgba(0xFFFFFF, 10) : Rgba(0x000000, 0);
        border = state.enabled ? Rgba(0xFFFFFF, 153) : Rgba(0xFFFFFF, 41);
        knob = state.enabled ? Rgba(0xFFFFFF, 201) : Rgba(0xFFFFFF, 96);
    } else {
        fill = !state.enabled ? Rgba(0x000000, 0)
               : state.pressed ? Rgba(0x000000, 23)
               : state.hovered ? Rgba(0x000000, 15) : Rgba(0x000000, 0);
        border = state.enabled ? Rgba(0x000000, 133) : Rgba(0x000000, 56);
        knob = state.enabled ? Rgba(0x000000, 156) : Rgba(0x000000, 91);
    }
    const auto painted_track = Inset(track, Px(1.0f));
    FillRoundedRect(painted_track, switch_height * 0.5f, fill);
    StrokeRoundedRect(painted_track, switch_height * 0.5f, border);
    const float position = state.check_progress < 0.0f
                               ? (state.checked ? 1.0f : 0.0f)
                               : Clamp01(state.check_progress);
    const float knob_radius = Px(6.0f);
    const float knob_x = track.left + Px(11.0f) + Px(20.0f) * position;
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x, top + switch_height * 0.5f),
                                   knob_radius, knob_radius),
                     ScratchBrush(knob));
    DrawText(text,
             D2D1::RectF(track.right + Px(12.0f), bounds.top, bounds.right, bounds.bottom),
             BodyFormat(), state.enabled ? theme_->text : theme_->text_disabled);
    if (state.keyboard_focus) {
        DrawFocusRing(track, switch_height * 0.5f);
    }
}

void Painter::DrawTitleBarButton(const D2D1_RECT_F& bounds, TitleBarButtonRole role,
                                 std::wstring_view custom_glyph,
                                 const ControlState& state) {
    if (!theme_) {
        return;
    }
    D2D1_COLOR_F fill{};
    D2D1_COLOR_F foreground = state.enabled ? theme_->text : theme_->text_disabled;
    bool draw_fill = false;
    if (state.enabled && (state.hovered || state.pressed)) {
        draw_fill = true;
        if (role == TitleBarButtonRole::Close) {
            fill = HexColor(0xC42B1C);
            foreground = HexColor(0xFFFFFF);
        } else {
            fill = high_contrast_ ? (state.pressed ? theme_->fill_pressed : theme_->fill_hover)
                                  : dark_ ? Rgba(0xFFFFFF, state.pressed ? 7 : 10)
                                          : Rgba(0x000000, state.pressed ? 7 : 10);
        }
    } else if (state.enabled && state.checked) {
        draw_fill = true;
        fill = high_contrast_ ? theme_->fill_selected
                              : dark_ ? Rgba(0xFFFFFF, 12) : Rgba(0x000000, 12);
    }
    if (draw_fill) {
        dc_->FillRectangle(bounds, ScratchBrush(fill));
    }

    std::wstring_view glyph = custom_glyph;
    if (role == TitleBarButtonRole::Minimize) glyph = kMinimize;
    if (role == TitleBarButtonRole::Maximize) glyph = kMaximize;
    if (role == TitleBarButtonRole::Restore) glyph = kRestore;
    if (role == TitleBarButtonRole::Close) glyph = kClose;
    DrawGlyphWithFormat(glyph, Inset(bounds, Px(12.0f)), foreground,
                        SmallIconFormat());
}

void Painter::DrawTab(const TabSpec& spec) {
    if (!theme_ || !compositor_) {
        return;
    }
    const float radius = Px(theme_->radius_control);
    ChromeTabShape shape;
    shape.top_radius = radius;
    shape.bottom_radius = Px(8.0f);
    shape.connect_bottom = spec.state.selected;
    if (spec.state.selected) {
        if (!high_contrast_) {
            auto shadow = spec.bounds;
            shadow.left += Px(1.0f);
            shadow.top += Px(2.0f);
            shadow.right -= Px(1.0f);
            shadow.bottom += Px(1.0f);
            FillChromeTab(dc_, ScratchBrush(Rgba(0x000000, dark_ ? 24 : 18)), shadow, shape);
        }
        const D2D1_COLOR_F selected_fill = high_contrast_ ? theme_->fill_selected
                                           : dark_ ? HexColor(0x282828) : HexColor(0xF9F9F9);
        FillChromeTab(dc_, ScratchBrush(selected_fill), spec.bounds, shape);
    }
    if (!spec.state.selected && spec.state.enabled &&
        (spec.state.hovered || spec.state.pressed)) {
        const D2D1_COLOR_F interaction = high_contrast_ ? (spec.state.pressed
                                                              ? theme_->fill_pressed
                                                              : theme_->fill_hover)
                                         : dark_ ? Rgba(0xFFFFFF, spec.state.pressed ? 12 : 15)
                                                 : Rgba(0x000000, spec.state.pressed ? 7 : 10);
        FillChromeTab(dc_, ScratchBrush(interaction), Inset(spec.bounds, 1.0f), shape);
    }
    auto content = Inset(spec.bounds, Px(10.0f));
    const D2D1_COLOR_F text_color = spec.state.enabled ? theme_->text : theme_->text_disabled;
    if (!spec.glyph.empty()) {
        const float icon_width = Px(18.0f);
        const D2D1_COLOR_F icon_color = spec.state.selected ? text_color
                                        : MultiplyAlpha(text_color, dark_ ? 0.79f : 0.61f);
        DrawGlyph(spec.glyph,
                  D2D1::RectF(content.left, content.top,
                             content.left + icon_width, content.bottom),
                  icon_color);
        content.left += icon_width + Px(6.0f);
    }
    if (spec.show_close) {
        const float close_width = Px(18.0f);
        const float close_pad = Px(6.0f);
        const D2D1_COLOR_F close_color = spec.state.enabled
                                            ? (dark_ ? HexColor(0xEAEAEA) : HexColor(0x484848))
                                            : theme_->text_disabled;
        DrawGlyphWithFormat(kClose,
                            D2D1::RectF(content.right - close_width - close_pad, content.top,
                                       content.right - close_pad, content.bottom),
                            close_color, MicroIconFormat());
        content.right -= close_width + close_pad + Px(6.0f);
    }
    DrawText(spec.text, content, BodyFormat(), text_color);
    if (spec.state.keyboard_focus) {
        DrawFocusRing(spec.bounds, radius);
    }
}

void Painter::DrawMenuSurface(const D2D1_RECT_F& bounds) {
    if (!theme_) {
        return;
    }
    const D2D1_COLOR_F fill = high_contrast_ ? theme_->surface_flyout
                              : dark_ ? HexColor(0x2B2B2B) : HexColor(0xF9F9F9);
    const D2D1_COLOR_F border = high_contrast_ ? theme_->stroke_card
                                : dark_ ? RgbaF(0xFFFFFF, 0.055f)
                                        : RgbaF(0x000000, 0.055f);
    const float radius = Px(theme_->radius_flyout);
    FillRoundedRect(bounds, radius, fill);
    StrokeRoundedRect(bounds, radius, border);
}

void Painter::DrawMenuItem(const MenuItemSpec& spec) {
    if (!theme_ || !compositor_) {
        return;
    }
    auto item = Inset(spec.bounds, Px(6.0f));
    item.top = spec.bounds.top + Px(2.0f);
    item.bottom = spec.bounds.bottom - Px(2.0f);
    if (spec.state.enabled &&
        (spec.state.hovered || spec.state.selected || spec.state.pressed)) {
        D2D1_COLOR_F interaction;
        if (high_contrast_) {
            interaction = spec.state.pressed ? theme_->fill_pressed : theme_->fill_hover;
        } else if (dark_) {
            interaction = spec.state.pressed ? RgbaF(0xFFFFFF, 0.06f)
                                             : RgbaF(0xFFFFFF, 0.08f);
        } else {
            interaction = spec.state.pressed ? RgbaF(0x000000, 0.06f)
                         : spec.state.selected ? Rgba(0x000000, 7) : Rgba(0x000000, 9);
        }
        FillRoundedRect(item, Px(5.0f), interaction);
    }
    D2D1_COLOR_F foreground = spec.state.enabled ? theme_->text
                                                  : dark_ ? RgbaF(0xFFFFFF, 0.40f)
                                                          : Rgba(0x000000, 112);
    if (spec.state.enabled && spec.state.pressed) {
        foreground = MultiplyAlpha(foreground, 0.70f);
    }
    // Menu padding is horizontal. Applying it vertically as well leaves only
    // 8-12 px for a 13 px body font at common DPI scales and clips CJK glyphs.
    auto content = item;
    content.left += spec.radio_group ? Px(4.0f) : Px(10.0f);
    content.right -= Px(10.0f);
    if (spec.radio_group) {
        const float radio_col = Px(12.0f);
        if (spec.radio) {
            const auto center = D2D1::Point2F(content.left + radio_col * 0.5f,
                                             (content.top + content.bottom) * 0.5f);
            dc_->FillEllipse(D2D1::Ellipse(center, Px(3.0f), Px(3.0f)), ScratchBrush(foreground));
        }
        content.left += radio_col;
    }
    const float icon_width = Px(20.0f);
    const auto icon_bounds = D2D1::RectF(content.left, content.top,
                                         content.left + icon_width, content.bottom);
    auto draw_glyph = [&]() {
        const float glyph_scale = std::clamp(spec.glyph_scale, 0.6f, 1.2f);
        if (std::abs(glyph_scale - 1.0f) < 0.001f || spec.glyph.empty()) {
            DrawGlyph(spec.glyph, icon_bounds, foreground);
            return;
        }
        ComPtr<IDWriteTextLayout> glyphLayout;
        IDWriteTextFormat* iconFormat = compositor_->IconFormat();
        const UINT32 length = static_cast<UINT32>(spec.glyph.size());
        if (iconFormat && SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(
                spec.glyph.data(), length, iconFormat,
                icon_bounds.right - icon_bounds.left,
                icon_bounds.bottom - icon_bounds.top, &glyphLayout)) && glyphLayout.get()) {
            glyphLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            glyphLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            glyphLayout->SetFontSize(iconFormat->GetFontSize() * glyph_scale,
                                     DWRITE_TEXT_RANGE{0, length});
            dc_->DrawTextLayout(D2D1::Point2F(icon_bounds.left, icon_bounds.top),
                                glyphLayout.get(), ScratchBrush(foreground),
                                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else {
            DrawGlyph(spec.glyph, icon_bounds, foreground);
        }
    };
    if (spec.has_swatch) {
        const auto center = D2D1::Point2F((icon_bounds.left + icon_bounds.right) * 0.5f,
                                         (icon_bounds.top + icon_bounds.bottom) * 0.5f);
        DrawTagDotState(center, 5.0f, spec.swatch_color, spec.checked, spec.mixed);
    } else if (spec.pictogram != MenuPictogram::None) {
        DrawPaneLayoutIcon(icon_bounds, spec.pictogram, foreground);
    } else if (spec.radio && !spec.radio_group) {
        const auto center = D2D1::Point2F((icon_bounds.left + icon_bounds.right) * 0.5f,
                                         (icon_bounds.top + icon_bounds.bottom) * 0.5f);
        dc_->FillEllipse(D2D1::Ellipse(center, Px(3.0f), Px(3.0f)), ScratchBrush(foreground));
    } else {
        draw_glyph();
    }
    content.left += icon_width + Px(8.0f);
    if (spec.has_submenu) {
        // 16px Fluent chevron in a 22px column — the old shortcut-column "›"
        // was caption-sized and looked like a punctuation mark.
        const float chevron = Px(22.0f);
        DrawGlyph(kChevronRight,
                  D2D1::RectF(content.right - chevron, content.top,
                             content.right, content.bottom),
                  foreground);
        content.right -= chevron + Px(4.0f);
    } else if (!spec.badge_text.empty()) {
        const float text_w = MeasureTextWidth(
            compositor_->DwriteFactory(), CaptionFormat(), spec.badge_text);
        const float badge_width = std::max(Px(28.0f), std::ceil(text_w) + Px(14.0f));
        // Caption is 12px; CJK line box is ~16–18px. A 18px pill plus 4px
        // inset leaves ~10px and CLIP cuts the strokes.
        const float badge_h = std::max(Px(20.0f), Height(content) - Px(3.0f));
        const float badge_y = content.top + (Height(content) - badge_h) * 0.5f;
        DrawBadge({D2D1::RectF(content.right - badge_width, badge_y,
                               content.right, badge_y + badge_h),
                   spec.badge_text, BadgeKind::Neutral});
        content.right -= badge_width + Px(8.0f);
    } else if (!spec.shortcut.empty()) {
        IDWriteTextFormat* cap = CaptionFormat();
        const float measured = MeasureTextWidth(
            compositor_->DwriteFactory(), cap, spec.shortcut) + Px(4.0f);
        const float min_label = Px(80.0f);
        const float max_shortcut = std::max(Px(24.0f), Width(content) - min_label - Px(8.0f));
        const float shortcut_width = std::clamp(measured, Px(24.0f), max_shortcut);
        auto shortcut_bounds = D2D1::RectF(content.right - shortcut_width, content.top,
                                          content.right, content.bottom);
        float label_right = shortcut_bounds.left - Px(8.0f);
        if (spec.shortcut_inline) {
            const float available = std::max(0.0f, Width(content));
            const float gap = std::min(Px(24.0f), available * 0.25f);
            const float label_width = std::clamp(spec.inline_label_width, 0.0f, available - gap);
            label_right = content.left + label_width;
            shortcut_bounds.left = label_right + gap;
        }
        DWRITE_WORD_WRAPPING old_wrap = DWRITE_WORD_WRAPPING_WRAP;
        DWRITE_TRIMMING old_trim{};
        ComPtr<IDWriteInlineObject> old_sign;
        if (cap) {
            old_wrap = cap->GetWordWrapping();
            cap->GetTrimming(&old_trim, &old_sign);
            cap->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            cap->SetTrimming(&trim, ellipsis_sign_.get());
        }
        DrawText(spec.shortcut, shortcut_bounds,
                 cap, theme_->text_secondary,
                 spec.shortcut_inline ? HorizontalAlignment::Left : HorizontalAlignment::Right);
        if (cap) {
            cap->SetTrimming(&old_trim, old_sign.get());
            cap->SetWordWrapping(old_wrap);
        }
        content.right = label_right;
    }
    // Long undo labels / Explorer verbs must ellipsize inside the fixed menu
    // width — they must never stretch the flyout.
    IDWriteTextFormat* body = BodyFormat();
    DWRITE_WORD_WRAPPING old_wrap = DWRITE_WORD_WRAPPING_WRAP;
    DWRITE_TRIMMING old_trim{};
    ComPtr<IDWriteInlineObject> old_sign;
    if (body) {
        old_wrap = body->GetWordWrapping();
        body->GetTrimming(&old_trim, &old_sign);
        body->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        body->SetTrimming(&trim, ellipsis_sign_.get());
    }
    DrawText(spec.text, content, body, foreground);
    if (body) {
        body->SetTrimming(&old_trim, old_sign.get());
        body->SetWordWrapping(old_wrap);
    }
    if (spec.separator_after) {
        const float y = spec.bounds.bottom - 0.5f;
        dc_->DrawLine(D2D1::Point2F(spec.bounds.left + Px(12.0f), y),
                      D2D1::Point2F(spec.bounds.right - Px(12.0f), y),
                      Brush(BrushId::Divider), 1.0f);
    }
}

void Painter::DrawTooltip(const D2D1_RECT_F& bounds, std::wstring_view text) {
    if (!theme_ || !compositor_) {
        return;
    }
    const D2D1_COLOR_F fill = high_contrast_ ? theme_->surface_flyout
                              : dark_ ? HexColor(0x2B2B2B) : HexColor(0xF9F9F9);
    const D2D1_COLOR_F border = high_contrast_ ? theme_->stroke_card
                                : dark_ ? HexColor(0x1C1C1C)
                                        : RgbaF(0x000000, 0.06f);
    FillRoundedRect(bounds, Px(4.0f), fill);
    StrokeRoundedRect(bounds, Px(4.0f), border);
    DrawText(text, Inset(bounds, Px(8.0f)), CaptionFormat(), theme_->text);
}

void Painter::DrawListRowBackground(const ListRowSpec& spec) {
    if (!theme_) {
        return;
    }
    auto row = spec.bounds;
    row.left += Px(4.0f);
    row.right -= Px(4.0f);
    const float radius = Px(theme_->radius_control);
    if (spec.state.selected) {
        FillRounded(row, radius, BrushId::Selected);
        FillRoundedAccent(dc_, Brush(BrushId::Accent), row, radius, Px(3.0f),
                          AccentEdge::Left);
    }
    if (spec.state.hovered) {
        FillRounded(row, radius, spec.state.pressed ? BrushId::Pressed : BrushId::Hover);
    }
    if (spec.drop_target) {
        StrokeRounded(row, radius, BrushId::Accent, std::max(2.0f, Px(2.0f)));
    }
    if (spec.state.keyboard_focus) {
        DrawFocusRing(row, radius);
    }
}

void Painter::DrawSidebarItem(const SidebarItemSpec& spec) {
    if (!theme_ || !compositor_) {
        return;
    }
    const float radius = Px(theme_->radius_flyout);
    if (spec.state.selected) {
        FillRoundedRect(spec.bounds, radius,
                        dark_ ? Rgba(0xFFFFFF, 26) : Rgba(0x000000, 20));
    } else if (spec.state.enabled && (spec.state.hovered || spec.state.pressed)) {
        FillRounded(spec.bounds, radius,
                    spec.state.pressed ? BrushId::Pressed : BrushId::Hover);
    }
    if (spec.drop_target) {
        StrokeRounded(spec.bounds, radius, BrushId::Accent, std::max(2.0f, Px(2.0f)));
    }
    const D2D1_COLOR_F foreground = spec.state.enabled ? theme_->text : theme_->text_disabled;
    const float icon_slot = Px(16.0f);
    auto content = spec.bounds;
    content.left += Px(10.0f);
    content.right -= Px(10.0f) + spec.trailing_reserve;
    content.top += Px(4.0f);
    content.bottom -= Px(4.0f);
    if (spec.status_dot) {
        const float r = Px(3.0f);
        DrawTagDot(D2D1::Point2F(content.left + r, (content.top + content.bottom) * 0.5f),
                   r, spec.status_color.a > 0 ? spec.status_color : HexColor(0x94A3B8));
        content.left += Px(10.0f);
    }
    const auto icon_rc = D2D1::RectF(content.left, content.top,
                                    content.left + icon_slot, content.bottom);
    if (spec.tag_dot) {
        DrawTagDot(D2D1::Point2F((icon_rc.left + icon_rc.right) * 0.5f,
                                (icon_rc.top + icon_rc.bottom) * 0.5f),
                   5.0f, spec.tag_color);
    } else if (!spec.skip_glyph && !spec.glyph.empty()) {
        const D2D1_COLOR_F icon_color = spec.icon_color.a > 0.0f ? spec.icon_color : foreground;
        DrawGlyph(spec.glyph, icon_rc, icon_color);
    }
    content.left += icon_slot + Px(8.0f);
    if (!spec.badge_text.empty()) {
        const float badge_width = spec.custom_badge_color
            ? MeasureTagWidth(spec.badge_text) : MeasureBadgeWidth(spec.badge_text);
        const float badge_height = spec.custom_badge_color
            ? std::min(Px(22.0f), Height(content) - Px(2.0f)) : Height(content);
        const float badge_top = content.top + (Height(content) - badge_height) * 0.5f;
        const auto badge_bounds = D2D1::RectF(content.right - badge_width, badge_top,
                                              content.right, badge_top + badge_height);
        if (spec.custom_badge_color && spec.badge_color.a > 0.0f) {
            DrawTag({badge_bounds, spec.badge_text, spec.badge_color});
        } else {
            BadgeSpec badge;
            badge.bounds = badge_bounds;
            badge.text = spec.badge_text;
            badge.kind = BadgeKind::Success;
            DrawBadge(badge);
        }
        content.right -= badge_width + Px(6.0f);
    } else if (spec.show_count || spec.badge_count > 0) {
        const auto count = std::to_wstring(std::min(spec.badge_count, 99));
        const float count_width = Px(spec.badge_count > 9 ? 22.0f : 14.0f);
        DrawText(count,
                 D2D1::RectF(content.right - count_width, content.top, content.right,
                            content.bottom),
                 CaptionFormat(), theme_->text_secondary, HorizontalAlignment::Right);
        content.right -= count_width + Px(6.0f);
    }
    if (spec.suppress_text) {
        return;
    }
    if (spec.detail.empty()) {
        DrawText(spec.text, content, NavFormat(), foreground);
    } else {
        const float detail_width = MeasureTextWidth(compositor_->DwriteFactory(),
                                                    CaptionFormat(), spec.detail) + Px(8.0f);
        DrawText(spec.text,
                 D2D1::RectF(content.left, content.top, content.right - detail_width,
                            content.bottom),
                 NavFormat(), foreground);
        DrawText(spec.detail,
                 D2D1::RectF(content.right - detail_width, content.top, content.right,
                            content.bottom),
                 CaptionFormat(), theme_->text_secondary, HorizontalAlignment::Right);
    }
}

void Painter::DrawTrayCard(const TrayCardSpec& spec) {
    if (!theme_ || !compositor_) {
        return;
    }
    const float radius = Px(theme_->radius_flyout);
    FillRounded(spec.bounds, radius, BrushId::Flyout);
    StrokeRounded(spec.bounds, radius,
                  spec.state.enabled && spec.state.hovered ? BrushId::Accent
                                                           : BrushId::StrokeCard);

    auto content = Inset(spec.bounds, Px(10.0f));
    const float icon_width = Px(18.0f);
    DrawGlyphWithBrush(spec.move_intent ? kMoveIntent : kCopyIntent,
                       D2D1::RectF(content.left, content.top,
                                  content.left + icon_width, content.bottom),
                       spec.move_intent ? BrushId::Accent : BrushId::TextSecondary);
    content.left += icon_width + Px(6.0f);

    if (spec.item_count > 0) {
        const float badge_size = Px(22.0f);
        const auto badge = D2D1::RectF(content.right - badge_size,
                                      content.top + (Height(content) - badge_size) * 0.5f,
                                      content.right,
                                      content.top + (Height(content) + badge_size) * 0.5f);
        FillRounded(badge, badge_size * 0.5f, BrushId::Selected);
        DrawTextWithBrush(std::to_wstring(std::min(spec.item_count, 99)), badge,
                          CaptionFormat(), BrushId::Text,
                          HorizontalAlignment::Center);
        content.right -= badge_size + Px(8.0f);
    }
    const float middle = (content.top + content.bottom) * 0.5f;
    DrawTextWithBrush(spec.summary,
                      D2D1::RectF(content.left, content.top, content.right,
                                 spec.detail.empty() ? content.bottom : middle + Px(2.0f)),
                      BodyFormat(), BrushId::Text);
    if (!spec.detail.empty()) {
        DrawTextWithBrush(spec.detail,
                          D2D1::RectF(content.left, middle - Px(1.0f), content.right,
                                     content.bottom),
                          CaptionFormat(), BrushId::TextSecondary);
    }
}

void Painter::DrawProgressBar(const ProgressSpec& spec) {
    if (!theme_) {
        return;
    }
    const float height = std::min(Height(spec.bounds), Px(4.0f));
    const float y = (spec.bounds.top + spec.bounds.bottom - height) * 0.5f;
    const auto track = D2D1::RectF(spec.bounds.left, y, spec.bounds.right, y + height);
    const D2D1_COLOR_F bar_color = spec.error
                                       ? (dark_ ? HexColor(0xFF99A4) : HexColor(0xC42B1C))
                                   : spec.paused
                                       ? (dark_ ? HexColor(0xFCE100) : HexColor(0x9D5D00))
                                       : theme_->accent;
    if (spec.indeterminate && !high_contrast_) {
        const float cycle_ms = motion::ProgressLongBar.delay_ms +
                               motion::ProgressLongBar.duration_ms;
        const float cycle = spec.animation_progress - std::floor(spec.animation_progress);
        const float elapsed = cycle * cycle_ms;
        const float short_pos = 1.45f * EvaluateMotion(motion::ProgressShortBar, elapsed);
        const float long_pos = 1.75f * EvaluateMotion(motion::ProgressLongBar, elapsed);
        const auto short_bar = D2D1::RectF(track.left + (short_pos - 0.4f) * Width(track),
                                          track.top,
                                          track.left + short_pos * Width(track), track.bottom);
        const auto long_bar = D2D1::RectF(track.left + (long_pos - 0.6f) * Width(track),
                                         track.top,
                                         track.left + long_pos * Width(track), track.bottom);
        dc_->PushAxisAlignedClip(track, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        FillRoundedRect(short_bar, height * 0.5f, bar_color);
        FillRoundedRect(long_bar, height * 0.5f, bar_color);
        dc_->PopAxisAlignedClip();
    } else {
        const D2D1_COLOR_F track_color = high_contrast_ ? theme_->stroke_divider
                                         : dark_ ? Rgba(0xFFFFFF, 155)
                                                 : Rgba(0x000000, 155);
        dc_->DrawLine(D2D1::Point2F(track.left, y + height * 0.5f),
                      D2D1::Point2F(track.right, y + height * 0.5f),
                      ScratchBrush(track_color), 1.0f);
        const float value = spec.indeterminate ? 0.5f : Clamp01(spec.value);
        const auto bar = D2D1::RectF(track.left, track.top,
                                    track.left + Width(track) * value,
                                    track.bottom);
        if (Width(bar) > 0.0f) {
            FillRoundedRect(bar, height * 0.5f, bar_color);
        }
    }
}

void Painter::DrawProgressRing(const ProgressSpec& spec) {
    if (!theme_ || !dc_) {
        return;
    }
    const float stroke = Px(6.0f);
    const float radius = std::max(0.0f, std::min(Width(spec.bounds), Height(spec.bounds)) * 0.5f
                                         - stroke * 0.5f);
    const auto center = D2D1::Point2F((spec.bounds.left + spec.bounds.right) * 0.5f,
                                     (spec.bounds.top + spec.bounds.bottom) * 0.5f);
    const D2D1_COLOR_F base = spec.error ? theme_->danger
                              : spec.paused ? theme_->text_secondary : theme_->accent;
    if (spec.indeterminate && !high_contrast_) {
        float progress = spec.animation_progress - std::floor(spec.animation_progress);
        float start_angle;
        float span_angle;
        if (progress < 0.5f) {
            const float t = progress * 2.0f;
            start_angle = 450.0f * t;
            span_angle = 180.0f * t;
        } else {
            const float t = (progress - 0.5f) * 2.0f;
            start_angle = 450.0f + 630.0f * t;
            span_angle = 180.0f * (1.0f - t);
        }
        DrawArc(center, radius, 180.0f - start_angle, -span_angle, stroke, base);
    } else {
        const float value = spec.indeterminate ? 0.5f : Clamp01(spec.value);
        const D2D1_COLOR_F track_color = high_contrast_ ? theme_->stroke_divider
                                         : dark_ ? RgbaF(0xFFFFFF, 0.18f)
                                                 : RgbaF(0x000000, 0.12f);
        DrawArc(center, radius, -90.0f, 360.0f, stroke, track_color);
        DrawArc(center, radius, -90.0f, 360.0f * value, stroke, base);
    }
}

void Painter::DrawTagDot(D2D1_POINT_2F center, float radius, const D2D1_COLOR_F& color) {
    if (!theme_ || !dc_) {
        return;
    }
    const float r = Px(radius);
    dc_->FillEllipse(D2D1::Ellipse(center, r, r), ScratchBrush(color));
    dc_->DrawEllipse(D2D1::Ellipse(center, r, r), Brush(BrushId::Background), 1.0f);
}

void Painter::DrawTagDotState(D2D1_POINT_2F center, float radius,
                              const D2D1_COLOR_F& color, bool checked, bool mixed,
                              bool hovered) {
    if (hovered && theme_ && dc_) {
        const D2D1_COLOR_F halo = high_contrast_ ? theme_->fill_hover
            : dark_ ? RgbaF(0xFFFFFF, 0.12f) : RgbaF(0x000000, 0.09f);
        const float halo_radius = Px(radius + 4.0f);
        dc_->FillEllipse(D2D1::Ellipse(center, halo_radius, halo_radius), ScratchBrush(halo));
    }

    const float dot_radius = radius + (hovered ? 1.0f : 0.0f);
    DrawTagDot(center, dot_radius, color);
    if ((!checked && !mixed) || !theme_ || !dc_) return;

    const D2D1_COLOR_F outline = high_contrast_ ? theme_->text
        : dark_ ? RgbaF(0xFFFFFF, 0.96f) : RgbaF(0x000000, 0.78f);
    const float outer = Px(dot_radius + 2.0f);
    if (mixed) {
        dc_->DrawEllipse(D2D1::Ellipse(center, outer, outer), ScratchBrush(outline), Px(1.0f));
        const float inner = Px((std::max)(1.0f, dot_radius - 2.0f));
        dc_->DrawEllipse(D2D1::Ellipse(center, inner, inner), ScratchBrush(outline), Px(1.0f));
    } else {
        dc_->DrawEllipse(D2D1::Ellipse(center, outer, outer), ScratchBrush(outline), Px(2.0f));
    }
}

D2D1_RECT_F Painter::ScrollbarThumbRect(const ScrollbarSpec& spec) const noexcept {
    if (!theme_ || !spec.enabled || spec.viewport_extent <= 0.0f ||
        spec.content_extent <= spec.viewport_extent || Height(spec.viewport) <= 0.0f) {
        return D2D1::RectF();
    }
    const float progress = high_contrast_ ? 1.0f : Clamp01(spec.expand_progress);
    const float width = Px(2.5f) + (Px(5.0f) - Px(2.5f)) * progress;
    const float min_thumb = Px(20.0f);
    const float thumb_height = std::min(Height(spec.viewport),
        std::max(min_thumb, Height(spec.viewport) * spec.viewport_extent / spec.content_extent));
    const float max_offset = std::max(0.0f, spec.content_extent - spec.viewport_extent);
    const float travel = std::max(0.0f, Height(spec.viewport) - thumb_height);
    const float normalized = max_offset > 0.0f ? Clamp01(spec.offset / max_offset) : 0.0f;
    const float top = spec.viewport.top + travel * normalized;
    const float right = spec.viewport.right - Px(2.5f);
    return D2D1::RectF(right - width, top, right, top + thumb_height);
}

void Painter::DrawScrollbar(const ScrollbarSpec& spec) {
    if (!theme_) {
        return;
    }
    const auto thumb = ScrollbarThumbRect(spec);
    if (Width(thumb) <= 0.0f || Height(thumb) <= 0.0f) {
        return;
    }
    const float progress = high_contrast_ ? 1.0f : Clamp01(spec.expand_progress);
    if (progress > 0.0f) {
        const auto background = D2D1::RectF(spec.viewport.right - Px(10.0f),
                                            spec.viewport.top, spec.viewport.right,
                                            spec.viewport.bottom);
        D2D1_COLOR_F background_color = high_contrast_ ? theme_->surface_flyout
                                            : dark_ ? Rgba(0x2C2C2C, 245)
                                                    : Rgba(0xFCFCFC, 217);
        background_color.a *= progress;
        FillRoundedRect(background, Px(6.0f), background_color);
    }
    const D2D1_COLOR_F foreground = high_contrast_ ? theme_->scrollbar_thumb
                                     : dark_ ? Rgba(0xFFFFFF, 139)
                                             : Rgba(0x000000, 114);
    FillRoundedRect(thumb, Width(thumb) * 0.5f, foreground);
}

void Painter::DrawBreadcrumbSegment(const BreadcrumbSegmentSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    const float radius = Px(theme_->radius_control);
    auto item = spec.bounds;
    const float accessory = (spec.has_separator || spec.has_menu) ? Px(18.0f) : 0.0f;
    item.right -= accessory;
    if (spec.state.enabled && (spec.state.hovered || spec.state.pressed)) {
        FillRounded(item, radius,
                    spec.state.pressed ? BrushId::Pressed : BrushId::Hover);
    }

    auto content = Inset(item, Px(8.0f));
    const D2D1_COLOR_F foreground = !spec.state.enabled ? theme_->text_disabled
                                      : spec.current ? theme_->text
                                                     : theme_->text_secondary;
    if (!spec.glyph.empty()) {
        const float icon_width = Px(18.0f);
        DrawGlyph(spec.glyph,
                  D2D1::RectF(content.left, content.top,
                             content.left + icon_width, content.bottom),
                  foreground);
        content.left += icon_width + Px(4.0f);
    }
    DrawText(spec.text, content, BodyFormat(), foreground);

    if (accessory > 0.0f) {
        const auto accessory_bounds = D2D1::RectF(spec.bounds.right - accessory,
                                                  spec.bounds.top, spec.bounds.right,
                                                  spec.bounds.bottom);
        DrawGlyphWithFormat(spec.has_menu ? kChevronDown : kChevronRight,
                            accessory_bounds, foreground, SmallIconFormat());
    }
    if (spec.state.keyboard_focus) {
        DrawFocusRing(item, radius);
    }
}

void Painter::DrawCommandSearchBox(const CommandSearchBoxSpec& spec) {
    if (!theme_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    TextFieldSpec field{};
    field.bounds = spec.bounds;
    field.placeholder = spec.placeholder;
    field.leading_glyph = kSearch;
    field.state = spec.state;
    field.trailing_keycap = spec.keycap;
    DrawTextField(field);
}

void Painter::DrawSplitter(const SplitterSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }

    const float cx = (spec.bounds.left + spec.bounds.right) * 0.5f;
    const float cy = (spec.bounds.top + spec.bounds.bottom) * 0.5f;
    const float line_thickness = Px(8.0f);
    const float handle_length = Px(32.0f);
    const float handle_thickness = Px(4.0f);
    const float handle_radius = Px(2.0f);

    if (spec.state.hovered || spec.state.pressed) {
        const float alpha = spec.state.pressed ? (23.0f / 255.0f) : (15.0f / 255.0f);
        const D2D1_COLOR_F track = dark_ ? RgbaF(0xFFFFFF, alpha) : RgbaF(0x000000, alpha);
        D2D1_RECT_F line;
        if (spec.vertical) {
            line = D2D1::RectF(cx - line_thickness * 0.5f, spec.bounds.top,
                               cx + line_thickness * 0.5f, spec.bounds.bottom);
        } else {
            line = D2D1::RectF(spec.bounds.left, cy - line_thickness * 0.5f,
                               spec.bounds.right, cy + line_thickness * 0.5f);
        }
        FillRoundedRect(line, 0.0f, track);
    }

    const D2D1_COLOR_F grip = high_contrast_ ? theme_->text
        : (dark_ ? HexColor(0x9F9F9F) : HexColor(0x8A8A8A));
    D2D1_RECT_F handle;
    if (spec.vertical) {
        handle = D2D1::RectF(cx - handle_thickness * 0.5f, cy - handle_length * 0.5f,
                             cx + handle_thickness * 0.5f, cy + handle_length * 0.5f);
    } else {
        handle = D2D1::RectF(cx - handle_length * 0.5f, cy - handle_thickness * 0.5f,
                             cx + handle_length * 0.5f, cy + handle_thickness * 0.5f);
    }
    FillRoundedRect(handle, handle_radius, grip);
}

void Painter::DrawSplitButton(const SplitButtonSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    const bool enabled = spec.primary_state.enabled && spec.menu_state.enabled;
    const bool pressed = spec.primary_state.pressed || spec.menu_state.pressed;
    const bool hovered = spec.primary_state.hovered || spec.menu_state.hovered;
    const float radius = Px(theme_->radius_control);
    D2D1_COLOR_F fill = !enabled ? MultiplyAlpha(theme_->accent, 0.35f)
                         : pressed ? theme_->accent_pressed
                         : hovered ? theme_->accent_hover : theme_->accent;
    if (high_contrast_ && !enabled) {
        fill = theme_->fill_input;
    }
    const D2D1_COLOR_F foreground = enabled ? theme_->accent_text : theme_->text_disabled;
    FillRoundedRect(spec.bounds, radius, fill);
    StrokeRoundedRect(spec.bounds, radius,
                      high_contrast_ ? theme_->stroke_card : MultiplyAlpha(fill, 0.86f));

    const float menu_width = Px(34.0f);
    const float divider_x = spec.bounds.right - menu_width;
    if (spec.menu_state.enabled && (spec.menu_state.hovered || spec.menu_state.pressed)) {
        const auto menu_fill = D2D1::RectF(divider_x, spec.bounds.top + Px(1.0f),
                                           spec.bounds.right - Px(1.0f),
                                           spec.bounds.bottom - Px(1.0f));
        FillRoundedRect(menu_fill, radius,
                        MultiplyAlpha(theme_->accent_text,
                                      spec.menu_state.pressed ? 0.16f : 0.10f));
    }
    dc_->DrawLine(D2D1::Point2F(divider_x, spec.bounds.top + Px(7.0f)),
                  D2D1::Point2F(divider_x, spec.bounds.bottom - Px(7.0f)),
                  ScratchBrush(MultiplyAlpha(foreground, 0.35f)), 1.0f);

    auto content = D2D1::RectF(spec.bounds.left + Px(10.0f), spec.bounds.top,
                               divider_x - Px(8.0f), spec.bounds.bottom);
    if (!spec.glyph.empty()) {
        const float icon_width = Px(18.0f);
        DrawGlyph(spec.glyph,
                  D2D1::RectF(content.left, content.top,
                             content.left + icon_width, content.bottom),
                  foreground);
        content.left += icon_width + Px(5.0f);
    }
    DrawText(spec.text, content, BodyFormat(), foreground);
    DrawGlyphWithFormat(kChevronDown,
                        D2D1::RectF(divider_x, spec.bounds.top, spec.bounds.right,
                                   spec.bounds.bottom),
                        foreground, SmallIconFormat());
    if (spec.primary_state.keyboard_focus || spec.menu_state.keyboard_focus) {
        DrawFocusRing(spec.bounds, radius);
    }
}

void Painter::DrawSegmentedItem(const SegmentedItemSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    const float radius = spec.position == SegmentPosition::Middle
                             ? 0.0f : Px(theme_->radius_control);
    D2D1_COLOR_F fill = Rgba(0x000000, 0);
    if (spec.state.selected || spec.state.checked) {
        fill = high_contrast_ ? theme_->fill_selected
                              : dark_ ? Rgba(0xFFFFFF, 22) : Rgba(0xFFFFFF, 214);
    } else if (spec.state.enabled && (spec.state.hovered || spec.state.pressed)) {
        fill = spec.state.pressed ? theme_->fill_pressed : theme_->fill_hover;
    }
    if (fill.a > 0.0f) {
        if (radius > 0.0f) {
            FillRoundedRect(spec.bounds, radius, fill);
        } else {
            dc_->FillRectangle(spec.bounds, ScratchBrush(fill));
        }
    }
    StrokeRoundedRect(spec.bounds, radius,
                      high_contrast_ ? theme_->stroke_card : theme_->stroke_divider);

    const D2D1_COLOR_F foreground = spec.state.enabled ? theme_->text
                                                        : theme_->text_disabled;
    auto content = Inset(spec.bounds, Px(6.0f));
    if (!spec.glyph.empty()) {
        const float icon_width = spec.text.empty() ? Width(content) : Px(18.0f);
        DrawGlyph(spec.glyph,
                  D2D1::RectF(content.left, content.top,
                             content.left + icon_width, content.bottom),
                  foreground);
        content.left += spec.text.empty() ? 0.0f : icon_width + Px(4.0f);
    }
    if (!spec.text.empty()) {
        DrawText(spec.text, content, BodyFormat(), foreground,
                 spec.glyph.empty() ? HorizontalAlignment::Center
                                    : HorizontalAlignment::Left);
    }
    if (spec.state.keyboard_focus) {
        DrawFocusRing(spec.bounds, radius);
    }
}

void Painter::DrawBadge(const BadgeSpec& spec) {
    if (!theme_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    D2D1_COLOR_F background;
    D2D1_COLOR_F foreground;
    D2D1_COLOR_F border = Rgba(0x000000, 0);
    if (spec.use_custom_colors) {
        background = spec.custom_background;
        foreground = spec.custom_foreground;
    } else if (high_contrast_) {
        background = spec.kind == BadgeKind::Neutral || spec.kind == BadgeKind::Keycap
                         ? theme_->fill_input : theme_->accent;
        foreground = spec.kind == BadgeKind::Neutral || spec.kind == BadgeKind::Keycap
                         ? theme_->text : theme_->accent_text;
        border = theme_->stroke_card;
    } else {
        switch (spec.kind) {
        case BadgeKind::Accent:
            background = MultiplyAlpha(theme_->accent, dark_ ? 0.24f : 0.16f);
            foreground = dark_ ? HexColor(0x69B7FF) : HexColor(0x005A9E);
            break;
        case BadgeKind::Success:
            background = dark_ ? HexColor(0x123D31) : HexColor(0xDFF6E9);
            foreground = dark_ ? HexColor(0x45D7A4) : HexColor(0x107C5A);
            break;
        case BadgeKind::Warning:
            background = dark_ ? HexColor(0x443616) : HexColor(0xFFF4CE);
            foreground = dark_ ? HexColor(0xF7D154) : HexColor(0x8A5500);
            break;
        case BadgeKind::Danger:
            background = dark_ ? HexColor(0x4A1F24) : HexColor(0xFDE7E9);
            foreground = dark_ ? HexColor(0xFF99A4) : HexColor(0xC42B1C);
            break;
        case BadgeKind::Keycap:
            background = dark_ ? Rgba(0x000000, 80) : Rgba(0xFFFFFF, 190);
            foreground = theme_->text_secondary;
            border = theme_->stroke_card;
            break;
        case BadgeKind::Neutral:
        default:
            background = dark_ ? Rgba(0xFFFFFF, 18) : Rgba(0x000000, 14);
            foreground = theme_->text_secondary;
            break;
        }
    }
    if (spec.dot) {
        const auto center = D2D1::Point2F((spec.bounds.left + spec.bounds.right) * 0.5f,
                                         (spec.bounds.top + spec.bounds.bottom) * 0.5f);
        const float radius = std::min(Width(spec.bounds), Height(spec.bounds)) * 0.5f;
        dc_->FillEllipse(D2D1::Ellipse(center, radius, radius), ScratchBrush(background));
        if (border.a > 0.0f) {
            dc_->DrawEllipse(D2D1::Ellipse(center, std::max(0.0f, radius - 0.5f),
                                           std::max(0.0f, radius - 0.5f)),
                             ScratchBrush(border), 1.0f, round_stroke_.get());
        }
        return;
    }
    const float radius = Height(spec.bounds) * 0.5f;
    FillRoundedRect(spec.bounds, radius, background);
    if (border.a > 0.0f) {
        StrokeRoundedRect(spec.bounds, radius, border);
    }
    // Horizontal pad only. Vertical inset > ~1.5px clips CJK caption ink
    // (12px font, ~16–18px line box) because DrawText uses CLIP.
    const float pad_x = Px(7.0f);
    const float pad_y = Px(1.5f);
    DrawText(spec.text,
             D2D1::RectF(spec.bounds.left + pad_x, spec.bounds.top + pad_y,
                         spec.bounds.right - pad_x, spec.bounds.bottom - pad_y),
             CaptionFormat(), foreground, HorizontalAlignment::Center,
             BlendOver(background, theme_->bg));
}

void Painter::InvalidateTypography() noexcept {
    ClearTextLayoutCache();
    body_format_.reset();
    nav_format_.reset();
    section_format_.reset();
    caption_format_.reset();
    small_icon_format_.reset();
    micro_icon_format_.reset();
    ellipsis_sign_.reset();
    format_scale_ = 0.0f;
}

void Painter::DrawTag(const TagSpec& spec) {
    if (!theme_ || !compositor_ || spec.text.empty() ||
        Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    const D2D1_COLOR_F color = spec.color.a > 0.0f ? spec.color : theme_->accent;
    const AccentShades shades = DeriveAccentShades(color);
    D2D1_COLOR_F background;
    D2D1_COLOR_F foreground;
    if (high_contrast_) {
        background = theme_->fill_input;
        foreground = theme_->text;
    } else if (dark_) {
        background = WithAlpha(color, 0.24f);
        foreground = shades.light2;
    } else {
        background = WithAlpha(color, 0.13f);
        foreground = shades.dark2;
    }
    const float radius = std::min(Px(4.0f), Height(spec.bounds) * 0.25f);
    FillRoundedRect(spec.bounds, radius, background);
    if (spec.bordered && !high_contrast_) {
        StrokeRoundedRect(spec.bounds, radius, WithAlpha(color, dark_ ? 0.38f : 0.30f));
    }
    const float pad_x = Px(7.0f);
    const float pad_y = Px(1.5f);
    DrawText(spec.text,
             D2D1::RectF(spec.bounds.left + pad_x, spec.bounds.top + pad_y,
                         spec.bounds.right - pad_x, spec.bounds.bottom - pad_y),
             CaptionFormat(), foreground, HorizontalAlignment::Center,
             BlendOver(background, theme_->bg));
}

float Painter::MeasureButtonWidth(std::wstring_view text, std::wstring_view glyph,
                                  bool drop_down) const {
    float width = Px(8.0f) * 2.0f;
    if (!glyph.empty()) width += Px(20.0f) + Px(4.0f);
    if (drop_down) width += Px(16.0f) + Px(4.0f);
    width += typography::MeasureLine(compositor_, BodyFormat(), text);
    return std::max(Px(32.0f), std::ceil(width));
}

float Painter::MeasureButtonHeight() const {
    float text_h = BodyFormat() ? BodyFormat()->GetFontSize() * 1.5f : Px(21.0f);
    float luma_w = 0.0f;
    float luma_h = 0.0f;
    if (compositor_ && BodyFormat() &&
        compositor_->MeasureLumaText(L"Ag\x6d4b", BodyFormat(), luma_w, &luma_h) &&
        luma_h > 0.0f) {
        text_h = std::max(text_h, luma_h);
    }
    return std::max(Px(32.0f), std::ceil(text_h + Px(8.0f)));
}

D2D1_RECT_F Painter::FitButtonBounds(D2D1_RECT_F bounds, std::wstring_view text,
                                     std::wstring_view glyph, bool drop_down) const {
    const float width = MeasureButtonWidth(text, glyph, drop_down);
    const float height = MeasureButtonHeight();
    if (Width(bounds) < width) bounds.right = bounds.left + width;
    if (Height(bounds) < height) {
        const float cy = (bounds.top + bounds.bottom) * 0.5f;
        bounds.top = cy - height * 0.5f;
        bounds.bottom = cy + height * 0.5f;
    }
    return bounds;
}

float Painter::MeasureTagWidth(std::wstring_view text) const {
    if (text.empty()) return 0.0f;
    const float text_w = MeasureTextWidth(compositor_, CaptionFormat(), text);
    const float fallback = Px(12.0f) * static_cast<float>(text.size());
    return std::max(Px(28.0f), std::ceil((text_w > 0.0f ? text_w : fallback) + Px(14.0f)));
}

float Painter::MeasureBadgeWidth(std::wstring_view text) const {
    if (text.empty()) return 0.0f;
    const float text_w = MeasureTextWidth(compositor_, CaptionFormat(), text);
    const float fallback = Px(12.0f) * static_cast<float>(text.size());
    return std::max(Px(28.0f), std::ceil((text_w > 0.0f ? text_w : fallback) + Px(14.0f)));
}

float Painter::OmnibarHintReservePx() const {
    const float label = MeasureTextWidth(compositor_, CaptionFormat(), kOmnibarHintBadge);
    const float label_w = label > 0.0f ? label : Px(24.0f);
    return Px(8.0f) + Px(8.0f) + Px(16.0f) + Px(6.0f) + label_w + Px(6.0f)
         + MeasureBadgeWidth(kOmnibarHintKey) + Px(6.0f);
}

D2D1_RECT_F Painter::DrawOmnibarHints(const D2D1_RECT_F& field, bool skip_search_glyph) {
    if (!theme_ || Width(field) <= 0.0f || Height(field) <= 0.0f) return {};
    const float reserve = OmnibarHintReservePx();
    if (Width(field) < reserve + Px(48.0f)) return {};

    const float chip_left = field.right - reserve;
    const float chip_right = field.right - Px(8.0f);
    const float chip_top = field.top + Px(3.0f);
    const float chip_bottom = field.bottom - Px(3.0f);
    const D2D1_RECT_F chip = D2D1::RectF(chip_left, chip_top, chip_right, chip_bottom);
    const D2D1_COLOR_F chip_fill = high_contrast_ ? theme_->fill_input
        : dark_ ? Rgba(0xFFFFFF, 18) : Rgba(0x000000, 14);
    FillRoundedRect(chip, Height(chip) * 0.5f, chip_fill);

    float x = chip_left + Px(8.0f);
    const float icon = Px(16.0f);
    const float icon_y = chip_top + (Height(chip) - icon) * 0.5f;
    const D2D1_RECT_F icon_rc = D2D1::RectF(x, icon_y, x + icon, icon_y + icon);
    if (!skip_search_glyph) {
        DrawGlyph(kSearch, icon_rc, theme_->text_secondary);
    }
    x += icon + Px(6.0f);

    const float label = MeasureTextWidth(compositor_, CaptionFormat(), kOmnibarHintBadge);
    const float label_w = label > 0.0f ? label : Px(24.0f);
    DrawText(kOmnibarHintBadge,
             D2D1::RectF(x, chip_top, x + label_w, chip_bottom),
             CaptionFormat(), theme_->text_secondary, HorizontalAlignment::Left,
             BlendOver(chip_fill, theme_->bg));
    x += label_w + Px(6.0f);

    const float key_w = MeasureBadgeWidth(kOmnibarHintKey);
    DrawBadge({D2D1::RectF(x, chip_top + Px(1.0f), x + key_w, chip_bottom - Px(1.0f)),
               kOmnibarHintKey, BadgeKind::Keycap});
    return icon_rc;
}

D2D1_RECT_F Painter::ButtonGlyphRect(const D2D1_RECT_F& bounds, bool icon_only) const {
    const float icon = Px(20.0f);
    if (icon_only) {
        const float cx = (bounds.left + bounds.right) * 0.5f;
        const float cy = (bounds.top + bounds.bottom) * 0.5f;
        return D2D1::RectF(cx - icon * 0.5f, cy - icon * 0.5f,
                           cx + icon * 0.5f, cy + icon * 0.5f);
    }
    const float pad_x = Px(8.0f);
    const float pad_y = Px(4.0f);
    return D2D1::RectF(bounds.left + pad_x, bounds.top + pad_y,
                       bounds.left + pad_x + icon, bounds.bottom - pad_y);
}

D2D1_RECT_F Painter::SidebarItemIconRect(const D2D1_RECT_F& bounds, bool status_dot) const {
    float left = bounds.left + Px(10.0f);
    if (status_dot) left += Px(10.0f);
    const float slot = Px(16.0f);
    return D2D1::RectF(left, bounds.top + Px(4.0f), left + slot, bounds.bottom - Px(4.0f));
}

D2D1_RECT_F Painter::DriveSidebarItemIconRect(const D2D1_RECT_F& bounds) const {
    const float left = bounds.left + Px(10.0f);
    const float top = bounds.top + Px(6.0f);
    return D2D1::RectF(left, top, left + Px(16.0f), top + Px(18.0f));
}

void Painter::DrawSidebarSectionHeader(const SidebarSectionHeaderSpec& spec) {
    if (!theme_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    if (spec.state.enabled && (spec.state.hovered || spec.state.pressed)) {
        FillRounded(spec.bounds, Px(theme_->radius_control),
                    spec.state.pressed ? BrushId::Pressed : BrushId::Hover);
    }
    const D2D1_COLOR_F foreground = spec.state.enabled ? theme_->text_secondary
                                                        : theme_->text_disabled;
    // Disclosure chevron sits at the trailing edge and mirrors collapsed state.
    const float chevron_slot = Px(14.0f);
    DrawText(spec.text,
             D2D1::RectF(spec.bounds.left + Px(10.0f), spec.bounds.top,
                        spec.bounds.right - chevron_slot - Px(8.0f), spec.bounds.bottom),
             SectionFormat(), foreground);
    DrawGlyphWithFormat(spec.expanded ? kChevronDown : kChevronRight,
                        D2D1::RectF(spec.bounds.right - chevron_slot - Px(8.0f), spec.bounds.top,
                                    spec.bounds.right - Px(8.0f), spec.bounds.bottom),
                        foreground, SmallIconFormat());
}

void Painter::DrawDriveSidebarItem(const DriveSidebarItemSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    const float radius = Px(theme_->radius_flyout);
    if (spec.state.selected) {
        FillRoundedRect(spec.bounds, radius,
                        dark_ ? Rgba(0xFFFFFF, 18) : Rgba(0x000000, 14));
    } else if (spec.state.enabled && (spec.state.hovered || spec.state.pressed)) {
        FillRounded(spec.bounds, radius,
                    spec.state.pressed ? BrushId::Pressed : BrushId::Hover);
    }
    if (spec.drop_target) {
        StrokeRounded(spec.bounds, radius, BrushId::Accent, std::max(2.0f, Px(2.0f)));
    }
    const D2D1_COLOR_F foreground = spec.state.enabled ? theme_->text
                                                        : theme_->text_disabled;
    auto content = spec.bounds;
    content.left += Px(10.0f);
    content.right -= Px(10.0f);
    content.top += Px(6.0f);
    content.bottom -= Px(8.0f);
    const float icon_slot = Px(16.0f);
    const float text_row = Px(18.0f);
    const D2D1_COLOR_F icon_color = spec.icon_color.a > 0.0f ? spec.icon_color : foreground;
    if (!spec.skip_glyph) {
        DrawGlyph(spec.glyph,
                  D2D1::RectF(content.left, content.top,
                             content.left + icon_slot, content.top + text_row),
                  icon_color);
    }
    const float text_left = content.left + icon_slot + Px(8.0f);
    const float detail_width = MeasureTextWidth(compositor_->DwriteFactory(),
                                                CaptionFormat(), spec.detail) + Px(6.0f);
    DrawText(spec.name,
             D2D1::RectF(text_left, content.top, content.right - detail_width,
                        content.top + text_row),
             NavFormat(), foreground);
    DrawText(spec.detail,
             D2D1::RectF(content.right - detail_width, content.top, content.right,
                        content.top + text_row),
             CaptionFormat(), theme_->text_secondary, HorizontalAlignment::Right);

    const auto track = D2D1::RectF(text_left, content.bottom - Px(4.0f), content.right,
                                   content.bottom);
    FillRoundedRect(track, Px(2.0f),
                    dark_ ? Rgba(0xFFFFFF, 20) : Rgba(0x000000, 18));
    const float value = Clamp01(spec.capacity);
    if (value > 0.0f) {
        auto value_rect = track;
        value_rect.right = track.left + Width(track) * value;
        D2D1_COLOR_F value_color = spec.bar_color.a > 0.0f ? spec.bar_color
                                 : spec.icon_color.a > 0.0f ? spec.icon_color
                                                            : theme_->accent;
        if (value >= 0.90f) {
            value_color = theme_->danger;
        }
        FillRoundedRect(value_rect, Px(2.0f), value_color);
    }
    if (spec.state.keyboard_focus) {
        DrawFocusRing(spec.bounds, radius);
    }
}

void Painter::DrawPaneHeader(const PaneHeaderSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    const float sort_width = Px(34.0f);
    const float filter_width = std::min(Px(260.0f), Width(spec.bounds) * 0.38f);
    const auto sort_bounds = D2D1::RectF(spec.bounds.right - sort_width, spec.bounds.top,
                                         spec.bounds.right, spec.bounds.bottom);
    const auto filter_bounds = D2D1::RectF(sort_bounds.left - filter_width - Px(8.0f),
                                           spec.bounds.top + Px(5.0f),
                                           sort_bounds.left - Px(8.0f),
                                           spec.bounds.bottom - Px(5.0f));
    const auto title_bounds = D2D1::RectF(spec.bounds.left + Px(12.0f), spec.bounds.top,
                                          filter_bounds.left - Px(12.0f),
                                          spec.bounds.bottom);
    const float title_width = std::min(Px(150.0f), Width(title_bounds) * 0.45f);
    DrawText(spec.title,
             D2D1::RectF(title_bounds.left, title_bounds.top,
                        title_bounds.left + title_width, title_bounds.bottom),
             BodyFormat(), theme_->text);
    DrawText(spec.summary,
             D2D1::RectF(title_bounds.left + title_width, title_bounds.top,
                        title_bounds.right, title_bounds.bottom),
             CaptionFormat(), theme_->text_secondary);

    TextFieldSpec filter{};
    filter.bounds = filter_bounds;
    filter.text = spec.filter_text;
    filter.placeholder = L"Filter";
    filter.leading_glyph = kSearch;
    filter.state = spec.filter_state;
    DrawTextField(filter);

    ButtonSpec sort{};
    sort.bounds = Inset(sort_bounds, Px(3.0f));
    sort.glyph = kSort;
    sort.kind = ButtonKind::Transparent;
    sort.state = spec.sort_state;
    sort.icon_only = true;
    DrawButton(sort);
}

void Painter::DrawColumnHeader(const ColumnHeaderSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    if (spec.state.enabled && (spec.state.hovered || spec.state.pressed)) {
        FillRounded(Inset(spec.bounds, Px(2.0f)), Px(theme_->radius_control),
                    spec.state.pressed ? BrushId::Pressed : BrushId::Hover);
    }
    auto content = Inset(spec.bounds, Px(10.0f));
    const D2D1_COLOR_F foreground = spec.state.enabled ? theme_->text_secondary
                                                        : theme_->text_disabled;
    if (spec.sort != SortDirection::None) {
        const float icon_width = Px(16.0f);
        DrawGlyphWithFormat(spec.sort == SortDirection::Ascending ? kChevronUp : kChevronDown,
                            D2D1::RectF(content.right - icon_width, content.top,
                                       content.right, content.bottom),
                            theme_->accent, SmallIconFormat());
        content.right -= icon_width + Px(2.0f);
    }
    DrawText(spec.text, content, CaptionFormat(), foreground, spec.alignment);
    dc_->DrawLine(D2D1::Point2F(spec.bounds.right - 0.5f, spec.bounds.top + Px(7.0f)),
                  D2D1::Point2F(spec.bounds.right - 0.5f, spec.bounds.bottom - Px(7.0f)),
                  Brush(BrushId::Divider), 1.0f);
}

void Painter::DrawFileRowContent(const FileRowContentSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    ListRowSpec row{};
    row.bounds = spec.bounds;
    row.state = spec.state;
    row.cut = spec.cut;
    row.drop_target = spec.drop_target;
    DrawListRowBackground(row);

    const float opacity = ListRowContentOpacity(row);
    const D2D1_COLOR_F foreground = MultiplyAlpha(
        spec.state.enabled ? theme_->text : theme_->text_disabled, opacity);
    const D2D1_COLOR_F secondary = MultiplyAlpha(theme_->text_secondary, opacity);
    auto content = spec.bounds;
    content.left += Px(12.0f);
    content.right -= Px(12.0f);
    const float total = Width(content);
    const float name_right = content.left + total * 0.48f;
    const float modified_right = content.left + total * 0.69f;
    const float detail_right = content.left + total * 0.87f;
    const float icon_width = Px(20.0f);
    DrawGlyph(spec.glyph,
              D2D1::RectF(content.left, content.top,
                         content.left + icon_width, content.bottom),
              foreground);
    DrawText(spec.name,
             D2D1::RectF(content.left + icon_width + Px(8.0f), content.top,
                        name_right - Px(42.0f), content.bottom),
             BodyFormat(), foreground);
    const size_t tag_count = std::min<size_t>(spec.tag_count, spec.tag_colors.size());
    for (size_t i = 0; i < tag_count; ++i) {
        const float x = name_right - Px(10.0f + static_cast<float>(tag_count - 1 - i) * 14.0f);
        DrawTagDot(D2D1::Point2F(x, (content.top + content.bottom) * 0.5f),
                   4.0f, spec.tag_colors[i]);
    }
    DrawText(spec.modified,
             D2D1::RectF(name_right + Px(8.0f), content.top,
                        modified_right - Px(8.0f), content.bottom),
             CaptionFormat(), secondary);
    if (!spec.status.empty()) {
        const auto badge_bounds = D2D1::RectF(modified_right + Px(8.0f),
                                              content.top + Px(5.0f),
                                              detail_right - Px(8.0f),
                                              content.bottom - Px(5.0f));
        DrawBadge({badge_bounds, spec.status, spec.status_kind});
    } else {
        DrawText(spec.type,
                 D2D1::RectF(modified_right + Px(8.0f), content.top,
                            detail_right - Px(8.0f), content.bottom),
                 CaptionFormat(), secondary);
    }
    DrawText(spec.size,
             D2D1::RectF(detail_right + Px(8.0f), content.top,
                        content.right, content.bottom),
             CaptionFormat(), secondary, HorizontalAlignment::Right);
}

void Painter::DrawStagingTrayPanel(const StagingTrayPanelSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    const float radius = Px(theme_->radius_flyout);
    const D2D1_COLOR_F fill = high_contrast_ ? theme_->surface_flyout
                              : dark_ ? Rgba(0x0D1422, 220) : Rgba(0xF3F7FD, 245);
    const D2D1_COLOR_F border = spec.state.hovered
                                    ? theme_->accent
                                    : (dark_ ? RgbaF(0x3B82F6, 0.30f) : RgbaF(0x3B82F6, 0.35f));
    FillRoundedRect(spec.bounds, radius, fill);
    StrokeRoundedRect(spec.bounds, radius, border);
    const auto header = D2D1::RectF(spec.bounds.left + Px(10.0f), spec.bounds.top + Px(6.0f),
                                    spec.bounds.right - Px(10.0f),
                                    spec.bounds.top + Px(28.0f));
    std::wstring count_text;
    if (!spec.count_label.empty()) {
        count_text.assign(spec.count_label.begin(), spec.count_label.end());
    } else if (spec.item_count > 0) {
        count_text = std::to_wstring(std::min(spec.item_count, 99));
    }
    float right = header.right;
    if (!spec.action_text.empty()) {
        // Fill-only pill, same language as toolbar 新建 — no stroke.
        const float action_width = Px(72.0f);
        const D2D1_RECT_F action_rc =
            D2D1::RectF(right - action_width, header.top + Px(1.5f), right,
                        header.bottom - Px(1.5f));
        FillRoundedRect(action_rc, Height(action_rc) * 0.5f,
                        spec.action_hovered ? theme_->accent_hover : theme_->accent);
        DrawText(spec.action_text, action_rc, CaptionFormat(), theme_->accent_text,
                 HorizontalAlignment::Center);
        right -= action_width + Px(6.0f);
    }
    if (!count_text.empty()) {
        // Size the badge to the text instead of fixed widths (CJK labels
        // like "4 项" used to clip). Sits left of the action button.
        const float text_w = MeasureTextWidth(
            compositor_ ? compositor_->DwriteFactory() : nullptr, CaptionFormat(), count_text);
        const float badge_width = std::max(Px(28.0f), std::ceil(text_w) + Px(14.0f));
        // Keep the badge tall enough for the caption line height — shaving
        // more than ~1.5px top/bottom clips CJK ink.
        DrawBadge({D2D1::RectF(right - badge_width, header.top + Px(1.5f), right,
                               header.bottom - Px(1.5f)),
                   count_text, BadgeKind::Accent});
        right -= badge_width + Px(6.0f);
    }
    DrawText(spec.title,
             D2D1::RectF(header.left, header.top, right, header.bottom),
             BodyFormat(), theme_->accent);
    if (spec.expanded && !spec.helper.empty()) {
        const float helper_left = spec.bounds.left + Px(10.0f);
        const float helper_right = spec.bounds.right - Px(10.0f);
        const float helper_width = std::max(0.0f, helper_right - helper_left);
        const float line_height = Px(18.0f);
        std::vector<std::wstring> lines;
        std::wstring line;
        const auto flush_line = [&]() {
            if (!line.empty()) {
                lines.push_back(std::move(line));
                line.clear();
            }
        };
        size_t cursor = 0;
        while (cursor < spec.helper.size()) {
            while (cursor < spec.helper.size() && spec.helper[cursor] == L' ') ++cursor;
            if (cursor >= spec.helper.size()) break;
            const size_t word_start = cursor;
            while (cursor < spec.helper.size() && spec.helper[cursor] != L' ') ++cursor;
            const std::wstring_view word = spec.helper.substr(word_start, cursor - word_start);
            const std::wstring candidate = line.empty()
                ? std::wstring(word) : line + L" " + std::wstring(word);
            if (!line.empty() && MeasureTextWidth(
                    compositor_ ? compositor_->DwriteFactory() : nullptr,
                    CaptionFormat(), candidate) > helper_width) {
                flush_line();
                line.assign(word);
            } else {
                line = candidate;
            }
        }
        flush_line();
        const size_t max_lines = static_cast<size_t>(std::max(
            1.0f, std::floor((Height(spec.bounds) - (header.bottom - spec.bounds.top) - Px(8.0f)) /
                              line_height)));
        if (lines.size() > max_lines) lines.resize(max_lines);
        for (size_t i = 0; i < lines.size(); ++i) {
            DrawText(lines[i],
                     D2D1::RectF(helper_left, header.bottom + static_cast<float>(i) * line_height,
                                 helper_right, header.bottom + static_cast<float>(i + 1) * line_height),
                     CaptionFormat(), theme_->text_secondary);
        }
    }
}

void Painter::DrawStagingItem(const StagingItemSpec& spec) {
    if (!theme_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    D2D1_COLOR_F fill = dark_ ? Rgba(0x000000, 76) : Rgba(0xFFFFFF, 190);
    if (spec.state.enabled && (spec.state.hovered || spec.state.pressed)) {
        fill = spec.state.pressed ? theme_->fill_pressed : theme_->fill_hover;
    }
    FillRoundedRect(spec.bounds, Px(theme_->radius_control), fill);
    const D2D1_COLOR_F foreground = !spec.state.enabled || spec.missing
                                        ? theme_->text_disabled : theme_->text;
    DrawText(spec.name,
             D2D1::RectF(spec.bounds.left + Px(7.0f), spec.bounds.top,
                        spec.bounds.right - Px(28.0f), spec.bounds.bottom),
             BodyFormat(), foreground);
    DrawGlyphWithFormat(kClose,
                        D2D1::RectF(spec.bounds.right - Px(26.0f), spec.bounds.top,
                                   spec.bounds.right - Px(4.0f), spec.bounds.bottom),
                        foreground, MicroIconFormat());
}

void Painter::DrawStatusBar(const StatusBarSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    dc_->FillRectangle(spec.bounds, ScratchBrush(theme_->status_bg));
    dc_->DrawLine(D2D1::Point2F(spec.bounds.left, spec.bounds.top + 0.5f),
                  D2D1::Point2F(spec.bounds.right, spec.bounds.top + 0.5f),
                  Brush(BrushId::Divider), 1.0f);
    const float third = Width(spec.bounds) / 3.0f;
    float left = spec.bounds.left + Px(10.0f);
    if (spec.ready) {
        const auto center = D2D1::Point2F(left + Px(5.0f),
                                         (spec.bounds.top + spec.bounds.bottom) * 0.5f);
        dc_->FillEllipse(D2D1::Ellipse(center, Px(4.0f), Px(4.0f)),
                         ScratchBrush(dark_ ? HexColor(0x45D7A4) : HexColor(0x107C5A)));
        left += Px(14.0f);
    }
    DrawText(spec.left_text,
             D2D1::RectF(left, spec.bounds.top,
                        spec.bounds.left + third, spec.bounds.bottom),
             CaptionFormat(), theme_->text_secondary);
    DrawText(spec.center_text,
             D2D1::RectF(spec.bounds.left + third, spec.bounds.top,
                        spec.bounds.left + third * 2.0f, spec.bounds.bottom),
             CaptionFormat(), theme_->text_secondary, HorizontalAlignment::Center);
    DrawText(spec.right_text,
             D2D1::RectF(spec.bounds.left + third * 2.0f, spec.bounds.top,
                        spec.bounds.right - Px(10.0f), spec.bounds.bottom),
             CaptionFormat(), theme_->text_secondary, HorizontalAlignment::Right);
}

void Painter::DrawInfoBar(const InfoBarSpec& spec) {
    if (!theme_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }
    D2D1_COLOR_F accent;
    std::wstring_view fallback_glyph;
    switch (spec.kind) {
    case InfoBarKind::Success:
        accent = dark_ ? HexColor(0x45D7A4) : HexColor(0x107C5A);
        fallback_glyph = kSuccess;
        break;
    case InfoBarKind::Warning:
        accent = dark_ ? HexColor(0xF7D154) : HexColor(0x8A5500);
        fallback_glyph = kWarning;
        break;
    case InfoBarKind::Error:
        accent = theme_->danger;
        fallback_glyph = kError;
        break;
    case InfoBarKind::Informational:
    default:
        accent = theme_->accent;
        fallback_glyph = kInfo;
        break;
    }
    const float radius = Px(8.0f);
    if (!high_contrast_) {
        auto shadow = spec.bounds;
        shadow.top += Px(2.0f);
        shadow.bottom += Px(2.0f);
        FillRoundedRect(shadow, radius, Rgba(0x000000, dark_ ? 28 : 12));
    }
    FillRoundedRect(spec.bounds, radius, theme_->surface_flyout);
    StrokeRoundedRect(spec.bounds, radius,
                      high_contrast_ ? theme_->stroke_card : theme_->stroke_card);
    FillRoundedAccent(dc_, ScratchBrush(accent), spec.bounds, radius, Px(3.0f), AccentEdge::Left);
    const bool stacked = Height(spec.bounds) >= Px(56.0f) && !spec.message.empty();
    const float icon_top = stacked ? spec.bounds.top + Px(12.0f)
        : (spec.bounds.top + spec.bounds.bottom - Px(28.0f)) * 0.5f;
    const auto glyph_bounds = D2D1::RectF(spec.bounds.left + Px(14.0f), icon_top,
                                          spec.bounds.left + Px(42.0f), icon_top + Px(28.0f));
    auto icon_fill = accent;
    icon_fill.a = dark_ ? 0.18f : 0.10f;
    FillRoundedRect(glyph_bounds, Px(14.0f), icon_fill);
    DrawGlyph(spec.glyph.empty() ? fallback_glyph : spec.glyph, glyph_bounds, accent);
    const float close_space = spec.show_close ? Px(32.0f) : Px(8.0f);
    const auto text_bounds = D2D1::RectF(glyph_bounds.right + Px(10.0f), spec.bounds.top,
                                         spec.bounds.right - close_space, spec.bounds.bottom);
    if (stacked) {
        DrawText(spec.title, D2D1::RectF(text_bounds.left, text_bounds.top + Px(10.0f),
                 text_bounds.right, text_bounds.top + Px(30.0f)), BodyFormat(), theme_->text);
        ComPtr<IDWriteTextLayout> layout;
        if (compositor_ && compositor_->DwriteFactory() && SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(
                spec.message.data(), static_cast<UINT32>(spec.message.size()), CaptionFormat(),
                Width(text_bounds), Height(text_bounds) - Px(36.0f), &layout))) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            dc_->DrawTextLayout(D2D1::Point2F(text_bounds.left, text_bounds.top + Px(32.0f)),
                                layout.get(), ScratchBrush(theme_->text_secondary), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    } else {
    const float title_width = std::min(Px(150.0f), Width(text_bounds) * 0.36f);
    DrawText(spec.title,
             D2D1::RectF(text_bounds.left, text_bounds.top,
                        text_bounds.left + title_width, text_bounds.bottom),
             BodyFormat(), theme_->text);
    DrawText(spec.message,
             D2D1::RectF(text_bounds.left + title_width, text_bounds.top,
                        text_bounds.right, text_bounds.bottom),
             CaptionFormat(), theme_->text_secondary);
    }
    if (spec.show_close) {
        if (spec.state.hovered) {
            const float center_y = (spec.bounds.top + spec.bounds.bottom) * 0.5f;
            FillRoundedRect(D2D1::RectF(spec.bounds.right - Px(30.0f), center_y - Px(14.0f),
                spec.bounds.right - Px(2.0f), center_y + Px(14.0f)), Px(4.0f), theme_->fill_hover);
        }
        DrawGlyphWithFormat(kClose,
                            D2D1::RectF(spec.bounds.right - Px(28.0f), spec.bounds.top,
                                       spec.bounds.right - Px(4.0f), spec.bounds.bottom),
                            theme_->text_secondary, MicroIconFormat());
    }
}

void Painter::DrawEmptyState(const EmptyStateSpec& spec) {
    if (!theme_ || !dc_ || Width(spec.bounds) <= 0.0f || Height(spec.bounds) <= 0.0f) {
        return;
    }

    const float icon_size = Px(52.0f);
    const float title_height = Px(24.0f);
    const float message_height = spec.message.empty() ? 0.0f : Px(20.0f);
    const float gap = Px(10.0f);
    const float content_height = icon_size + gap + title_height + message_height;
    const float center_x = (spec.bounds.left + spec.bounds.right) * 0.5f;
    const float top = (spec.bounds.top + spec.bounds.bottom - content_height) * 0.5f;
    const D2D1_RECT_F icon_bounds = D2D1::RectF(
        center_x - icon_size * 0.5f, top,
        center_x + icon_size * 0.5f, top + icon_size);

    FillRoundedRect(icon_bounds, icon_size * 0.5f,
                    high_contrast_ ? theme_->surface_flyout : theme_->fill_hover);
    if (high_contrast_)
        StrokeRoundedRect(icon_bounds, icon_size * 0.5f, theme_->stroke_card);
    DrawGlyph(spec.glyph, icon_bounds, theme_->text_secondary);

    const float text_width = std::min(Width(spec.bounds) - Px(24.0f), Px(240.0f));
    const D2D1_RECT_F title_bounds = D2D1::RectF(
        center_x - text_width * 0.5f, icon_bounds.bottom + gap,
        center_x + text_width * 0.5f, icon_bounds.bottom + gap + title_height);
    DrawText(spec.title, title_bounds, BodyFormat(), theme_->text_secondary,
             HorizontalAlignment::Center);
    if (!spec.message.empty()) {
        DrawText(spec.message,
                 D2D1::RectF(title_bounds.left, title_bounds.bottom,
                            title_bounds.right, title_bounds.bottom + message_height),
                 CaptionFormat(), theme_->text_disabled, HorizontalAlignment::Center);
    }
}

bool Painter::Contains(const D2D1_RECT_F& bounds, float x, float y) noexcept {
    return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
}

float Painter::ListRowContentOpacity(const ListRowSpec& spec) noexcept {
    return spec.cut ? 0.55f : 1.0f;
}

} // namespace pulse::ui::fluent
