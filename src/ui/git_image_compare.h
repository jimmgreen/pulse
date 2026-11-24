#pragma once
// Git image diff support: background WIC decoding to premultiplied PBGRA and
// interactive before/after slider comparison rendering.
#include "fluent_components.h"
#include "ui_compositor.h"
#include <cstdint>
#include <string_view>
#include <vector>

namespace pulse::ui {

struct DecodedGitImage {
    uint32_t orig_w = 0;
    uint32_t orig_h = 0;
    uint32_t bmp_w = 0;
    uint32_t bmp_h = 0;
    uint64_t file_bytes = 0;
    std::vector<uint8_t> pixels;  // premultiplied 32bpp PBGRA

    bool ok() const noexcept { return bmp_w > 0 && bmp_h > 0 && !pixels.empty(); }
};

// Decodes encoded image bytes off the UI thread. Scales down if larger than
// `max_edge` while preserving `orig_w`/`orig_h` for display metadata.
DecodedGitImage DecodeGitImageBytes(std::string_view bytes, uint32_t max_edge = 2048);

ComPtr<ID2D1Bitmap> CreateBitmapFromDecodedImage(ID2D1DeviceContext* dc, const DecodedGitImage& img);

struct GitImageCompareLayout {
    D2D1_RECT_F stage{};
    D2D1_RECT_F image{};
    D2D1_RECT_F divider{};
    D2D1_RECT_F handle{};
    D2D1_RECT_F meta{};
    float split_x = 0.0f;
};

GitImageCompareLayout LayoutGitImageCompare(const D2D1_RECT_F& bounds,
                                            uint32_t img_w, uint32_t img_h,
                                            float split_ratio, float scale);

float GitImageSplitFromX(const GitImageCompareLayout& lay, float x);

void DrawGitImageCompare(ID2D1DeviceContext* dc, fluent::Painter& painter,
                         IDWriteTextFormat* small_fmt, ID2D1SolidColorBrush* brush,
                         const Theme& theme, float scale, const D2D1_RECT_F& bounds,
                         ID2D1Bitmap* old_bmp, const DecodedGitImage* old_meta,
                         ID2D1Bitmap* new_bmp, const DecodedGitImage* new_meta,
                         float split_ratio, bool plain_terms, bool dragging);

}  // namespace pulse::ui
