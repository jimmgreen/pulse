#pragma once
#include <d2d1_1.h>
#include <dwrite.h>

namespace pulse::ui {
struct VisibleTextRunDiagnostic {
    UINT32 utf16_first = 0;
    UINT32 utf16_second = 0;
    UINT32 formats = 0;
    UINT32 glyph_count = 0;
    FLOAT baseline_y = 0;
    FLOAT bound_top = 0;
    FLOAT bound_bottom = 0;
    HRESULT face_result = E_PENDING;
    HRESULT translate_result = E_PENDING;
    bool culled = false;
};
struct VisibleTextDrawStats {
    UINT32 emoji_diagnostic_count = 0;
    VisibleTextRunDiagnostic emoji_diagnostics[3]{};
    UINT32 total_runs = 0;
    UINT32 culled_runs = 0;
    UINT32 drawn_runs = 0;
    UINT32 paint_plain_glyphs = 0;
    UINT32 paint_unbounded_glyphs = 0;
    UINT32 color_runs = 0;
    UINT32 color_culled_runs = 0;
    UINT32 color_translated_runs = 0;
    UINT32 color_layers = 0;
    UINT32 color_empty_runs = 0;
    UINT32 color_zero_alpha_layers = 0;
    FLOAT first_color_x = 0;
    FLOAT first_color_y = 0;
    FLOAT first_color_alpha = 0;
    UINT32 first_color_format = 0;
};
// clip is in layout drawing coordinates (the same coordinates as origin).
// False requests native DrawTextLayout fallback. The caller retains its viewport clip.
bool DrawVisibleTextLayout(ID2D1DeviceContext* dc, IDWriteTextLayout* layout,
                           ID2D1Brush* brush, D2D1_POINT_2F origin, D2D1_RECT_F clip,
                           VisibleTextDrawStats* stats = nullptr);
}
