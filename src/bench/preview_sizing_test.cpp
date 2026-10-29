// Quick Look size presets and the image header probe (quick_preview_sizing.h).
#include "../ui/quick_preview_sizing.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pulse::ui;

namespace {
int failures = 0, passes = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", what);
    (ok ? passes : failures)++;
}
int W(const RECT& r) { return r.right - r.left; }
int H(const RECT& r) { return r.bottom - r.top; }
bool Inside(const RECT& r, const RECT& work) {
    return r.left >= work.left && r.top >= work.top && r.right <= work.right && r.bottom <= work.bottom;
}

PreviewSizeInput Base(PreviewSizeMode mode) {
    PreviewSizeInput in;
    in.mode = mode;
    in.work = RECT{0, 0, 2560, 1392};   // 2560x1440 minus a 48 px taskbar
    in.centre = POINT{1280, 696};
    in.chrome_h = 120;
    in.scale = 1.5f;
    return in;
}

void Put24(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 3; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
}

void Push16Be(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }

std::vector<uint8_t> Jpeg(uint32_t w, uint32_t h, int orientation) {
    std::vector<uint8_t> v{0xFF, 0xD8};
    if (orientation) {
        // APP1 Exif, big-endian TIFF, IFD0 with one Orientation entry.
        std::vector<uint8_t> tiff{'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1,
                                  0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, uint8_t(orientation), 0, 0,
                                  0, 0, 0, 0};
        v.push_back(0xFF); v.push_back(0xE1);
        Push16Be(v, static_cast<uint32_t>(2 + 6 + tiff.size()));
        for (char c : {'E', 'x', 'i', 'f'}) v.push_back(uint8_t(c));
        v.push_back(0); v.push_back(0);
        v.insert(v.end(), tiff.begin(), tiff.end());
    }
    v.push_back(0xFF); v.push_back(0xDB); Push16Be(v, 4); v.push_back(0); v.push_back(0);  // DQT stub
    v.push_back(0xFF); v.push_back(0xC2); Push16Be(v, 11); v.push_back(8);
    Push16Be(v, h); Push16Be(v, w);
    v.push_back(3); v.push_back(0); v.push_back(0); v.push_back(0);
    return v;
}
} // namespace

int main() {
    {   // 适应内容: a landscape photo fills up to 90% of the work area, keeps its shape.
        auto in = Base(PreviewSizeMode::Fit);
        in.content_w = 4032; in.content_h = 3024;
        const RECT r = PreviewWindowRect(in);
        Check(Inside(r, in.work), "fit: inside the work area");
        Check(H(r) <= 1392 * 0.9 + 1 && H(r) >= 1392 * 0.9 - 2, "fit: a tall-enough photo uses 90% of the height");
        const double aspect = double(W(r)) / double(H(r) - in.chrome_h);
        Check(aspect > 1.32 && aspect < 1.35, "fit: the content area keeps 4:3");
    }
    {   // A small picture is shown at its real pixels, not blown up.
        auto in = Base(PreviewSizeMode::Fit);
        in.content_w = 900; in.content_h = 700;
        const RECT r = PreviewWindowRect(in);
        Check(W(r) == 900 && H(r) == 820, "fit: small content at real pixels");
    }
    {   // A tiny icon still gets the minimum window (480 x 320 DIP).
        auto in = Base(PreviewSizeMode::Fit);
        in.content_w = 32; in.content_h = 32;
        const RECT r = PreviewWindowRect(in);
        Check(W(r) == 720 && H(r) == 480, "fit: minimum size in DIP");
    }
    {   // A portrait photo is limited by the height.
        auto in = Base(PreviewSizeMode::Fit);
        in.content_w = 3024; in.content_h = 4032;
        const RECT r = PreviewWindowRect(in);
        Check(W(r) < H(r) && Inside(r, in.work), "fit: portrait window");
    }
    {   // No content size (documents, a picture not probed yet): 标准.
        auto in = Base(PreviewSizeMode::Fit);
        const RECT r = PreviewWindowRect(in);
        Check(W(r) == 1792 && H(r) == 974, "fit without content = standard 70%");
    }
    {
        const RECT r = PreviewWindowRect(Base(PreviewSizeMode::Large));
        Check(W(r) == 2253 && H(r) == 1225, "large: 88% of the work area");
        const RECT m = PreviewWindowRect(Base(PreviewSizeMode::Maximized));
        Check(W(m) == 2560 && H(m) == 1392, "maximized: the work area");
    }
    {   // A remembered (dragged) size wins over the preset and stays on screen.
        auto in = Base(PreviewSizeMode::Large);
        in.remembered_w = 1500; in.remembered_h = 1000;
        in.centre = POINT{2500, 1350};
        const RECT r = PreviewWindowRect(in);
        Check(W(r) == 1500 && H(r) == 1000 && Inside(r, in.work), "remembered size, pushed inside the work area");
    }
    {   // A secondary monitor left of the primary (negative coordinates).
        auto in = Base(PreviewSizeMode::Standard);
        in.work = RECT{-1920, 0, 0, 1040};
        in.centre = POINT{-960, 520};
        const RECT r = PreviewWindowRect(in);
        Check(Inside(r, in.work) && W(r) == 1344, "standard on a negative-coordinate monitor");
        in.work = RECT{0, 0, 0, 0};
        const RECT none = PreviewWindowRect(in);
        Check(W(none) == 0, "empty work area gives an empty rect");
    }

    uint32_t w = 0, h = 0;
    {
        const uint8_t png[24] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13,
                                 'I', 'H', 'D', 'R', 0, 0, 0x0F, 0xC0, 0, 0, 0x0B, 0xD0};
        Check(ProbeImageSize(png, sizeof(png), w, h) && w == 4032 && h == 3024, "png");
        Check(!ProbeImageSize(png, 20, w, h), "png: truncated header");
    }
    {
        const uint8_t gif[10] = {'G', 'I', 'F', '8', '9', 'a', 0x40, 0x01, 0xF0, 0x00};
        Check(ProbeImageSize(gif, sizeof(gif), w, h) && w == 320 && h == 240, "gif");
    }
    {
        uint8_t bmp[26] = {'B', 'M'};
        bmp[14] = 40; bmp[18] = 0x20; bmp[19] = 0x03;  // 800
        const int32_t top_down = -600;
        std::memcpy(bmp + 22, &top_down, 4);
        Check(ProbeImageSize(bmp, sizeof(bmp), w, h) && w == 800 && h == 600, "bmp (top-down)");
    }
    {
        uint8_t webp[30] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X'};
        Put24(webp + 24, 1919);
        Put24(webp + 27, 1079);
        Check(ProbeImageSize(webp, sizeof(webp), w, h) && w == 1920 && h == 1080, "webp VP8X");
        uint8_t lossy[30] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
        lossy[23] = 0x9D; lossy[24] = 0x01; lossy[25] = 0x2A;
        lossy[26] = 0x80; lossy[27] = 0x02; lossy[28] = 0xE0; lossy[29] = 0x01;
        Check(ProbeImageSize(lossy, sizeof(lossy), w, h) && w == 640 && h == 480, "webp VP8");
    }
    {
        auto plain = Jpeg(4000, 3000, 0);
        Check(ProbeImageSize(plain.data(), plain.size(), w, h) && w == 4000 && h == 3000, "jpeg");
        auto upright = Jpeg(4000, 3000, 1);
        Check(ProbeImageSize(upright.data(), upright.size(), w, h) && w == 4000 && h == 3000, "jpeg orientation 1");
        auto turned = Jpeg(4000, 3000, 6);
        Check(ProbeImageSize(turned.data(), turned.size(), w, h) && w == 3000 && h == 4000, "jpeg orientation 6 swaps");
        Check(!ProbeImageSize(turned.data(), turned.size() - 6, w, h), "jpeg: frame header cut off");
        const uint8_t junk[8] = {0xFF, 0xD8, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
        Check(!ProbeImageSize(junk, sizeof(junk), w, h), "jpeg: garbage after SOI");
        const uint8_t text[12] = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!'};
        Check(!ProbeImageSize(text, sizeof(text), w, h), "not an image");
        Check(!ProbeImageSize(nullptr, 0, w, h), "null buffer");
    }
    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
