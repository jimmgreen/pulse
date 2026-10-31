#include "quick_preview_window.h"
#include "text_contrast_scope.h"
#include "visible_text_renderer.h"
#include <cstdio>
#include <d2d1helper.h>
#include <utility>
#include <cmath>

namespace pulse::ui {

void QuickPreviewWindow::ResetTextDrawingCache() {
    text_commands_.reset();
    text_commands_device_.reset();
    text_commands_params_.reset();
    last_text_ms_ = 0.0;
}

void QuickPreviewWindow::DrawCachedText(ID2D1DeviceContext* dc,
                                       ID2D1SolidColorBrush* text_brush,
                                       float origin_x, float origin_y) {
    bool use_visible_text = !TextOptimizationBaselineForTest() && preview_text_.size() >= 4096 && !text_commands_.get();
#ifdef PULSE_WITH_SELFTEST
    static const bool previous = GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) &&
        GetEnvironmentVariableW(L"PULSE_TEST_VISIBLE_TEXT_BASELINE", nullptr, 0);
    use_visible_text = use_visible_text && !previous;
#endif
    if (use_visible_text) {
        VisibleTextDrawStats* stats_ptr = nullptr;
#ifdef PULSE_WITH_SELFTEST
        VisibleTextDrawStats stats{};
        static unsigned trace_count = 0;
        const bool trace = trace_count < 4 &&
            GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) &&
            GetEnvironmentVariableW(L"PULSE_TEST_VISIBLE_TEXT_TRACE", nullptr, 0);
        if (trace) stats_ptr = &stats;
#endif
        const bool drawn = DrawVisibleTextLayout(dc, text_layout_.get(), text_brush,
            D2D1::Point2F(origin_x, origin_y), ContentRect(), stats_ptr);
#ifdef PULSE_WITH_SELFTEST
        if (trace) {
            ++trace_count;
            printf("visible_text used=%d total=%u culled=%u drawn=%u\n", drawn,
                stats.total_runs, stats.culled_runs, stats.drawn_runs);
            printf("visible_color runs=%u culled=%u translated=%u empty=%u layers=%u zero_alpha=%u first=(%.3f,%.3f) alpha=%.3f format=%u\n",
                stats.color_runs, stats.color_culled_runs, stats.color_translated_runs, stats.color_empty_runs,
                stats.color_layers, stats.color_zero_alpha_layers, stats.first_color_x, stats.first_color_y,
                stats.first_color_alpha, stats.first_color_format);
            for (unsigned i = 0; i < stats.emoji_diagnostic_count; ++i) {
                const auto& e = stats.emoji_diagnostics[i];
                printf("visible_emoji utf16=%x/%x fmt=%u glyphs=%u y=%.3f bounds=%.3f/%.3f face=%lx translate=%lx culled=%d\n",
                    e.utf16_first, e.utf16_second, e.formats, e.glyph_count, e.baseline_y, e.bound_top, e.bound_bottom,
                    static_cast<unsigned long>(e.face_result), static_cast<unsigned long>(e.translate_result), e.culled);
            }
        }
#endif
        if (drawn) {
            ResetTextDrawingCache();
            return;
        }
    }
    D2D1_MATRIX_3X2_F transform{};
    dc->GetTransform(&transform);
    // Animation transforms use native drawing so pixel snapping remains unchanged.
    // Let the first frame measure native drawing; cheap text needs no recording.
    const bool cache_allowed = !TextOptimizationBaselineForTest() && preview_text_.size() >= 4096 && preview_text_.size() <= ipc::kPreviewMaxTextChars &&
        (text_commands_.get() || last_text_ms_ > 8.0) &&
        transform._11 == 1.0f && transform._12 == 0.0f && transform._21 == 0.0f &&
        transform._22 == 1.0f && transform._31 == 0.0f && transform._32 == 0.0f;
    if (cache_allowed) {
        ComPtr<ID2D1Device> device;
        ComPtr<IDWriteRenderingParams> params;
        dc->GetDevice(&device);
        dc->GetTextRenderingParams(&params);
        float dpi_x = 0.0f, dpi_y = 0.0f;
        dc->GetDpi(&dpi_x, &dpi_y);
        const auto color = text_brush->GetColor();
        const auto aa = dc->GetTextAntialiasMode();
        if (device.get() != text_commands_device_.get() ||
            params.get() != text_commands_params_.get() || aa != text_commands_aa_ ||
            dpi_x != text_commands_dpi_x_ || dpi_y != text_commands_dpi_y_ ||
            color.r != text_commands_color_.r || color.g != text_commands_color_.g ||
            color.b != text_commands_color_.b || color.a != text_commands_color_.a) {
            ResetTextDrawingCache();
        }
        if (!text_commands_.get() && device.get()) {
            // Record glyph commands independently of the current viewport clip. Keeping
            // the original layout preserves shaping, wrapping and color-font layers.
            ComPtr<ID2D1DeviceContext> recorder;
            ComPtr<ID2D1CommandList> commands;
            if (SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &recorder)) &&
                SUCCEEDED(recorder->CreateCommandList(&commands))) {
                recorder->SetDpi(dpi_x, dpi_y);
                recorder->SetTextAntialiasMode(aa);
                recorder->SetTextRenderingParams(params.get());
                recorder->SetTarget(commands.get());
                recorder->BeginDraw();
                recorder->DrawTextLayout(D2D1::Point2F(origin_x, origin_y), text_layout_.get(), text_brush,
                    D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                const HRESULT draw_result = recorder->EndDraw();
                recorder->SetTarget(nullptr);
                if (SUCCEEDED(draw_result) && SUCCEEDED(commands->Close())) {
                    text_commands_ = std::move(commands);
                    text_commands_device_ = std::move(device);
                    text_commands_params_ = std::move(params);
                    text_commands_color_ = color;
                    text_commands_origin_ = D2D1::Point2F(origin_x, origin_y);
                    text_commands_dpi_x_ = dpi_x;
                    text_commands_dpi_y_ = dpi_y;
                    text_commands_aa_ = aa;
                }
            }
        }
    }
    const float offset_x = origin_x - text_commands_origin_.x;
    const float offset_y = origin_y - text_commands_origin_.y;
    const float pixels_x = offset_x * text_commands_dpi_x_ / 96.0f;
    const float pixels_y = offset_y * text_commands_dpi_y_ / 96.0f;
    // Fractional scrolling must retain native glyph snapping. Keep the recording
    // for later whole-pixel offsets instead of recording on every touchpad frame.
    const bool whole_pixels = pixels_x == std::round(pixels_x) && pixels_y == std::round(pixels_y);
    if (cache_allowed && text_commands_.get() && whole_pixels) {
        dc->DrawImage(text_commands_.get(), D2D1::Point2F(offset_x, offset_y));
    } else {
        dc->DrawTextLayout(D2D1::Point2F(origin_x, origin_y), text_layout_.get(), text_brush,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
}

} // namespace pulse::ui
