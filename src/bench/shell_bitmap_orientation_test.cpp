#include "../preview_host/shell_bitmap.h"
#include <cstdio>
#include <cstring>

int main() {
    int failures = 0;
    for (const int depth : {24, 32}) {
        for (const int sign : {-1, 1}) {
            for (const int alpha : {0, 128, 255}) {
                if (depth == 24 && alpha != 255) continue;
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = 3;
                info.bmiHeader.biHeight = sign * 2;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = static_cast<WORD>(depth);
                void* pixels = nullptr;
                HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
                if (!bitmap || !pixels) { ++failures; continue; }
                const int pitch = 12;
                std::memset(pixels, 0, pitch * 2);
                for (int y = 0; y < 2; ++y) {
                    const int stored_y = sign < 0 ? y : 1 - y;
                    auto* row = static_cast<uint8_t*>(pixels) + stored_y * pitch;
                    for (int x = 0; x < 3; ++x) {
                        auto* p = row + x * (depth / 8);
                        p[0] = static_cast<uint8_t>(10 + y * 30 + x);
                        p[1] = 80;
                        p[2] = 240; // Forces straight-alpha normalization at alpha=128.
                        if (depth == 32) p[3] = static_cast<uint8_t>(alpha);
                    }
                }
                std::vector<uint8_t> out;
                UINT width = 0, height = 0, stride = 0;
                bool ok = pulse::preview::ShellBitmapToPbgra(bitmap, WTSAT_RGB, out, width, height, stride);
                ok = ok && width == 3 && height == 2 && stride == 12 && out.size() == 24;
                if (ok) for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) {
                    const auto* p = out.data() + y * stride + x * 4;
                    const int a = alpha == 0 ? 255 : alpha;
                    ok = ok && p[0] == ((10 + y * 30 + x) * a + 127) / 255 &&
                        p[1] == (80 * a + 127) / 255 && p[2] == (240 * a + 127) / 255 && p[3] == a;
                }
                std::printf("[%s] depth=%d top_down=%d alpha=%d\n", ok ? "PASS" : "FAIL", depth, sign < 0, alpha);
                failures += !ok;
                DeleteObject(bitmap);
            }
        }
    }
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
