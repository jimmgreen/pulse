#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace pulse::ui {
// Worker-side box filtering of premultiplied BGRA. No filesystem or COM work.
inline bool CompactFolderPixels(const std::vector<uint8_t>& source,
    uint32_t width, uint32_t height, uint32_t stride,
    std::vector<uint8_t>& output, uint32_t& out_width, uint32_t& out_height) {
    if (!width || !height || width > 512 || height > 512 || stride < width * 4 ||
        source.size() < static_cast<size_t>(stride) * height) return false;
    const auto edge = (std::max)(width, height);
    const auto target = (std::min)(128u, edge);
    out_width = (std::max)(1u, width * target / edge);
    out_height = (std::max)(1u, height * target / edge);
    output.resize(static_cast<size_t>(out_width) * out_height * 4);
    for (uint32_t y = 0; y < out_height; ++y) {
        const uint32_t y0 = y * height / out_height;
        const uint32_t y1 = (y + 1) * height / out_height;
        for (uint32_t x = 0; x < out_width; ++x) {
            const uint32_t x0 = x * width / out_width;
            const uint32_t x1 = (x + 1) * width / out_width;
            uint32_t sums[4]{};
            for (uint32_t sy = y0; sy < y1; ++sy) {
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const auto* pixel = source.data() + static_cast<size_t>(sy) * stride + sx * 4;
                    for (size_t c = 0; c < 4; ++c) sums[c] += pixel[c];
                }
            }
            const auto count = (x1 - x0) * (y1 - y0);
            auto* pixel = output.data() + (static_cast<size_t>(y) * out_width + x) * 4;
            for (size_t c = 0; c < 4; ++c) pixel[c] = static_cast<uint8_t>((sums[c] + count / 2) / count);
        }
    }
    return true;
}
}
