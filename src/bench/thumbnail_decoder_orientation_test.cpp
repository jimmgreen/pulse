#include "preview_host_client.h"
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    pulse_test::Host host;
    if (!host.Start()) return 2;
    int failures = 0;
    int cases = 0;
    const unsigned char colors[4][3] = {{0,0,255}, {0,255,0}, {255,0,0}, {0,255,255}};
    const int corners[8][4] = {
        {0,1,2,3}, {1,0,3,2}, {3,2,1,0}, {2,3,0,1},
        {0,2,1,3}, {2,0,3,1}, {3,1,2,0}, {1,3,0,2}};
    for (const auto* suffix : {L".jpg", L".png", L".bmp", L".unknown"}) {
        for (int orientation = 1; orientation <= 8; ++orientation) {
            for (unsigned edge : {64u, 128u, 256u, 48u, 800u, 64u}) {
                const auto path = std::wstring(argv[1]) + L"\\orientation" +
                    std::to_wstring(orientation) + suffix;
                pulse_test::Result result;
                const bool received = host.Request(path, result, MAXDWORD, edge,
                    pulse::ipc::PreviewRequestKind::Content, pulse::ipc::kPreviewRequestFlagGrid);
                const unsigned expected_width = orientation >= 5 ? edge * 3 / 4 : edge;
                const unsigned expected_height = orientation >= 5 ? edge : edge * 3 / 4;
                const bool dimensions = received && result.response.width == expected_width &&
                    result.response.height == expected_height;
                bool pixels = received && !result.pixels.empty();
                if (pixels) {
                    for (unsigned y : {1u, 3u}) for (unsigned x : {1u, 3u}) {
                        const auto* actual = &result.pixels[(result.response.height * y / 4) *
                            result.response.stride + (result.response.width * x / 4) * 4];
                        const auto* expected = colors[corners[orientation - 1][
                            (y == 3 ? 2 : 0) + (x == 3 ? 1 : 0)]];
                        for (int channel = 0; channel < 3; ++channel)
                            pixels &= abs(int(actual[channel]) - int(expected[channel])) < 30;
                    }
                }
                std::printf("[%s] suffix=%ls EXIF=%d edge=%u actual=%ux%u expected=%ux%u corners=%d\n",
                    dimensions && pixels ? "PASS" : "FAIL", suffix, orientation, edge,
                    result.response.width, result.response.height, expected_width, expected_height, pixels);
                failures += !dimensions || !pixels;
                ++cases;
            }
        }
    }
    std::printf("DECODER_ORIENTATION_RESULT cases=%d failed=%d\n", cases, failures);
    return failures ? 1 : 0;
}
