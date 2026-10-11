// Shared DirectWrite typography policy for Pulse-owned UI surfaces.
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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

// Process-wide text rendering choice (Settings > Text rendering).
enum class TextRenderMode : int { Auto = 0, Sharp = 1, Smooth = 2 };
void SetTextRenderMode(TextRenderMode mode) noexcept;
TextRenderMode CurrentTextRenderMode() noexcept;
// Sent to existing UI-thread windows after the selected backend changes.
UINT TextBackendChangedMessage() noexcept;
// Auto draws with LumaText; Sharp and Smooth use DirectWrite directly.
inline bool UseLumaTextForUi() noexcept { return CurrentTextRenderMode() == TextRenderMode::Auto; }

// Process-wide UI font size (Settings > Interface font size), in percent of the
// built-in sizes: 90, 100, 112 or 125. Applies to every role except Icon, so
// glyph icons keep their slots. Callers recreate cached formats after a change.
void SetUiFontScale(int percent) noexcept;
int UiFontScalePercent() noexcept;
float UiFontScale() noexcept;

// Process-wide UI font family override (Settings > Interface font).
// Empty = follow the built-in PreferredTextFamily() logic. Applies to every
// role except Icon, so glyph icons keep their slots. Bumps Generation().
void SetUiFontFamily(const std::wstring& family) noexcept;
std::wstring UiFontFamily() noexcept;
// Process-wide UI font weight override (Settings > Interface font weight).
// 400 = normal, 500 = medium, 700 = bold. Applies to every role except Icon.
void SetUiFontWeight(int weight) noexcept;
int UiFontWeight() noexcept;
// Enumerates installed font family names via the system font collection.
std::vector<std::wstring> SystemFontFamilies(IDWriteFactory* factory);

// Native EDIT controls hosted on Pulse surfaces (address bar and rename,
// dialog fields, menu filters, Quick Look find) all use this font: the UI text
// family at `dip` DIPs times the interface font size and the DPI scale, so the
// editor matches the DirectWrite text around it. The caller owns the HFONT and
// recreates it when Generation() changes.
int EditFontPixels(float scale, float dip = 14.0f) noexcept;
HFONT CreateEditFont(float scale, float dip = 14.0f);

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
