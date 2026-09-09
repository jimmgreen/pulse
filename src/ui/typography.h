// Shared DirectWrite typography policy for Pulse-owned UI surfaces.
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>

#include <cstdint>
#include <string_view>

namespace pulse::ui { class Compositor; }

namespace pulse::ui::typography {

enum class FontRole {
    Text,
    Display,
    Icon,
    Monospace,
};

struct TextFormatSpec {
    FontRole role = FontRole::Text;
    float size = 14.0f;
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
};

const wchar_t* LocaleName() noexcept;
bool HasIconFont(IDWriteFactory2* factory);
const wchar_t* PreferredTextFamily() noexcept;
HRESULT CreateTextFormat(IDWriteFactory2* factory, const TextFormatSpec& spec,
                         IDWriteTextFormat** format);
HRESULT CreateRenderingParams(IDWriteFactory2* factory, HMONITOR monitor,
                              IDWriteRenderingParams2** params);

// Invalidate language-dependent fallback and measurement caches.
void InvalidateCaches();
std::uint64_t Generation() noexcept;

// Pulse lays out in physical pixels on a 96-DPI D2D target. Keep vertical
// text boxes stable while preserving DirectWrite's natural horizontal spacing.
D2D1_RECT_F SnapVerticalBounds(const D2D1_RECT_F& bounds) noexcept;

// Dest rects clip ink, not advance. Luma's optical weight and Mitchell
// filter extend past DWrite's cluster width by about this much.
float InkPad(IDWriteTextFormat* format) noexcept;

// DWrite advance plus right overhang. Empty text is 0.
float MeasureAdvance(IDWriteFactory2* factory, IDWriteTextFormat* format,
                     std::wstring_view text);

// max(DWrite, Luma) plus InkPad so a size-to-content box does not shave
// the last glyph. Falls back to MeasureAdvance when Luma is off.
float MeasureLine(Compositor* compositor, IDWriteTextFormat* format,
                  std::wstring_view text);

// Wrapped block height at `width`. Newlines are paragraph breaks.
float MeasureWrapped(IDWriteFactory2* factory, IDWriteTextFormat* format,
                     std::wstring_view text, float width);

} // namespace pulse::ui::typography
