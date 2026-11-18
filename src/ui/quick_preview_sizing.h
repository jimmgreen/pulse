#pragma once
// Quick Look window size presets (Settings > 快速预览 > 预览窗口) and a
// header-only probe of a picture's pixel size. Pure functions: no window or
// file access (pulse_preview_sizing_test).
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulse::ui {

// Same order and values as app::kPreviewSize*.
enum class PreviewSizeMode : int { Fit = 0, Standard, Large, Maximized, FullScreen };

struct PreviewSizeInput {
    PreviewSizeMode mode = PreviewSizeMode::Fit;
    RECT work{};                 // the monitor's work area, px
    POINT centre{};              // the window is centred here, then kept inside `work`
    int content_w = 0;           // the picture or video, px; 0 = not media / not known yet
    int content_h = 0;
    int chrome_h = 0;            // header + footer + playback bar, px
    float scale = 1.0f;          // DPI scale
    int remembered_w = 0;        // a size the user dragged the window to, px; 0 = none
    int remembered_h = 0;
};

inline constexpr float kPreviewMinWidthDip = 480.0f;
inline constexpr float kPreviewMinHeightDip = 320.0f;
inline constexpr double kPreviewFitShare = 0.90;       // 适应内容: at most 90% of the work area
inline constexpr double kPreviewStandardShare = 0.70;  // 标准
inline constexpr double kPreviewLargeShare = 0.88;     // 大

inline RECT CentredPreviewRect(POINT centre, int width, int height, const RECT& work) {
    const int work_w = work.right - work.left, work_h = work.bottom - work.top;
    width = std::clamp(width, 1, (std::max)(1, work_w));
    height = std::clamp(height, 1, (std::max)(1, work_h));
    int x = centre.x - width / 2, y = centre.y - height / 2;
    x = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
    y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    return RECT{x, y, x + width, y + height};
}

// Where the window opens. 最大化 returns the work area; 全屏 too (the window
// then covers the whole monitor itself). A remembered size wins over the
// preset's. 适应内容 keeps the content's shape at no more than its real
// pixels, within 90% of the work area; without a content size it is 标准.
inline RECT PreviewWindowRect(const PreviewSizeInput& in) {
    const int work_w = in.work.right - in.work.left, work_h = in.work.bottom - in.work.top;
    if (work_w <= 0 || work_h <= 0) return RECT{0, 0, 0, 0};
    if (in.mode == PreviewSizeMode::Maximized || in.mode == PreviewSizeMode::FullScreen)
        return in.work;
    const float scale = in.scale > 0.0f ? in.scale : 1.0f;
    const int min_w = (std::min)(work_w, static_cast<int>(std::lround(kPreviewMinWidthDip * scale)));
    const int min_h = (std::min)(work_h, static_cast<int>(std::lround(kPreviewMinHeightDip * scale)));
    double w = 0.0, h = 0.0;
    if (in.remembered_w > 0 && in.remembered_h > 0) {
        w = in.remembered_w;
        h = in.remembered_h;
    } else if (in.mode == PreviewSizeMode::Fit && in.content_w > 0 && in.content_h > 0) {
        const double box_w = work_w * kPreviewFitShare;
        const double box_h = (std::max)(1.0, work_h * kPreviewFitShare - in.chrome_h);
        const double k = (std::min)({box_w / in.content_w, box_h / in.content_h, 1.0});
        w = in.content_w * k;
        h = in.content_h * k + in.chrome_h;
    } else {
        const double share = in.mode == PreviewSizeMode::Large ? kPreviewLargeShare : kPreviewStandardShare;
        w = work_w * share;
        h = work_h * share;
    }
    const int width = std::clamp(static_cast<int>(std::lround(w)), min_w, work_w);
    const int height = std::clamp(static_cast<int>(std::lround(h)), min_h, work_h);
    return CentredPreviewRect(in.centre, width, height, in.work);
}

namespace preview_probe {
inline uint32_t Be16(const uint8_t* p) { return (uint32_t{p[0]} << 8) | p[1]; }
inline uint32_t Be32(const uint8_t* p) { return (Be16(p) << 16) | Be16(p + 2); }
inline uint32_t Le16(const uint8_t* p) { return uint32_t{p[0]} | (uint32_t{p[1]} << 8); }
inline uint32_t Le24(const uint8_t* p) { return Le16(p) | (uint32_t{p[2]} << 16); }
inline uint32_t Le32(const uint8_t* p) { return Le16(p) | (Le16(p + 2) << 16); }

// EXIF orientation from an APP1 payload (after "Exif\0\0"); 1 when absent.
inline int ExifOrientation(const uint8_t* tiff, size_t n) {
    if (n < 8) return 1;
    const bool le = tiff[0] == 'I' && tiff[1] == 'I';
    if (!le && !(tiff[0] == 'M' && tiff[1] == 'M')) return 1;
    auto u16 = [&](size_t at) { return le ? Le16(tiff + at) : Be16(tiff + at); };
    auto u32 = [&](size_t at) { return le ? Le32(tiff + at) : Be32(tiff + at); };
    if (u16(2) != 42) return 1;
    const size_t ifd = u32(4);
    if (ifd > n || n - ifd < 2) return 1;
    const size_t count = u16(ifd);
    for (size_t i = 0; i < count; ++i) {
        const size_t entry = ifd + 2 + i * 12;
        if (entry > n || n - entry < 12) break;
        if (u16(entry) == 0x0112) {
            const int value = static_cast<int>(u16(entry + 8));
            return value >= 1 && value <= 8 ? value : 1;
        }
    }
    return 1;
}
} // namespace preview_probe

// Pixel size of a PNG, JPEG (EXIF orientation applied, like the decoder),
// GIF, BMP or WebP from its first `n` bytes; false for anything else or when
// the size lies beyond them. Never decodes.
inline bool ProbeImageSize(const uint8_t* p, size_t n, uint32_t& width, uint32_t& height) {
    using namespace preview_probe;
    width = height = 0;
    if (!p) return false;
    auto done = [&](uint32_t w, uint32_t h) {
        if (w == 0 || h == 0 || w > 1000000 || h > 1000000) return false;
        width = w; height = h;
        return true;
    };
    if (n >= 24 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G' &&
        p[12] == 'I' && p[13] == 'H' && p[14] == 'D' && p[15] == 'R')
        return done(Be32(p + 16), Be32(p + 20));
    if (n >= 10 && p[0] == 'G' && p[1] == 'I' && p[2] == 'F' && p[3] == '8')
        return done(Le16(p + 6), Le16(p + 8));
    if (n >= 26 && p[0] == 'B' && p[1] == 'M') {
        const uint32_t dib = Le32(p + 14);
        if (dib == 12) return done(Le16(p + 18), Le16(p + 20));
        if (dib >= 40) {
            const int32_t h = static_cast<int32_t>(Le32(p + 22));
            return done(Le32(p + 18), static_cast<uint32_t>(h < 0 ? -static_cast<int64_t>(h) : h));
        }
        return false;
    }
    if (n >= 30 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
        p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P') {
        if (p[12] == 'V' && p[13] == 'P' && p[14] == '8' && p[15] == ' ' &&
            p[23] == 0x9D && p[24] == 0x01 && p[25] == 0x2A)
            return done(Le16(p + 26) & 0x3FFF, Le16(p + 28) & 0x3FFF);
        if (p[12] == 'V' && p[13] == 'P' && p[14] == '8' && p[15] == 'L' && p[20] == 0x2F) {
            const uint32_t b1 = p[21], b2 = p[22], b3 = p[23], b4 = p[24];
            return done(1 + (b1 | ((b2 & 0x3F) << 8)), 1 + ((b2 >> 6) | (b3 << 2) | ((b4 & 0x0F) << 10)));
        }
        if (p[12] == 'V' && p[13] == 'P' && p[14] == '8' && p[15] == 'X')
            return done(1 + Le24(p + 24), 1 + Le24(p + 27));
        return false;
    }
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        int orientation = 1;
        size_t i = 2;
        while (i + 4 <= n) {
            if (p[i] != 0xFF) return false;
            const uint8_t marker = p[i + 1];
            if (marker == 0xFF) { ++i; continue; }            // fill byte
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
                i += 2;
                continue;
            }
            if (marker == 0xD9 || marker == 0xDA) return false;  // no frame header before the scan
            const size_t length = Be16(p + i + 2);
            if (length < 2) return false;
            const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 &&
                             marker != 0xC8 && marker != 0xCC;
            if (sof) {
                if (i + 9 > n) return false;
                uint32_t h = Be16(p + i + 5), w = Be16(p + i + 7);
                if (orientation >= 5) std::swap(w, h);
                return done(w, h);
            }
            if (marker == 0xE1 && length >= 16 && i + 2 + length <= n &&
                p[i + 4] == 'E' && p[i + 5] == 'x' && p[i + 6] == 'i' && p[i + 7] == 'f' &&
                p[i + 8] == 0 && p[i + 9] == 0)
                orientation = ExifOrientation(p + i + 10, length - 8);
            i += 2 + length;
        }
    }
    return false;
}

} // namespace pulse::ui
