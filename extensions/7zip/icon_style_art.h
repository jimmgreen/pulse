// icon_style_art.h - the 接管压缩包 icon styles drawn as canvas vectors for the settings preview.
// Same geometry and colours as icons/*.ico (all three styles rendered from archive_icon_styles_mockup.html),
// on the mockup's 64-unit grid placed at (x, y) with side `size`.
#pragma once
#include "association.h"
#include <pulse/model_builder.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace icon_art {

struct Format { uint32_t rgb; const char* label; };
inline constexpr Format k7z{0x6A4FB3, "7Z"}, kZip{0x1F7A87, "ZIP"}, kRar{0x8B4A9E, "RAR"};

inline uint32_t Mix(uint32_t rgb, uint32_t to, double t) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const double a = (rgb >> shift) & 0xFF, b = (to >> shift) & 0xFF;
        out |= static_cast<uint32_t>(std::lround(a + (b - a) * t)) << shift;
    }
    return out;
}
inline uint32_t Tint(uint32_t rgb, double t) { return Mix(rgb, 0xFFFFFF, t); }
inline uint32_t Shade(uint32_t rgb, double t) { return Mix(rgb, 0x000000, t); }
inline double Luminance(uint32_t rgb) {
    double sum = 0;
    const double weights[] = {0.2126, 0.7152, 0.0722};
    for (int i = 0; i < 3; ++i) {
        double v = ((rgb >> (16 - 8 * i)) & 0xFF) / 255.0;
        v = v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        sum += weights[i] * v;
    }
    return sum;
}
// Label and zipper colour on a coloured face: white, or a dark tone of the same hue on light colours.
inline uint32_t Ink(uint32_t rgb) { return Luminance(rgb) > 0.42 ? Shade(rgb, 0.62) : 0xFFFFFF; }
inline std::string Hex(uint32_t rgb) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%06X", rgb & 0xFFFFFF);
    return buffer;
}

// Maps the 64-unit grid onto the canvas.
struct Grid {
    double x, y, s;
    std::string N(double v) const {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", v);
        return buffer;
    }
    std::string P(double gx, double gy) const { return N(x + gx * s) + " " + N(y + gy * s); }
    // A rounded corner through control point (cx, cy) to (ex, ey); the host's path syntax has no Q.
    std::string Corner(double cx, double cy, double ex, double ey) const {
        const auto c = P(cx, cy);
        return " C" + c + " " + c + " " + P(ex, ey);
    }
    void Rect(pulse::sdk::CanvasBuilder& c, double gx, double gy, double w, double h, double r, uint32_t fill) const {
        c.Rect(x + gx * s, y + gy * s, w * s, h * s, r * s, Hex(fill));
    }
};

// Three teeth and a pull from (cx, y0), the shortened zipper of the new styles.
inline void Zipper(pulse::sdk::CanvasBuilder& c, const Grid& g, double cx, double y0, uint32_t teeth, uint32_t hole) {
    for (int i = 0; i < 3; ++i) g.Rect(c, i % 2 ? cx + 0.1 : cx - 3.5, y0 + i * 3.3, 3.4, 2.3, 0.75, teeth);
    const double py = y0 + 9.8;
    g.Rect(c, cx - 3.8, py, 7.6, 10.4, 3.8, teeth);
    g.Rect(c, cx - 1.3, py + 1.7, 2.6, 4.6, 1.3, hole);
}

// The format name as real text through the host's UI text renderer (LumaText): bold, centred on
// (cx, cy), capital height `cap`, within `max_w` (all in icon units). ScaledText sizes it in viewBox
// units, so the label scales with the icon and the three previews share one letter size.
inline void Label(pulse::sdk::CanvasBuilder& c, const Grid& g, double cx, double cy, double cap, double max_w,
                  uint32_t color, const char* text) {
    const double size = cap / 0.72;   // em size whose capitals are `cap` tall
    const double line = size * 1.5;
    c.ScaledText(g.x + (cx - max_w / 2) * g.s, g.y + (cy - line / 2) * g.s, max_w * g.s, line * g.s,
                 Hex(color), size * g.s, "bold", "center", text);
}

inline void Draw(pulse::sdk::CanvasBuilder& c, int style, const Format& f, double x, double y, double size) {
    const Grid g{x, y, size / 64.0};
    const uint32_t base = f.rgb, ink = Ink(base);
    if (style == association::kIconFolder) {
        c.Path(Hex(Shade(base, 0.22)), "none", 0,
               "M" + g.P(6, 14) + " C" + g.P(6, 10) + " " + g.P(6, 10) + " " + g.P(10, 10) + " L" + g.P(22.2, 10) + " L" + g.P(28.4, 15) +
               " L" + g.P(54, 15) + " C" + g.P(58, 15) + " " + g.P(58, 15) + " " + g.P(58, 19) + " L" + g.P(58, 22) + " L" + g.P(6, 22) + " Z");
        g.Rect(c, 6, 20, 52, 36, 4, base);
        Zipper(c, g, 32, 21.2, ink, base);
        Label(c, g, 32, 47.5, 9, 40, ink, f.label);
    } else if (style == association::kIconColor) {
        c.Path(Hex(base), "none", 0,
               "M" + g.P(16, 4) + " L" + g.P(38, 4) + " L" + g.P(52, 18) + " L" + g.P(52, 56) + g.Corner(52, 60, 48, 60) +
               " L" + g.P(16, 60) + g.Corner(12, 60, 12, 56) + " L" + g.P(12, 8) + g.Corner(12, 4, 16, 4) + " Z");
        c.Path(Hex(Tint(base, 0.45)), "none", 0,
               "M" + g.P(38, 4) + " L" + g.P(38, 14) + " C" + g.P(38, 18) + " " + g.P(38, 18) + " " + g.P(42, 18) + " L" + g.P(52, 18) + " Z");
        Zipper(c, g, 32, 12, ink, base);
        Label(c, g, 32, 46.5, 9, 32, ink, f.label);
    } else {
        // 「文档」: a white page, a zipper on a tinted track down the fold, the colour on a plate.
        c.Path("#FFFFFF", "#C9D0D8", 0.8 * g.s,
               "M" + g.P(17, 3.5) + " L" + g.P(39.5, 3.5) + " L" + g.P(52, 16) + " L" + g.P(52, 56.5) + g.Corner(52, 60.5, 48, 60.5) +
               " L" + g.P(17, 60.5) + g.Corner(13, 60.5, 13, 56.5) + " L" + g.P(13, 7.5) + g.Corner(13, 3.5, 17, 3.5) + " Z");
        c.Path("#E2E7EE", "none", 0,
               "M" + g.P(39.5, 3.5) + " L" + g.P(39.5, 12) + " C" + g.P(39.5, 16) + " " + g.P(39.5, 16) + " " + g.P(43.5, 16) + " L" + g.P(52, 16) + " Z");
        g.Rect(c, 29, 4.4, 8, 20.7, 1.4, Tint(base, 0.84));
        const uint32_t teeth = Shade(base, 0.06);
        for (int i = 0; i < 6; ++i) g.Rect(c, i % 2 ? 33.1 : 29.5, 5.2 + i * 3.3, 3.4, 2.3, 0.75, teeth);
        g.Rect(c, 29.2, 23.5, 7.6, 10.4, 3.8, teeth);
        g.Rect(c, 31.7, 25.2, 2.6, 4.6, 1.3, 0xFFFFFF);
        g.Rect(c, 5.5, 36.5, 36, 19, 4.5, base);
        Label(c, g, 23.5, 46, 8.5, 28, ink, f.label);
    }
}

// The settings card's illustration: 7Z, ZIP and RAR side by side at one size, so the three labels
// share one letter height (a larger 7Z made its label visibly bigger than ZIP's and RAR's).
inline pulse::sdk::ModelNode Preview(const std::string& id, int style) {
    pulse::sdk::CanvasBuilder canvas(id);
    canvas.ViewBox(150, 56).MinHeight(80);
    constexpr double kSide = 46, kGap = 4, kLeft = (150 - 3 * kSide - 2 * kGap) / 2;
    Draw(canvas, style, k7z, kLeft, 5, kSide);
    Draw(canvas, style, kZip, kLeft + kSide + kGap, 5, kSide);
    Draw(canvas, style, kRar, kLeft + 2 * (kSide + kGap), 5, kSide);
    return canvas.BuildNode();
}

} // namespace icon_art