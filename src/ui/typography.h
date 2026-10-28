// Shared DirectWrite typography policy for Pulse-owned UI surfaces.
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>

#include <cstdint>
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
// The family for native EDIT controls: the interface font family below, or the
// language's built-in one. Returns storage owned by the caller's thread.
const wchar_t* PreferredTextFamily();
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

// Process-wide interface font family (Settings > 界面字体), e.g.
// "Microsoft YaHei UI". Empty keeps the built-in per-language cascade. The
// choice covers Text and Display only: glyph icons must keep their icon font
// and code must keep its monospace face. A family that is not installed is
// ignored, so a saved preference can never leave the UI without a font.
// Callers recreate cached formats after a change.
void SetUiFontFamily(std::wstring family);
std::wstring UiFontFamily();

// One row of the font picker. `value` is what the preferences store (the
// English family name, which FindFamilyName resolves in every locale and stays
// valid when the UI language changes); `label` is the name to show.
struct FontFamilyChoice {
    std::wstring value;
    std::wstring label;
};
// The first row is the built-in cascade (empty value, localized label). The
// rest are installed families that can draw the interface: Latin letters plus
// CJK, which keeps icon and symbol faces out of the list. Common Chinese UI
// faces come first, then the remaining families by name.
std::vector<FontFamilyChoice> UiFontFamilies(IDWriteFactory2* factory);
// The label the picker shows for `family`, or empty when the family is no longer
// installed (the interface then falls back to the built-in cascade).
std::wstring UiFontFamilyLabel(IDWriteFactory2* factory, const std::wstring& family);

// Native EDIT controls hosted on Pulse surfaces (address bar and rename,
// dialog fields, menu filters, Quick Look find) all use this font: the UI text
// family at `dip` DIPs times the interface font size and the DPI scale, so the
// editor matches the DirectWrite text around it. The caller owns the HFONT and
// recreates it when Generation() changes.
int EditFontPixels(float scale, float dip = 14.0f) noexcept;
HFONT CreateEditFont(float scale, float dip = 14.0f);

// Invalidate language-dependent fallback and measurement caches.
void InvalidateCaches();
// Text formats were recreated (DPI, font scale, a new window). Width caches
// keyed by format pointer must not hand a new format the widths of a freed
// one at the same address; this bumps Generation() without rebuilding the
// font fallback.
void InvalidateMeasurements() noexcept;
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
