#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace pulse::ui {

struct LumaTextStats {
    std::uint64_t draw_calls = 0;
    std::uint64_t freetype_glyphs = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t surface_cache_hits = 0;
    std::uint64_t surface_cache_misses = 0;
    std::uint64_t surface_cache_evictions = 0;
    std::uint64_t fallback_draws = 0;
};

class LumaTextRenderer {
public:
    // Run the EDIT's default mouse handler without letting it paint, then the
    // caller presents LumaText. Pass the same format used by PaintEdit so
    // click-to-caret uses LumaText cluster positions, not GDI.
    LRESULT CallEditDefaultMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                 IDWriteTextFormat* format = nullptr);
    LumaTextRenderer();
    ~LumaTextRenderer();
    LumaTextRenderer(const LumaTextRenderer&) = delete;
    LumaTextRenderer& operator=(const LumaTextRenderer&) = delete;

    bool Init(IDWriteFactory* dwrite, ID2D1RenderTarget* target);
    void Shutdown() noexcept;
    bool Enabled() const noexcept;

    bool Draw(std::wstring_view text, IDWriteTextFormat* format,
              const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
              const D2D1_COLOR_F& background,
              DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING);
    bool Measure(std::wstring_view text, IDWriteTextFormat* format,
                 float& width, float* height = nullptr);
    bool PaintEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                   const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background);

    const LumaTextStats& Stats() const noexcept;
    void RecordFallback() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::ui
