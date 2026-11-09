#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace pulse::ui {

struct LumaTextStats {
    // Enabled in diagnostic builds, or explicitly with PULSE_STARTUP_TRACE.
    std::uint64_t layout_time_us = 0;
    std::uint64_t draw_time_us = 0;
    std::uint64_t frame_end_time_us = 0;
    std::uint64_t recording_end_time_us = 0;
    std::uint64_t shaping_time_us = 0;
    std::uint64_t raster_time_us = 0;
    std::uint64_t upload_time_us = 0;
    std::uint64_t composition_time_us = 0;
    std::uint64_t draw_calls = 0;
    std::uint64_t freetype_glyphs = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t surface_cache_hits = 0;
    std::uint64_t surface_cache_misses = 0;
    std::uint64_t surface_cache_evictions = 0;
    std::uint64_t fallback_draws = 0;
    std::uint64_t fallback_text = 0, fallback_layout = 0, fallback_split = 0;
    std::uint64_t fail_input = 0, fail_font = 0, fail_layout = 0, fail_bounds = 0, fail_backend = 0;
    std::uint64_t edit_directwrite_draws = 0;
    std::uint64_t edit_directwrite_hits = 0;
    std::uint64_t edit_layout_builds = 0;
    std::uint64_t edit_layout_cache_hits = 0;
    DWRITE_RENDERING_MODE edit_rendering_mode = DWRITE_RENDERING_MODE_DEFAULT;
};

class LumaTextRenderer {
public:
    // Run the EDIT's default mouse handler without letting it paint, then the
    // caller presents DirectWrite. Pass the same format used by PaintEdit so
    // click-to-caret uses the same cluster positions as the displayed text.
    LRESULT CallEditDefaultMouse(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                   IDWriteTextFormat* format = nullptr);
    void SynchronizeEditSelection(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
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
    // Leading-aligned single line; text from UTF-16 index `split` on uses
    // split_foreground. False when the line would need an ellipsis.
    // DirectWrite layout with LumaText glyphs; `clip` = the layout box.
    bool DrawLayout(D2D1_POINT_2F origin, IDWriteTextLayout* layout,
                    const D2D1_COLOR_F& foreground, bool clip);
    bool DrawSplit(std::wstring_view text, IDWriteTextFormat* format,
                   const D2D1_RECT_F& bounds, const D2D1_COLOR_F& foreground,
                   std::uint32_t split, const D2D1_COLOR_F& split_foreground,
                   const D2D1_COLOR_F& background);
    bool Measure(std::wstring_view text, IDWriteTextFormat* format,
                 float& width, float* height = nullptr);
    bool PaintEdit(HWND hwnd, HDC hdc, IDWriteTextFormat* format,
                   const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background);

    // Called within the main device context BeginDraw/EndDraw. Uses the same
    // edit bitmap and IME geometry, without writing to a child-window DC.
    bool DrawEditOnSurface(HWND hwnd, IDWriteTextFormat* format,
        const D2D1_COLOR_F& foreground, const D2D1_COLOR_F& background, const D2D1_RECT_F& bounds);

    const LumaTextStats& Stats() const noexcept;
    void RecordFallback(unsigned api = 0) noexcept;

private:
    void TrackEdit(HWND hwnd);
    void SyncEditSelection(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK EditSelectionProc(HWND hwnd, UINT message, WPARAM wparam,
                                              LPARAM lparam, UINT_PTR id, DWORD_PTR data);
    std::vector<HWND> tracked_edits_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulse::ui
