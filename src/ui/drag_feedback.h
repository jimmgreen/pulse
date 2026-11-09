#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string_view>
#include <cmath>
#include "command_icons.h"

namespace pulse::ui {

// drag_undo_mockup.html: .gbadge, .gbadge small, and updateFileDrag.
// The tab separates the label and optional <small>; it is never displayed.
struct DragFeedbackLayout {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> label;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> hint;
    float label_width = 0;
    float hint_width = 0;
    float width = 0;
};

inline DragFeedbackLayout LayoutDragFeedback(IDWriteFactory* factory, std::wstring_view text, float scale, bool blocked = false) {
    DragFeedbackLayout result;
    const auto split = text.find(L'\t');
    // Laid out on every frame of a drag: the two formats are kept per factory
    // and scale instead of being created each time.
    struct Formats { IDWriteFactory* factory = nullptr; float scale = 0; Microsoft::WRL::ComPtr<IDWriteTextFormat> label, hint; };
    thread_local Formats cached;
    if (cached.factory != factory || cached.scale != scale) cached = Formats{factory, scale, nullptr, nullptr};
    auto make = [&](std::wstring_view value, float size, auto& layout, float& width) {
        auto& format = size >= 12 ? cached.label : cached.hint;
        if (!format) {
            if (FAILED(factory->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * scale, L"", &format))) return;
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (FAILED(factory->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format.Get(),
            10000 * scale, 24 * scale, &layout))) return;
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(layout->GetMetrics(&metrics))) width = metrics.widthIncludingTrailingWhitespace;
    };
    make(text.substr(0, split), 12, result.label, result.label_width);
    if (split != std::wstring_view::npos)
        make(text.substr(split + 1), 10, result.hint, result.hint_width);
    // padding 10 + icon 11 + gap 6 + label + (gap 6 + margin-left 4 + small) + padding 10.
    result.width = (37 + (blocked ? 2 : 0)) * scale + result.label_width + (result.hint ? 10 * scale + result.hint_width : 0);
    return result;
}

inline void DrawDragFeedback(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush,
    const DragFeedbackLayout& layout, float x, float y, float scale, bool dark, bool blocked, bool copy, bool cancel,
    D2D1_COLOR_F fill = D2D1::ColorF(0x0078D4), D2D1_COLOR_F on_fill = D2D1::ColorF(0xFFFFFF)) {
    // .gbadge is the accent; callers that know the window's theme pass its
    // accent and the text colour that reads on it.
    const auto pill = D2D1::RoundedRect(D2D1::RectF(x, y, x + layout.width, y + 24 * scale), 12 * scale, 12 * scale);
    // The DC target has no blur effect. Sample the two CSS shadow kernels as
    // nested alpha contours, using the reference's offset, blur and opacity.
    auto shadow = [&](float offset, float blur, float opacity) {
        const float sigma = blur * 0.5f;
        float previous = 0;
        for (int layer = static_cast<int>(std::ceil(3 * sigma)); layer >= 0; --layer) {
            const float alpha = opacity * 0.5f * std::erfc(static_cast<float>(layer) / (1.41421356f * sigma));
            auto color = D2D1::ColorF(dark ? 0x000000 : 0x303B50, (alpha - previous) / (1 - previous));
            brush->SetColor(color);
            const float spread = static_cast<float>(layer) * scale;
            target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - spread, y + offset * scale - spread,
                x + layout.width + spread, y + (24 + offset) * scale + spread), 12 * scale + spread, 12 * scale + spread), brush);
            previous = alpha;
        }
    };
    shadow(8, dark ? 28.0f : 24.0f, dark ? 0.45f : 0.16f);
    shadow(1, 3, dark ? 0.35f : 0.10f);
    fill.a = 1.0f;
    brush->SetColor(blocked ? D2D1::ColorF(dark ? 0x2B2B2B : 0xFFFFFF) : fill);
    target->FillRoundedRectangle(pill, brush);
    if (blocked) {
        brush->SetColor(dark ? D2D1::ColorF(0xFFFFFF, 0.07f) : D2D1::ColorF(0xDCE3D9));
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 0.5f * scale, y + 0.5f * scale,
            x + layout.width - 0.5f * scale, y + 23.5f * scale), 11.5f * scale, 11.5f * scale), brush, scale);
    }
    const D2D1_COLOR_F ink = blocked ? (dark ? D2D1::ColorF(0xFFFFFF, 0.606f) : D2D1::ColorF(0x66748A)) : on_fill;
    brush->SetColor(ink);
    x += blocked ? scale : 0;
    const auto icon = cancel ? command_icons::FromGlyph(L"\xE711") : blocked ? command_icons::FromGlyph(L"\xE7BA") :
        copy ? command_icons::Icon::Copy : command_icons::Icon::FolderGo;
    command_icons::Draw(target, brush, nullptr, icon, D2D1::RectF(x + 10 * scale, y + 6.5f * scale,
        x + 21 * scale, y + 17.5f * scale));
    if (layout.label) target->DrawTextLayout(D2D1::Point2F(x + 27 * scale, y), layout.label.Get(), brush);
    if (layout.hint) {
        auto muted = ink; muted.a *= 0.8f; brush->SetColor(muted);
        target->DrawTextLayout(D2D1::Point2F(x + 37 * scale + layout.label_width, y), layout.hint.Get(), brush);
    }
}

} // namespace pulse::ui
