#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pulse::desktop::render::text_contrast {
constexpr size_t kGridSide = 16;
constexpr size_t kMaxSamples = kGridSide * kGridSide;
constexpr uint32_t kDarkInk = 0xFF1B2028u;
constexpr uint32_t kLightInk = 0xFFFFFFFFu;
struct Srgb {
    float r = 0, g = 0, b = 0;
};
struct Samples {
    std::array<Srgb, kMaxSamples> colors{};
    size_t count = 0;
};
inline float Unit(float v) noexcept { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; }
inline Srgb FromArgb(uint32_t color) noexcept {
    return {((color >> 16) & 255u) / 255.0f, ((color >> 8) & 255u) / 255.0f, (color & 255u) / 255.0f};
}
// D2D's ordinary BGRA UNORM source-over operates on encoded channels. Decode
// to linear light only after composition when measuring relative luminance.
inline Srgb SourceOver(Srgb background, Srgb foreground, float alpha) noexcept {
    const float a = Unit(alpha);
    return {Unit(background.r) * (1 - a) + Unit(foreground.r) * a,
            Unit(background.g) * (1 - a) + Unit(foreground.g) * a,
            Unit(background.b) * (1 - a) + Unit(foreground.b) * a};
}
inline Srgb SourceOver(Srgb background, uint32_t foreground, float opacity = 1.0f) noexcept {
    return SourceOver(background, FromArgb(foreground), ((foreground >> 24) & 255u) / 255.0f * Unit(opacity));
}
inline float LinearChannel(float encoded) noexcept {
    const float c = Unit(encoded);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
// WCAG sRGB relative luminance and contrast ratio; not encoded RGB brightness.
inline float RelativeLuminance(Srgb color) noexcept {
    return 0.2126f * LinearChannel(color.r) + 0.7152f * LinearChannel(color.g) + 0.0722f * LinearChannel(color.b);
}
// Rec.709 perceptual sRGB luma matching macOS 26 Tahoe / WacOS glassLumaHysteresisRange (0.40 / 0.60).
inline float PerceivedLuma(Srgb color) noexcept {
    return 0.2126f * Unit(color.r) + 0.7152f * Unit(color.g) + 0.0722f * Unit(color.b);
}
inline float MeanPerceivedLuma(const Samples& samples) noexcept {
    const size_t count = std::min(samples.count, kMaxSamples);
    if (!count) return -1.0f;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) sum += PerceivedLuma(samples.colors[i]);
    return static_cast<float>(sum / static_cast<double>(count));
}
// Hysteresis band [dark_threshold, light_threshold] keeps translucent glass ink stable across
// medium-brightness and dynamic video wallpapers instead of jittering per card or per frame.
inline bool ResolveHysteresisLightInk(float perceived_luma, bool has_previous, bool previous_light_ink,
                                      bool theme_dark, float dark_threshold = 0.40f,
                                      float light_threshold = 0.60f) noexcept {
    if (!std::isfinite(perceived_luma) || perceived_luma < 0.0f) {
        return has_previous ? previous_light_ink : theme_dark;
    }
    if (perceived_luma > light_threshold) return false;
    if (perceived_luma < dark_threshold) return true;
    return has_previous ? previous_light_ink : theme_dark;
}
inline float ContrastRatio(float a, float b) noexcept {
    return (std::max(a, b) + 0.05f) / (std::min(a, b) + 0.05f);
}
// Clear menu glass: the smallest theme veil (alpha over the vibrancy-boosted backdrop) that
// keeps the menu's theme-locked ink at `target` contrast for the poorest 10th percentile of
// the samples. A backdrop that already suits the ink keeps the light `floor_alpha`, so the
// glass stays clear; only a backdrop that fights the ink is veiled more, up to `max_alpha`.
// No samples: `fallback_alpha`. The ink is opposite the veil, so contrast rises with alpha.
inline float SolveGlassVeilAlpha(const Samples& samples, Srgb veil, uint32_t ink_argb, float vibrancy,
                                 float floor_alpha, float max_alpha, float fallback_alpha,
                                 float target = 4.5f) noexcept {
    floor_alpha = Unit(floor_alpha);
    max_alpha = std::max(floor_alpha, Unit(max_alpha));
    const size_t count = std::min(samples.count, kMaxSamples);
    if (!count) return std::clamp(fallback_alpha, floor_alpha, max_alpha);
    const float vib = std::isfinite(vibrancy) ? std::clamp(vibrancy, 0.0f, 3.0f) : 1.0f;
    std::array<Srgb, kMaxSamples> base{};
    for (size_t i = 0; i < count; ++i) {
        const Srgb c{Unit(samples.colors[i].r), Unit(samples.colors[i].g), Unit(samples.colors[i].b)};
        const float l = PerceivedLuma(c);
        base[i] = {Unit(l + (c.r - l) * vib), Unit(l + (c.g - l) * vib), Unit(l + (c.b - l) * vib)};
    }
    const size_t rank = (count + 9) / 10 - 1;
    const auto passes = [&](float alpha) {
        std::array<float, kMaxSamples> score{};
        for (size_t i = 0; i < count; ++i) {
            const Srgb bg = SourceOver(base[i], veil, alpha);
            score[i] = ContrastRatio(RelativeLuminance(bg), RelativeLuminance(SourceOver(bg, ink_argb)));
        }
        std::nth_element(score.begin(), score.begin() + rank, score.begin() + count);
        return score[rank] >= target;
    };
    if (passes(floor_alpha)) return floor_alpha;
    if (!passes(max_alpha)) return max_alpha;
    float lo = floor_alpha, hi = max_alpha;
    for (int i = 0; i < 12; ++i) {
        const float mid = 0.5f * (lo + hi);
        (passes(mid) ? hi : lo) = mid;
    }
    return hi;
}
inline Samples Fill(Srgb color) noexcept {
    Samples s;
    s.colors[0] = color;
    s.count = 1;
    return s;
}
inline Samples Fill(uint32_t color) noexcept { return Fill(FromArgb(color)); }
template<class Transform>
inline void TransformSamples(Samples& samples, Transform transform) {
    for (size_t i = 0; i < std::min(samples.count, kMaxSamples); ++i) {
        samples.colors[i] = transform(samples.colors[i]);
    }
}
struct InkChoice {
    bool light = false;
    bool valid = false;
    float light_contrast = 0, dark_contrast = 0;
};
inline InkChoice ChooseInk(const Samples& samples, uint32_t light_argb = kLightInk,
                           uint32_t dark_argb = kDarkInk, bool fallback_light = false) noexcept {
    const size_t count = std::min(samples.count, kMaxSamples);
    if (!count) return {fallback_light, false, 0, 0};
    std::array<float, kMaxSamples> light{}, dark{};
    double light_sum = 0, dark_sum = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto bg = samples.colors[i];
        const float lum = RelativeLuminance(bg);
        light[i] = ContrastRatio(lum, RelativeLuminance(SourceOver(bg, light_argb)));
        dark[i] = ContrastRatio(lum, RelativeLuminance(SourceOver(bg, dark_argb)));
        light_sum += light[i];
        dark_sum += dark[i];
    }
    // Nearest-rank 10th percentile protects the poorly contrasted parts of a
    // mixed region. Mean contrast breaks exact ties only; no brightness cutoff.
    const size_t rank = (count + 9) / 10 - 1;
    std::nth_element(light.begin(), light.begin() + rank, light.begin() + count);
    std::nth_element(dark.begin(), dark.begin() + rank, dark.begin() + count);
    const bool use_light = light[rank] != dark[rank] ? light[rank] > dark[rank] :
                         light_sum != dark_sum ? light_sum > dark_sum : fallback_light;
    return {use_light, true, light[rank], dark[rank]};
}
// Sample the cached opaque wallpaper's PBGRA image. Even a 31x31 region is
// bounded to 16x16 reads. Cell centers avoid bias toward the top/left edge.
inline Samples SamplePbgra(std::span<const uint8_t> pixels, uint32_t width, uint32_t height,
                           float left, float top, float right, float bottom) noexcept {
    Samples result;
    if (!width || !height || static_cast<uint64_t>(width) * height > pixels.size() / 4 ||
        !std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) || !std::isfinite(bottom) ||
        right <= left || bottom <= top) return result;
    left = std::clamp(left, 0.0f, static_cast<float>(width));
    right = std::clamp(right, 0.0f, static_cast<float>(width));
    top = std::clamp(top, 0.0f, static_cast<float>(height));
    bottom = std::clamp(bottom, 0.0f, static_cast<float>(height));
    if (right <= left || bottom <= top) return result;
    const size_t nx = static_cast<size_t>(std::min(16.0f, std::ceil(right - left)));
    const size_t ny = static_cast<size_t>(std::min(16.0f, std::ceil(bottom - top)));
    for (size_t y = 0; y < ny; ++y) {
        for (size_t x = 0; x < nx; ++x) {
            const auto px = std::min(width - 1, static_cast<uint32_t>(left + (x + 0.5f) * (right - left) / nx));
            const auto py = std::min(height - 1, static_cast<uint32_t>(top + (y + 0.5f) * (bottom - top) / ny));
            const auto* p = pixels.data() + (static_cast<size_t>(py) * width + px) * 4;
            result.colors[result.count++] = {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f};
        }
    }
    return result;
}
struct PhotoGrid {
    std::array<uint32_t, kMaxSamples> pbgra{};
    uint32_t width = 0, height = 0, source_width = 0, source_height = 0;
};
struct PhotoSamples {
    Samples colors;
    std::array<float, kMaxSamples> alpha{};
};
struct Rect {
    float left, top, right, bottom;
};
inline PhotoGrid MakePhotoGrid(std::span<const uint8_t> pixels, uint32_t width, uint32_t height) noexcept {
    PhotoGrid grid;
    if (!width || !height || static_cast<uint64_t>(width) * height > pixels.size() / 4) return grid;
    grid.source_width = width;
    grid.source_height = height;
    grid.width = std::min(width, 16u);
    grid.height = std::min(height, 16u);
    for (uint32_t y = 0; y < grid.height; ++y) {
        for (uint32_t x = 0; x < grid.width; ++x) {
            const uint32_t px = static_cast<uint32_t>((static_cast<uint64_t>(2 * x + 1) * width) / (2 * grid.width));
            const uint32_t py = static_cast<uint32_t>((static_cast<uint64_t>(2 * y + 1) * height) / (2 * grid.height));
            const auto* p = pixels.data() + (static_cast<size_t>(py) * width + px) * 4;
            grid.pbgra[y * grid.width + x] = p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
    }
    return grid;
}
inline PhotoSamples SamplePhotoGrid(const PhotoGrid& grid, Rect tile, Rect text) noexcept {
    PhotoSamples out;
    if (!grid.width || !grid.height || !grid.source_width || !grid.source_height) return out;
    for (float c : {tile.left, tile.top, tile.right, tile.bottom, text.left, text.top, text.right, text.bottom})
        if (!std::isfinite(c)) return out;
    const float tw = tile.right - tile.left, th = tile.bottom - tile.top;
    if (tw <= 0 || th <= 0) return out;
    text.left = std::max(tile.left, text.left);
    text.right = std::min(tile.right, text.right);
    text.top = std::max(tile.top, text.top);
    text.bottom = std::min(tile.bottom, text.bottom);
    if (text.right <= text.left || text.bottom <= text.top) return out;
    const float scale = std::max(tw / grid.source_width, th / grid.source_height);
    const float drawn_w = grid.source_width * scale, drawn_h = grid.source_height * scale;
    for (size_t y = 0; y < kGridSide; ++y) {
        for (size_t x = 0; x < kGridSide; ++x) {
            const float tx = text.left + (x + 0.5f) * (text.right - text.left) / kGridSide;
            const float ty = text.top + (y + 0.5f) * (text.bottom - text.top) / kGridSide;
            const float u = std::clamp((tx - tile.left + (drawn_w - tw) * 0.5f) / drawn_w, 0.0f, 1.0f);
            const float v = std::clamp((ty - tile.top + (drawn_h - th) * 0.5f) / drawn_h, 0.0f, 1.0f);
            const auto px = std::min(grid.width - 1, static_cast<uint32_t>(u * grid.width));
            const auto py = std::min(grid.height - 1, static_cast<uint32_t>(v * grid.height));
            const uint32_t color = grid.pbgra[py * grid.width + px];
            const float a = ((color >> 24) & 255u) / 255.0f;
            auto rgb = FromArgb(color);
            if (a > 0) rgb = {Unit(rgb.r / a), Unit(rgb.g / a), Unit(rgb.b / a)};
            const auto n = out.colors.count++;
            out.colors.colors[n] = rgb;
            out.alpha[n] = a;
        }
    }
    return out;
}

}
